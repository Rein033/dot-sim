//! `kode-sim bench`: how fast esp-emu runs a firmware on this computer.
//!
//! The firmware boots headless, as a review's does (no page, no window, no
//! serial port, the card slot empty), and runs for a number of seconds of
//! wall time once kemu has said HELLO. The speed is the emulated time it
//! went through in them over the wall time: esp-emu's own clock, which every
//! message on its co-simulation bus carries and the board's chips read
//! ([`crate::emu::Emu::now_ns`]), so it is the same whatever the app does
//! or prints. esp-emu 1.0 counts cycles, but says how many only when the
//! CPU faults: no cycles per second here.
//!
//! The CPU time is esp-emu's over its whole run, boot and stop included:
//! the process's children's, as the system counts them once they have gone
//! (getrusage), so the same on macOS and Linux.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::json;
use tokio::sync::{mpsc, watch};

use crate::bridge::{Bridge, Config, Limits, Msg, Page};
use crate::emu::{self, Launch};
use crate::proto;

pub struct Args {
    pub firmware: PathBuf,
    pub elf: Option<PathBuf>,
    /// What the summary calls it: the project's folder, or the image's file.
    pub what: String,
    pub seconds: u64,
    pub json: bool,
}

/// How long the firmware may take to say HELLO: a slow computer's boot,
/// kodeOS's included, with room to spare.
const BOOT_WAIT: Duration = Duration::from_secs(180);
/// How often the clock is read while it runs (a reset is noticed by it).
const POLL: Duration = Duration::from_millis(5);
/// How long a heartbeat may take at either end of the measurement.
const BEAT_WAIT: Duration = Duration::from_secs(2);

/// The emulated time a run has gone through, across the chip's resets:
/// esp-emu's clock starts again from 0 at each one.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Emulated {
    last: u64,
    ran: u64,
}

impl Emulated {
    /// Counting from `now`, the clock as it is.
    pub fn from(now: u64) -> Emulated {
        Emulated { last: now, ran: 0 }
    }

    /// The clock read again: what it moved since, or all of it after a reset.
    pub fn advance(&mut self, now: u64) {
        self.ran += if now >= self.last { now - self.last } else { now };
        self.last = now;
    }

    pub fn ns(&self) -> u64 {
        self.ran
    }
}

/// What a bench measured.
#[derive(Clone, Debug, PartialEq)]
pub struct Measured {
    /// The wall time it measured over, and the emulated time in it.
    pub wall: Duration,
    pub emulated: Duration,
    /// The screen updates kemu sent in it (FB_RECT, as the page gets them).
    pub frames: u64,
    /// The CPU time esp-emu used, user and system, over the whole of its
    /// run (boot and stop included), and that run's wall time.
    pub cpu: Duration,
    pub cpu_wall: Duration,
    /// From esp-emu's start to the firmware's first HELLO, and the
    /// emulated time in it.
    pub boot: Duration,
    pub boot_emulated: Duration,
}

/// `a / b`, or 0 when there is no `b`.
fn per(a: f64, b: Duration) -> f64 {
    if b.is_zero() {
        0.0
    } else {
        a / b.as_secs_f64()
    }
}

impl Measured {
    /// Emulated seconds per wall second: 1 is real time.
    pub fn ratio(&self) -> f64 {
        per(self.emulated.as_secs_f64(), self.wall)
    }

    /// Screen updates per wall second: what a page would have drawn.
    pub fn fps(&self) -> f64 {
        per(self.frames as f64, self.wall)
    }

    /// The cores esp-emu kept busy, on average over its run.
    pub fn cores(&self) -> f64 {
        per(self.cpu.as_secs_f64(), self.cpu_wall)
    }
}

/// The computer it ran on and the esp-emu it ran.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Host {
    pub cpu: String,
    pub cores: usize,
    pub esp_emu: String,
}

impl Host {
    /// This computer, and the esp-emu at `exe`.
    fn here(exe: &std::path::Path) -> Host {
        let version = std::process::Command::new(exe).arg("--version").output().ok().and_then(|o| {
            let said = String::from_utf8_lossy(&o.stdout).into_owned() + &String::from_utf8_lossy(&o.stderr);
            esp_emu_version(&said)
        });
        Host {
            cpu: cpu_model().unwrap_or_else(|| std::env::consts::ARCH.to_string()),
            cores: std::thread::available_parallelism().map_or(1, |n| n.get()),
            esp_emu: version.unwrap_or_else(|| "unknown".into()),
        }
    }
}

#[cfg(target_os = "macos")]
fn cpu_model() -> Option<String> {
    let name = c"machdep.cpu.brand_string";
    let mut buf = [0u8; 256];
    let mut len = buf.len();
    let rc = unsafe { libc::sysctlbyname(name.as_ptr(), buf.as_mut_ptr().cast(), &mut len, std::ptr::null_mut(), 0) };
    if rc != 0 {
        return None;
    }
    let s = String::from_utf8_lossy(&buf[..len]).trim_end_matches('\0').trim().to_string();
    (!s.is_empty()).then_some(s)
}

#[cfg(not(target_os = "macos"))]
fn cpu_model() -> Option<String> {
    cpuinfo_model(&std::fs::read_to_string("/proc/cpuinfo").ok()?)
}

/// The processor's name in Linux's /proc/cpuinfo: x86's "model name", or
/// an ARM board's "Model" or "Hardware".
#[cfg(any(test, not(target_os = "macos")))]
pub fn cpuinfo_model(cpuinfo: &str) -> Option<String> {
    let field = |key: &str| {
        cpuinfo.lines().find_map(|l| {
            let (k, v) = l.split_once(':')?;
            (k.trim() == key && !v.trim().is_empty()).then(|| v.trim().to_string())
        })
    };
    field("model name").or_else(|| field("Model")).or_else(|| field("Hardware"))
}

/// The version in `esp-emu --version`'s answer (its banner, then
/// "esp-emu 1.0.0").
pub fn esp_emu_version(out: &str) -> Option<String> {
    out.lines().rev().find_map(|l| l.trim().strip_prefix("esp-emu ")).map(|v| v.trim().to_string())
}

/// The CPU time of this process's children that have gone, user and system.
fn children_cpu() -> Duration {
    let mut u: libc::rusage = unsafe { std::mem::zeroed() };
    if unsafe { libc::getrusage(libc::RUSAGE_CHILDREN, &mut u) } != 0 {
        return Duration::ZERO;
    }
    let tv = |t: libc::timeval| Duration::new(t.tv_sec as u64, t.tv_usec as u32 * 1000);
    tv(u.ru_utime) + tv(u.ru_stime)
}

fn round(x: f64, places: i32) -> f64 {
    let k = 10f64.powi(places);
    (x * k).round() / k
}

/// The summary, for a person.
pub fn text(what: &str, host: &Host, m: &Measured) -> String {
    let s = |d: Duration| d.as_secs_f64();
    [
        format!("kode-sim bench: {what} on esp-emu {}", host.esp_emu),
        format!("  host     {}, {} cores", host.cpu, host.cores),
        format!("  speed    {:.2}× real time ({:.2} s emulated in {:.2} s)", m.ratio(), s(m.emulated), s(m.wall)),
        format!("  screen   {:.1} frames/s ({} from kemu)", m.fps(), m.frames),
        format!("  esp-emu  {:.1} s of CPU in {:.1} s ({:.2} of a core)", s(m.cpu), s(m.cpu_wall), m.cores()),
        format!("  boot     {:.1} s until kemu's HELLO ({:.2} s emulated)", s(m.boot), s(m.boot_emulated)),
    ]
    .join("\n")
}

/// The same, for a script: one JSON object on one line.
pub fn json(what: &str, host: &Host, m: &Measured) -> String {
    let s = |d: Duration| round(d.as_secs_f64(), 3);
    json!({
        "firmware": what,
        "host_cpu": host.cpu,
        "host_cores": host.cores,
        "esp_emu": host.esp_emu,
        "ratio": round(m.ratio(), 3),
        "emulated_s": s(m.emulated),
        "wall_s": s(m.wall),
        "frames": m.frames,
        "fps": round(m.fps(), 2),
        "cpu_s": s(m.cpu),
        "cpu_wall_s": s(m.cpu_wall),
        "boot_s": s(m.boot),
        "boot_emulated_s": s(m.boot_emulated),
    })
    .to_string()
}

/// The clock's next heartbeat: when it came, and what it said. The
/// measurement starts and ends on one, so wall and emulated time are read
/// together; without one in [`BEAT_WAIT`], the clock as it is.
async fn beat(bridge: &Bridge) -> (Instant, u64) {
    let was = bridge.sim_time_ns().unwrap_or(0);
    let deadline = Instant::now() + BEAT_WAIT;
    loop {
        let now = bridge.sim_time_ns().unwrap_or(0);
        if now != was || Instant::now() >= deadline {
            return (Instant::now(), now);
        }
        tokio::time::sleep(Duration::from_millis(1)).await;
    }
}

/// `kode-sim bench`: 0 when it measured, 1 when it could not (esp-emu did
/// not start, the firmware never said HELLO or crashed, a signal).
pub async fn command(a: Args) -> i32 {
    let exe = match emu::ensure_esp_emu() {
        Ok(exe) => exe,
        Err(e) => {
            eprintln!("kode-sim: {e}");
            return 1;
        }
    };
    let host = Host::here(&exe);
    let bridge = Bridge::new(Config {
        launch: Launch { firmware: a.firmware, elf: a.elf, forwards: vec![], restrict: false, save_state: false },
        http_port: 0,
        wifi: true,
        sd: None,
        sd_in: false,
    });
    // The page it would have: every frame counted, none skipped.
    let frames = Arc::new(AtomicU64::new(0));
    let (hello_tx, mut hello) = watch::channel(false);
    let (page, mut queue) =
        Page::with_limits(Limits { media: usize::MAX / 2, backlog: usize::MAX / 2, stuck: Duration::MAX });
    bridge.join(page);
    {
        let frames = frames.clone();
        tokio::spawn(async move {
            while let Some((m, _)) = queue.next().await {
                if let Msg::Bin(b) = m {
                    match b.get(2) {
                        Some(&proto::FB_RECT) => {
                            frames.fetch_add(1, Ordering::Relaxed);
                        }
                        Some(&proto::HELLO) => {
                            let _ = hello_tx.send(true);
                        }
                        _ => {}
                    }
                }
            }
        });
    }
    let (state_tx, mut states) = mpsc::unbounded_channel();
    bridge.on_state(state_tx);
    let mut stop = crate::run::stopped();

    let cpu_before = children_cpu();
    let started = Instant::now();
    eprintln!("kode-sim: booting {} ...", a.what);
    let measured = tokio::select! {
        m = measure(&bridge, a.seconds, &frames, &mut hello, started) => m,
        Some((state, detail)) = async {
            loop {
                match states.recv().await {
                    Some((s, d)) if s == "crashed" => return Some((s, d)),
                    Some(_) => {}
                    None => return None,
                }
            }
        } => Err(format!("the firmware {state} - {}", detail.lines().next().unwrap_or(""))),
        _ = stop.changed() => Err("stopped before the measurement ended".into()),
    };
    bridge.close().await;
    emu::stop_all().await;
    let cpu_wall = started.elapsed();
    let cpu = children_cpu().saturating_sub(cpu_before);
    match measured {
        Ok(mut m) => {
            m.cpu = cpu;
            m.cpu_wall = cpu_wall;
            if a.json {
                println!("{}", json(&a.what, &host, &m));
            } else {
                println!("{}", text(&a.what, &host, &m));
            }
            0
        }
        Err(e) => {
            eprintln!("kode-sim: {e}");
            1
        }
    }
}

/// Boot, wait for HELLO, then `seconds` of wall time between two heartbeats.
async fn measure(
    bridge: &Arc<Bridge>,
    seconds: u64,
    frames: &AtomicU64,
    hello: &mut watch::Receiver<bool>,
    started: Instant,
) -> Result<Measured, String> {
    bridge.start_headless().await?;
    if tokio::time::timeout(BOOT_WAIT, hello.wait_for(|h| *h)).await.is_err() {
        return Err(format!("the firmware said no HELLO in {} s: is it built for the simulator?", BOOT_WAIT.as_secs()));
    }
    let boot = started.elapsed();
    let boot_emulated = Duration::from_nanos(bridge.sim_time_ns().unwrap_or(0));
    eprintln!("kode-sim: up in {:.1} s; measuring for {seconds} s ...", boot.as_secs_f64());
    let (t0, now) = beat(bridge).await;
    let mut clock = Emulated::from(now);
    let frames0 = frames.load(Ordering::Relaxed);
    let span = Duration::from_secs(seconds);
    while t0.elapsed() < span {
        tokio::time::sleep(POLL.min(span.saturating_sub(t0.elapsed()))).await;
        clock.advance(bridge.sim_time_ns().unwrap_or(0));
    }
    let (t1, now) = beat(bridge).await;
    clock.advance(now);
    Ok(Measured {
        wall: t1 - t0,
        emulated: Duration::from_nanos(clock.ns()),
        frames: frames.load(Ordering::Relaxed) - frames0,
        cpu: Duration::ZERO,
        cpu_wall: Duration::ZERO,
        boot,
        boot_emulated,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn measured() -> Measured {
        Measured {
            wall: Duration::from_secs(20),
            emulated: Duration::from_millis(6_800),
            frames: 250,
            cpu: Duration::from_millis(21_300),
            cpu_wall: Duration::from_millis(25_000),
            boot: Duration::from_millis(3_300),
            boot_emulated: Duration::from_millis(1_100),
        }
    }

    fn host() -> Host {
        Host { cpu: "Intel(R) Core(TM) i5-14500T".into(), cores: 6, esp_emu: "1.0.0".into() }
    }

    #[test]
    fn the_emulated_time_adds_up_across_the_chip_s_resets() {
        let mut t = Emulated::from(5_000);
        assert_eq!(t.ns(), 0);
        t.advance(5_000); // no heartbeat since
        t.advance(9_000);
        assert_eq!(t.ns(), 4_000);
        t.advance(1_500); // reset: the clock started again from 0
        assert_eq!(t.ns(), 5_500);
        t.advance(2_000);
        assert_eq!(t.ns(), 6_000);
    }

    #[test]
    fn the_ratio_is_emulated_time_over_wall_time() {
        let m = measured();
        assert!((m.ratio() - 0.34).abs() < 1e-9, "{}", m.ratio());
        assert!((m.fps() - 12.5).abs() < 1e-9, "{}", m.fps());
        assert!((m.cores() - 0.852).abs() < 1e-9, "{}", m.cores());
    }

    #[test]
    fn nothing_measured_is_no_speed_rather_than_a_division_by_zero() {
        let m = Measured { wall: Duration::ZERO, cpu_wall: Duration::ZERO, ..measured() };
        assert_eq!((m.ratio(), m.fps(), m.cores()), (0.0, 0.0, 0.0));
    }

    #[test]
    fn the_processor_is_found_in_cpuinfo() {
        let x86 = "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) Core(TM) i5-14500T\n\
                   processor\t: 1\nmodel name\t: Intel(R) Core(TM) i5-14500T\n";
        assert_eq!(cpuinfo_model(x86).as_deref(), Some("Intel(R) Core(TM) i5-14500T"));
        let pi =
            "processor\t: 0\nBogoMIPS\t: 108.00\n\nHardware\t: BCM2835\nModel\t\t: Raspberry Pi 5 Model B Rev 1.0\n";
        assert_eq!(cpuinfo_model(pi).as_deref(), Some("Raspberry Pi 5 Model B Rev 1.0"));
        assert_eq!(cpuinfo_model("Hardware\t: BCM2835\n").as_deref(), Some("BCM2835"));
        assert_eq!(cpuinfo_model("processor\t: 0\nmodel name\t: \n"), None);
        assert_eq!(cpuinfo_model(""), None);
    }

    #[test]
    fn the_esp_emu_version_is_its_answer_s_last_word() {
        let banner =
            "\n    ESP32 series emulator · v1.0.0\n    https://github.com/espressif/esp-emulator\n\nesp-emu 1.0.0\n";
        assert_eq!(esp_emu_version(banner).as_deref(), Some("1.0.0"));
        assert_eq!(esp_emu_version("esp-emu 1.1.0-rc.1").as_deref(), Some("1.1.0-rc.1"));
        assert_eq!(esp_emu_version("command not found"), None);
    }

    #[test]
    fn the_summary_says_the_speed_first() {
        let t = text("my_app", &host(), &measured());
        let lines: Vec<&str> = t.lines().collect();
        assert_eq!(lines[0], "kode-sim bench: my_app on esp-emu 1.0.0");
        assert!(t.contains("Intel(R) Core(TM) i5-14500T, 6 cores"), "{t}");
        assert!(t.contains("0.34× real time (6.80 s emulated in 20.00 s)"), "{t}");
        assert!(t.contains("12.5 frames/s (250 from kemu)"), "{t}");
        assert!(t.contains("21.3 s of CPU in 25.0 s (0.85 of a core)"), "{t}");
        assert!(t.contains("3.3 s until kemu's HELLO (1.10 s emulated)"), "{t}");
    }

    #[test]
    fn the_json_has_the_same_numbers() {
        let j: serde_json::Value = serde_json::from_str(&json("my_app", &host(), &measured())).unwrap();
        assert_eq!(j["firmware"], "my_app");
        assert_eq!(j["host_cpu"], "Intel(R) Core(TM) i5-14500T");
        assert_eq!(j["host_cores"], 6);
        assert_eq!(j["esp_emu"], "1.0.0");
        assert_eq!(j["ratio"], 0.34);
        assert_eq!(j["emulated_s"], 6.8);
        assert_eq!(j["wall_s"], 20.0);
        assert_eq!(j["frames"], 250);
        assert_eq!(j["fps"], 12.5);
        assert_eq!(j["cpu_s"], 21.3);
        assert_eq!(j["cpu_wall_s"], 25.0);
        assert_eq!(j["boot_s"], 3.3);
        assert_eq!(j["boot_emulated_s"], 1.1);
        assert!(!json("my_app", &host(), &measured()).contains('\n'));
    }
}

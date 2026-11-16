//! kode-sim: a Kode Dot on the desk.
//!
//! One binary, every command: `build` builds a
//! project with kemu in it (or, `--board`, as for the board); `run` builds
//! and boots it in esp-emu with the board's chips in-process and serves the
//! page; `install` puts an app in a running kodeOS through its console, or
//! `--mailbox` into app_pending as an Arduino upload does; `review` puts an
//! app through a scenario, headless; `bench` measures how fast esp-emu runs
//! it here.

mod audio;
mod bench;
mod bridge;
mod build;
mod chips;
mod emu;
mod install;
mod proto;
mod review;
mod run;
mod sdcard;
mod serial;
mod usbhost;
mod web;
mod window;

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "kode-sim", version, about = "A Kode Dot on the desk")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Build a project for the simulator.
    Build {
        /// An ESP-IDF project: a kode-sdk app or kodeOS
        project: PathBuf,
        /// Build against this kode-sdk checkout instead of the pinned one
        #[arg(long, value_name = "DIR")]
        sdk: Option<PathBuf>,
        /// Build it as for the board: kemu left out, the image a Kode Dot
        /// (or a factory kodeOS's mailbox) takes
        #[arg(long)]
        board: bool,
        /// Also print what it built on stdout, as one JSON object:
        /// `merged_bin` (what `run --firmware` boots), `elf`, `app_bin`
        #[arg(long)]
        json: bool,
    },
    /// Build an app and install it in the kodeOS that `kode-sim run` is running.
    Install {
        /// An ESP-IDF project to build, or an app image already built (.bin)
        project: PathBuf,
        /// Build against this kode-sdk checkout instead of the pinned one
        #[arg(long, value_name = "DIR")]
        sdk: Option<PathBuf>,
        /// The simulator whose page is on this port (default: the one started last)
        #[arg(long)]
        port: Option<u16>,
        /// Put the app in the app_pending partition and reset the chip, as
        /// an Arduino/esptool upload does: kodeOS adopts it as it boots.
        /// No console needed, whatever is running
        #[arg(long)]
        mailbox: bool,
        /// Build it as for the board (no kemu): what a factory kodeOS takes
        #[arg(long)]
        board: bool,
    },
    /// Put an app through a scenario in a Kode Dot of its own, headless: a
    /// verdict, screenshots and the log (apps.kode.diy's review).
    ///
    /// Exits 0 when every check passed, 1 when one failed, 2 when the review
    /// could not run at all.
    Review {
        /// The app built for the simulator (kode-sim build's <app>.bin), or its project to build
        app: PathBuf,
        /// The scenario (JSON); without one, the checks every review makes
        scenario: Option<PathBuf>,
        /// Where the verdict goes
        #[arg(long, default_value = "review")]
        out: PathBuf,
        /// kodeOS built for the simulator (a folder with merged-binary.bin
        /// and kodeos.elf), or a KodeOS checkout [default: $KODE_SIM_KODEOS]
        #[arg(long, env = "KODE_SIM_KODEOS")]
        kodeos: PathBuf,
    },
    /// Build a project, boot it and open the Kode Dot in a window of its own.
    ///
    /// Or boot a firmware already built, with --firmware (and --elf).
    Run {
        /// An ESP-IDF project: a kode-sdk app or kodeOS
        #[arg(required_unless_present = "firmware", conflicts_with = "firmware")]
        project: Option<PathBuf>,
        /// Build against this kode-sdk checkout instead of the pinned one
        #[arg(long, value_name = "DIR", conflicts_with = "firmware")]
        sdk: Option<PathBuf>,
        /// A bootable image already built: bootloader, partition table and app
        #[arg(long, value_name = "MERGED.bin")]
        firmware: Option<PathBuf>,
        /// The app's ELF, for esp-emu's symbols
        #[arg(long, value_name = "APP.elf", requires = "firmware")]
        elf: Option<PathBuf>,
        /// The page's port (0: one the Mac picks, said as it starts)
        #[arg(long, default_value_t = 8765)]
        port: u16,
        /// Open no window: the page stays at the address it prints
        #[arg(long)]
        no_open: bool,
        /// Make 127.0.0.1:PORT on the Mac reach the Kode Dot's port (default
        /// the same number) - a server the app opens; repeatable
        #[arg(long, value_name = "PORT[:KODE_PORT]", value_parser = run::parse_forward)]
        forward: Vec<(u16, u16)>,
        /// A Kode Dot whose C5 never comes up: no WiFi, the radio fails
        #[arg(long)]
        no_wifi: bool,
        /// A network with no way out but DNS, as a review's ("network":
        /// "none"): the clock comes from the P4's RTC, not the internet
        #[arg(long)]
        no_internet: bool,
        /// The microSD's card image, made (1 GB, FAT32) if it does not exist
        /// [default: <cache>/sd/card.img]
        #[arg(long, value_name = "CARD.img")]
        sd: Option<PathBuf>,
        /// Start with the slot empty (the page's "SD insertada" puts the card in)
        #[arg(long)]
        no_sd: bool,
        /// Exit non-zero when the firmware crashes, instead of serving the
        /// page so it can be restarted
        #[arg(long)]
        exit_on_crash: bool,
        /// Keep the flash when esp-emu stops, so what was installed is there
        /// next time: written back into MERGED.bin with --firmware; for a
        /// PROJECT, into a flash of its own in the cache (beside its build),
        /// kept until the build changes
        #[arg(long)]
        save_state: bool,
    },
    /// Build a project, boot it headless and measure how fast esp-emu runs
    /// it here: emulated time over wall time.
    ///
    /// Or measure a firmware already built, with --firmware (and --elf).
    Bench {
        /// An ESP-IDF project: a kode-sdk app or kodeOS
        #[arg(required_unless_present = "firmware", conflicts_with = "firmware")]
        project: Option<PathBuf>,
        /// Build against this kode-sdk checkout instead of the pinned one
        #[arg(long, value_name = "DIR", conflicts_with = "firmware")]
        sdk: Option<PathBuf>,
        /// A bootable image already built: bootloader, partition table and app
        #[arg(long, value_name = "MERGED.bin")]
        firmware: Option<PathBuf>,
        /// The app's ELF, for esp-emu's symbols
        #[arg(long, value_name = "APP.elf", requires = "firmware")]
        elf: Option<PathBuf>,
        /// How long to measure for, in seconds of wall time, once it has booted
        #[arg(long, default_value_t = 20, value_parser = clap::value_parser!(u64).range(1..=3600))]
        seconds: u64,
        /// Print the summary as one JSON object, for a script
        #[arg(long)]
        json: bool,
    },
}

fn main() -> ExitCode {
    match Cli::parse().cmd {
        Cmd::Build { project, sdk, board, json } => {
            ExitCode::from(build::command(&project, sdk.as_deref(), board, json) as u8)
        }
        Cmd::Install { project, sdk, port, mailbox, board } => {
            ExitCode::from(install::command(&project, sdk.as_deref(), port, mailbox, board) as u8)
        }
        Cmd::Review { app, scenario, out, kodeos } => {
            let rt = tokio::runtime::Runtime::new().expect("a tokio runtime");
            let code = rt.block_on(async {
                // review stops itself on SIGTERM, SIGINT and SIGHUP (128 + it).
                let code = review::command(&app, scenario.as_deref(), &out, &kodeos).await;
                // An esp-emu outlives the process that started it.
                emu::stop_all().await;
                code
            });
            std::process::exit(code);
        }
        Cmd::Bench { project, sdk, firmware, elf, seconds, json } => {
            let (firmware, elf, what) = match (project, firmware) {
                (Some(project), _) => match build::built(&project, sdk.as_deref(), false) {
                    Ok(fw) => (fw.merged_bin, Some(fw.elf), name_of(&project)),
                    Err(code) => return ExitCode::from(code as u8),
                },
                (None, Some(firmware)) => {
                    let what = name_of(&firmware);
                    (firmware, elf, what)
                }
                (None, None) => unreachable!("clap requires one"),
            };
            let rt = tokio::runtime::Runtime::new().expect("a tokio runtime");
            let code = rt.block_on(bench::command(bench::Args { firmware, elf, what, seconds, json }));
            ExitCode::from(code as u8)
        }
        Cmd::Run {
            project,
            sdk,
            firmware,
            elf,
            port,
            no_open,
            forward,
            no_wifi,
            no_internet,
            sd,
            no_sd,
            exit_on_crash,
            save_state,
        } => {
            // Held until kode-sim exits: the saved flash is this run's.
            let mut _flash_lock = None;
            let (firmware, elf) = match (project, firmware) {
                (Some(project), _) => match build::built(&project, sdk.as_deref(), false) {
                    // The build's own image is made again by the next build.
                    Ok(fw) if save_state => match run::saved_flash(&fw.merged_bin) {
                        Ok((flash, lock)) => {
                            _flash_lock = Some(lock);
                            (flash, Some(fw.elf))
                        }
                        Err(e) => {
                            eprintln!("kode-sim: could not keep a flash beside {}: {e}", fw.merged_bin.display());
                            return ExitCode::FAILURE;
                        }
                    },
                    Ok(fw) => (fw.merged_bin, Some(fw.elf)),
                    Err(code) => return ExitCode::from(code as u8),
                },
                (None, Some(firmware)) => (firmware, elf),
                (None, None) => unreachable!("clap requires one"),
            };
            let args = run::Args {
                firmware,
                elf,
                port,
                open: !no_open,
                forwards: forward,
                wifi: !no_wifi,
                restrict: no_internet,
                sd: sd.unwrap_or_else(sdcard::default_path),
                sd_in: !no_sd,
                exit_on_crash,
                save_state,
            };
            let rt = tokio::runtime::Runtime::new().expect("a tokio runtime");
            let code = rt.block_on(run::run(args));
            // Not waiting on the thread that reads the terminal.
            std::process::exit(code);
        }
    }
}

/// A path's last part, as a summary names what it ran.
fn name_of(p: &std::path::Path) -> String {
    let p = std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
    p.file_name().map_or_else(|| p.display().to_string(), |n| n.to_string_lossy().into_owned())
}

//! The virtual chips, driven the way the kode-sdk drivers drive them.
//! (firmware/tests/test_models.c, carried over test for test.)

use kodedot_chips::*;

fn r1(c: &mut impl Chip, ctx: &mut Ctx, reg: u8) -> u8 {
    let mut v = [0xEE];
    assert!(c.read(ctx, reg, &mut v));
    v[0]
}

fn w1(c: &mut impl Chip, ctx: &mut Ctx, reg: u8, v: u8) {
    assert!(c.write(ctx, reg, &[v]));
}

fn fresh<C: Chip + Default>() -> (C, Ctx) {
    let mut ctx = Ctx::default();
    let mut c = C::default();
    c.reset(&mut ctx);
    ctx.events.clear();
    (c, ctx)
}

fn last_led(ctx: &Ctx) -> Option<[u8; 3]> {
    ctx.events.iter().rev().find_map(|e| match e {
        Event::Led(rgb) => Some(*rgb),
        _ => None,
    })
}

fn last_haptic(ctx: &Ctx) -> Option<(u8, u16)> {
    ctx.events.iter().rev().find_map(|e| match e {
        Event::Haptic { strength, ms } => Some((*strength, *ms)),
        _ => None,
    })
}

fn pin(ctx: &Ctx, n: u8) -> Option<bool> {
    ctx.events.iter().rev().find_map(|e| match e {
        Event::Pin { pin, level } if *pin == n => Some(*level),
        _ => None,
    })
}

#[test]
fn ktd2026_red() {
    let (mut c, mut ctx) = fresh::<Ktd2026>();
    // kdrv_led: wake, ISET1 (G) ISET2 (B) ISET3 (R), then LED_EN. r=255 is code 39.
    w1(&mut c, &mut ctx, 0x00, 0x18);
    w1(&mut c, &mut ctx, 0x06, 0);
    w1(&mut c, &mut ctx, 0x07, 0);
    w1(&mut c, &mut ctx, 0x08, 39);
    w1(&mut c, &mut ctx, 0x04, 0x01 << 4);
    assert_eq!(last_led(&ctx), Some([255, 0, 0]));
}

#[test]
fn ktd2026_sleep_is_dark() {
    let (mut c, mut ctx) = fresh::<Ktd2026>();
    w1(&mut c, &mut ctx, 0x00, 0x18);
    w1(&mut c, &mut ctx, 0x06, 9); // green, about 64
    w1(&mut c, &mut ctx, 0x04, 0x01);
    let [r, g, b] = last_led(&ctx).unwrap();
    assert!(r == 0 && (60..=68).contains(&g) && b == 0);
    w1(&mut c, &mut ctx, 0x04, 0x00);
    w1(&mut c, &mut ctx, 0x00, 0x08); // asleep
    assert_eq!(last_led(&ctx), Some([0, 0, 0]));
}

#[test]
fn tca_button_press_pulls_int_low_until_read() {
    let (mut c, mut ctx) = fresh::<Tca6408a>();
    assert_eq!(r1(&mut c, &mut ctx, 0x00), 0xFF); // nothing pressed: pulled up
    assert_eq!(pin(&ctx, PIN_IOEXP_INT), Some(true));
    c.set_buttons(&mut ctx, BTN_BOT | BTN_UP);
    assert_eq!(pin(&ctx, PIN_IOEXP_INT), Some(false)); // INT asserted (active low)
    let input = r1(&mut c, &mut ctx, 0x00);
    assert_eq!(input & 0x01, 0); // P0 bottom, active low
    assert_eq!(input & 0x02, 0); // P1 pad-up
    assert_eq!(input & 0x1C, 0x1C); // left, down, right released
    assert_eq!(pin(&ctx, PIN_IOEXP_INT), Some(true)); // reading the input clears INT
}

#[test]
fn tca_touch_irq_on_p6() {
    let (mut c, mut ctx) = fresh::<Tca6408a>();
    c.set_touch_irq(&mut ctx, true);
    assert_eq!(pin(&ctx, PIN_IOEXP_INT), Some(false));
    assert_eq!(r1(&mut c, &mut ctx, 0x00) & 0x40, 0);
    c.set_touch_irq(&mut ctx, false);
    assert_eq!(r1(&mut c, &mut ctx, 0x00) & 0x40, 0x40);
}

#[test]
fn tca_registers_keep_writes() {
    let (mut c, mut ctx) = fresh::<Tca6408a>();
    assert_eq!(r1(&mut c, &mut ctx, 0x01), 0xFF);
    assert_eq!(r1(&mut c, &mut ctx, 0x02), 0x00);
    assert_eq!(r1(&mut c, &mut ctx, 0x03), 0xFF);
    w1(&mut c, &mut ctx, 0x03, 0x7F); // P7 an output: the panel's VCI_EN
    w1(&mut c, &mut ctx, 0x01, 0x80);
    assert_eq!(r1(&mut c, &mut ctx, 0x03), 0x7F);
    assert_eq!(r1(&mut c, &mut ctx, 0x01), 0x80);
    assert_eq!(r1(&mut c, &mut ctx, 0x00) & 0x80, 0x80); // an output reads its latch
}

#[test]
fn cst820_touch_reported() {
    let (mut c, mut ctx) = fresh::<Cst820>();
    let (mut id, mut fr) = ([0u8; 3], [0u8; 6]);
    assert!(c.read(&mut ctx, 0xA7, &mut id));
    assert_eq!(id[0] & 0xF0, 0xB0);
    assert!(c.read(&mut ctx, 0x01, &mut fr));
    assert_eq!(fr[1], 0);
    c.set_touch(true, 300, 401);
    assert!(c.read(&mut ctx, 0x01, &mut fr));
    assert_eq!(fr[1], 1);
    assert_eq!(((fr[2] as u16 & 0x0F) << 8) | fr[3] as u16, 300);
    assert_eq!(((fr[4] as u16 & 0x0F) << 8) | fr[5] as u16, 401);
    c.set_touch(false, 300, 401);
    assert!(c.read(&mut ctx, 0x01, &mut fr));
    assert_eq!(fr[1], 0);
}

#[test]
fn cst820_irq_ctl_after_reset() {
    let (mut c, mut ctx) = fresh::<Cst820>();
    assert_eq!(r1(&mut c, &mut ctx, 0xFA), 0x60); // touch and change, as from the factory
}

fn aw86233_in_rtp() -> (Aw86233, Ctx) {
    let (mut c, mut ctx) = fresh::<Aw86233>();
    assert_eq!(r1(&mut c, &mut ctx, 0x57), 0x23);
    assert_eq!(r1(&mut c, &mut ctx, 0x69), 0x30);
    w1(&mut c, &mut ctx, 0x46, 0x02); // out of standby, 12 kHz
    w1(&mut c, &mut ctx, 0x07, 128); // gain: half
    w1(&mut c, &mut ctx, 0x08, 0x01); // RTP
    w1(&mut c, &mut ctx, 0x09, 0x01); // GO
    assert_eq!(r1(&mut c, &mut ctx, 0x3F) & 0x0F, 0x08);
    (c, ctx)
}

#[test]
fn aw86233_play_emits_haptic() {
    let (mut c, mut ctx) = aw86233_in_rtp();
    let pcm: Vec<u8> = (0..64).map(|i| if i & 1 == 1 { 127i8 as u8 } else { -127i8 as u8 }).collect();
    assert!(c.write(&mut ctx, 0x32, &pcm));
    let (strength, _) = last_haptic(&ctx).unwrap();
    assert!((45..=55).contains(&strength)); // full swing at half gain
    w1(&mut c, &mut ctx, 0x09, 0x02); // STOP
    assert_eq!(last_haptic(&ctx).map(|h| h.0), Some(0));
}

#[test]
fn aw86233_fifo_fills_and_drains() {
    let (mut c, mut ctx) = aw86233_in_rtp();
    let pcm = [0u8; 64];
    for _ in 0..64 {
        assert!(c.write(&mut ctx, 0x32, &pcm)); // 64 bursts of 64 = 4096 bytes, at t=0
    }
    assert_ne!(r1(&mut c, &mut ctx, 0x01) & 0x08, 0); // almost full
    ctx.now_us = 1_000_000; // a second at 12 kHz drains it
    assert_eq!(r1(&mut c, &mut ctx, 0x01) & 0x08, 0);
}

#[test]
fn bq_identity() {
    let (mut c, mut ctx) = fresh::<Bq25896>();
    assert_eq!((r1(&mut c, &mut ctx, 0x14) >> 3) & 7, 0); // PN: bq25896
}

#[test]
fn bq_status_follows_usb() {
    let (mut c, mut ctx) = fresh::<Bq25896>();
    ctx.battery = Battery { percent: 80, usb: true };
    let st = r1(&mut c, &mut ctx, 0x0B);
    assert!(st >> 5 != 0 && st & 0x04 != 0); // VBUS, power good
    assert_ne!(r1(&mut c, &mut ctx, 0x11) & 0x80, 0); // VBUS_GD
    ctx.battery = Battery { percent: 80, usb: false };
    let st = r1(&mut c, &mut ctx, 0x0B);
    assert!(st >> 5 == 0 && st & 0x04 == 0);
    let batv = (r1(&mut c, &mut ctx, 0x0E) & 0x7F) as u32;
    assert!((3701..4200).contains(&(2304 + 20 * batv)));
}

#[test]
fn bq_batfet_off_on_battery_powers_off() {
    let (mut c, mut ctx) = fresh::<Bq25896>();
    ctx.battery = Battery { percent: 50, usb: false };
    let v = r1(&mut c, &mut ctx, 0x09) | 0x20;
    w1(&mut c, &mut ctx, 0x09, v);
    assert!(ctx.events.contains(&Event::PowerOff));
}

#[test]
fn bq_batfet_with_usb_does_nothing() {
    let (mut c, mut ctx) = fresh::<Bq25896>();
    ctx.battery = Battery { percent: 50, usb: true };
    let v = r1(&mut c, &mut ctx, 0x09) | 0x20;
    w1(&mut c, &mut ctx, 0x09, v);
    assert!(!ctx.events.contains(&Event::PowerOff));
}

#[test]
fn cw2217_reports_percent() {
    let (mut c, mut ctx) = fresh::<Cw2217>();
    let mut w = [0u8; 2];
    assert_eq!(r1(&mut c, &mut ctx, 0x00), 0xA0);
    assert_eq!(r1(&mut c, &mut ctx, 0xA7) & 0x0C, 0x0C);
    ctx.battery = Battery { percent: 15, usb: false };
    assert!(c.read(&mut ctx, 0x04, &mut w));
    assert_eq!(w[0], 15);
    assert!(c.read(&mut ctx, 0x02, &mut w));
    let mv = (((u16::from_be_bytes(w) & 0x3FFF) as u32) * 5 + 8) / 16;
    assert!((3301..4250).contains(&mv));
    w1(&mut c, &mut ctx, 0x0B, 0x80 | 5); // UPDATE_FLAG + alert, kept
    assert_eq!(r1(&mut c, &mut ctx, 0x0B), 0x80 | 5);
}

/// kodeOS's power_ao turns the cell's voltage into the percentage it shows,
/// through the board's curve (kbsp_cell.h); this is that function.
fn kodeos_soc_from_ocv(mv: i32) -> u32 {
    if mv <= CELL_OCV_MV[0] as i32 {
        return 0;
    }
    for i in 1..CELL_OCV_MV.len() {
        if mv < CELL_OCV_MV[i] as i32 {
            let span = CELL_OCV_MV[i] as i32 - CELL_OCV_MV[i - 1] as i32;
            let rise = CELL_OCV_PCT[i] as i32 - CELL_OCV_PCT[i - 1] as i32;
            let into = mv - CELL_OCV_MV[i - 1] as i32;
            return (CELL_OCV_PCT[i - 1] as i32 + (into * rise + span / 2) / span) as u32;
        }
    }
    100
}

/// The slider's percentage is the one kodeOS shows once it has settled: the
/// cell's voltage (the gauge's VCELL, 312.5 uV a step) is on kodeOS's curve.
#[test]
fn the_cell_voltage_reads_as_the_slider_on_kodeos_curve() {
    let (mut c, mut ctx) = fresh::<Cw2217>();
    let mut bad = vec![];
    for pct in 0..=100u8 {
        ctx.battery = Battery { percent: pct, usb: false };
        let mut w = [0u8; 2];
        assert!(c.read(&mut ctx, 0x02, &mut w));
        let mv = (u16::from_be_bytes(w) as u32 * 5 / 16) as i32;
        if kodeos_soc_from_ocv(mv) != pct as u32 {
            bad.push((pct, mv, kodeos_soc_from_ocv(mv)));
        }
    }
    assert!(bad.is_empty(), "slider %, mV, kodeOS shows: {bad:?}");
}

#[test]
fn es8311_ids() {
    let (mut c, mut ctx) = fresh::<Es8311>();
    assert_eq!(r1(&mut c, &mut ctx, 0xFD), 0x83);
    assert_eq!(r1(&mut c, &mut ctx, 0xFE), 0x11);
}

#[test]
fn es8311_volume_stored() {
    let (mut c, mut ctx) = fresh::<Es8311>();
    w1(&mut c, &mut ctx, 0x32, 0xBF);
    assert_eq!(r1(&mut c, &mut ctx, 0x32), 0xBF);
    assert_eq!(c.out_gain_q8(), 256); // 0 dB
    w1(&mut c, &mut ctx, 0x32, 0xBF - 12); // -6 dB
    assert!((126..=130).contains(&c.out_gain_q8()));
    w1(&mut c, &mut ctx, 0x31, 0x60); // DAC muted
    assert_eq!(c.out_gain_q8(), 0);
}

// ---- The board: what esp-emu's I2C transactions reach -----------------------

#[test]
fn a_register_read_is_one_transaction_and_a_split_read_continues() {
    let mut board = Board::new();
    // The gauge's VERSION, then CHIP_ID 0xA7.. of the touch in two pieces, as
    // the legacy driver splits a read: the second carries no register.
    assert_eq!(board.i2c(0x64, &[0x00], 1, false), Some(vec![0xA0]));
    let first = board.i2c(0x15, &[0xA7], 2, false).unwrap();
    let rest = board.i2c(0x15, &[], 1, true).unwrap();
    assert_eq!([first, rest].concat(), vec![0xB7, 0x00, 0x01]);
}

#[test]
fn a_write_reaches_its_chip_and_a_probe_finds_only_the_board() {
    let mut board = Board::new();
    assert_eq!(board.i2c(0x30, &[0x00, 0x18], 0, false), Some(vec![]));
    assert_eq!(board.i2c(0x30, &[0x00], 1, false), Some(vec![0x18]));
    assert_eq!(board.i2c(0x20, &[], 0, false), Some(vec![])); // probe: ACK
    assert_eq!(board.i2c(0x6A, &[], 0, false), Some(vec![])); // the IMU
    assert_eq!(board.i2c(0x0D, &[], 0, false), None); // nothing at the QMC5883L's address
    let mut addrs = board.addrs();
    addrs.sort();
    assert_eq!(addrs, vec![0x15, 0x18, 0x20, 0x2C, 0x30, 0x5A, 0x64, 0x6A, 0x6B]);
}

#[test]
fn the_board_takes_the_page_s_buttons_touch_and_battery() {
    let mut board = Board::new();
    board.set_buttons(BTN_LEFT);
    assert_eq!(board.i2c(0x20, &[0x00], 1, false), Some(vec![!(1u8 << 2)]));
    board.set_touch(true, 10, 20);
    assert_eq!(board.i2c(0x15, &[0x02], 1, false), Some(vec![1]));
    assert_eq!(board.i2c(0x20, &[0x00], 1, false).unwrap()[0] & 0x40, 0); // INT on P6
    board.set_battery(42, false);
    assert_eq!(board.i2c(0x64, &[0x04], 1, false), Some(vec![42]));
}

#[test]
fn what_the_chips_do_comes_out_as_events() {
    let mut board = Board::new();
    board.take_events();
    board.i2c(0x30, &[0x00, 0x18], 0, false); // KTD2026 awake
    board.i2c(0x30, &[0x08, 39], 0, false); // red at full scale
    board.i2c(0x30, &[0x04, 0x01 << 4], 0, false);
    assert_eq!(board.take_events(), vec![Event::Led([255, 0, 0])]);
    assert!(board.take_events().is_empty());
}

#[test]
fn the_codec_s_volume_comes_out_when_it_changes() {
    let mut board = Board::new();
    board.take_events();
    board.i2c(0x18, &[0x32, 0xBF], 0, false); // 0 dB
    assert_eq!(board.take_events(), vec![Event::Gain(256)]);
    board.i2c(0x18, &[0x32, 0xBF], 0, false); // the same again: nothing new
    assert!(board.take_events().is_empty());
    board.i2c(0x18, &[0x31, 0x60], 0, false); // DAC muted
    assert_eq!(board.take_events(), vec![Event::Gain(0)]);
}

// ---- QMI8658A ----------------------------------------------------------------

fn imu_up() -> (Qmi8658a, Ctx) {
    let (mut c, mut ctx) = fresh::<Qmi8658a>();
    // kdrv_qmi8658_create(): sensors off, CTRL1, +-8 g and +-512 dps at 896.8 Hz, no filter, the CTRL9 handshake, both on.
    w1(&mut c, &mut ctx, 0x08, 0x00);
    w1(&mut c, &mut ctx, 0x02, 0x58);
    w1(&mut c, &mut ctx, 0x03, 0x23);
    w1(&mut c, &mut ctx, 0x04, 0x53);
    w1(&mut c, &mut ctx, 0x06, 0x00);
    w1(&mut c, &mut ctx, 0x09, 0x80);
    w1(&mut c, &mut ctx, 0x08, 0x03);
    (c, ctx)
}

fn imu_burst(c: &mut Qmi8658a, ctx: &mut Ctx) -> [i32; 7] {
    let mut raw = [0u8; 14];
    assert!(c.read(ctx, 0x33, &mut raw));
    let w = |i: usize| i16::from_le_bytes([raw[i], raw[i + 1]]) as i32;
    [w(0), w(2), w(4), w(6), w(8), w(10), w(12)]
}

#[test]
fn qmi8658a_identity_and_configuration_read_back() {
    let (mut c, mut ctx) = fresh::<Qmi8658a>();
    assert_eq!(r1(&mut c, &mut ctx, 0x00), 0x05);
    assert_eq!(r1(&mut c, &mut ctx, 0x02), 0x20, "CTRL1 resets big-endian with no auto-increment");
    let (mut c, mut ctx) = imu_up();
    for (reg, v) in [(0x02, 0x58), (0x03, 0x23), (0x04, 0x53), (0x06, 0x00), (0x09, 0x80), (0x08, 0x03)] {
        assert_eq!(r1(&mut c, &mut ctx, reg), v, "register {reg:#04x}");
    }
    assert_eq!(c.addr(), 0x6A);
}

#[test]
fn qmi8658a_at_rest_flat_reads_one_g_on_z_in_the_scale_the_driver_chose() {
    let (mut c, mut ctx) = imu_up();
    let [t, ax, ay, az, gx, gy, gz] = imu_burst(&mut c, &mut ctx);
    assert_eq!((ax, ay, az), (0, 0, 4096), "+-8 g is 4096 LSB/g");
    assert_eq!((gx, gy, gz), (0, 0, 0));
    assert_eq!(t, 30 * 256, "30.0 C in 8.8 fixed point");
}

#[test]
fn qmi8658a_converts_motion_with_each_scale_and_clips_to_sixteen_bits() {
    let (mut c, mut ctx) = imu_up();
    ctx.motion.accel_mg = [-500, 250, 8500];
    ctx.motion.gyro_mdps = [90_000, -45_000, 600_000];
    let [_, ax, ay, az, gx, gy, gz] = imu_burst(&mut c, &mut ctx);
    assert_eq!((ax, ay), (-2048, 1024));
    assert_eq!(az, 32767, "8.5 g is past +-8 g: clipped");
    assert_eq!((gx, gy), (90 * 64, -45 * 64), "+-512 dps is 64 LSB/dps");
    assert_eq!(gz, 32767, "600 dps is past +-512: clipped");
    w1(&mut c, &mut ctx, 0x03, 0x03); // +-2 g
    ctx.motion.accel_mg = [0, 0, 1000];
    assert_eq!(imu_burst(&mut c, &mut ctx)[3], 16384);
}

#[test]
fn qmi8658a_a_sensor_that_is_off_reads_zero() {
    let (mut c, mut ctx) = imu_up();
    ctx.motion.gyro_mdps = [10_000; 3];
    w1(&mut c, &mut ctx, 0x08, 0x01); // accelerometer alone
    let d = imu_burst(&mut c, &mut ctx);
    assert_eq!((d[3], d[4], d[5], d[6]), (4096, 0, 0, 0));
    w1(&mut c, &mut ctx, 0x08, 0x00);
    assert_eq!(imu_burst(&mut c, &mut ctx), [0; 7], "the part at rest");
}

#[test]
fn qmi8658a_left_big_endian_without_auto_increment_a_burst_is_one_byte_repeated() {
    let (mut c, mut ctx) = fresh::<Qmi8658a>();
    w1(&mut c, &mut ctx, 0x08, 0x03);
    let mut raw = [0u8; 6];
    assert!(c.read(&mut ctx, 0x33, &mut raw));
    assert!(raw.iter().all(|&b| b == raw[0]), "what the driver's read-back of CTRL1 is there to catch");
}

#[test]
fn qmi8658a_ctrl9_commands_report_done_on_statusint_until_acknowledged() {
    let (mut c, mut ctx) = imu_up();
    assert_eq!(r1(&mut c, &mut ctx, 0x2D) & 0x80, 0);
    w1(&mut c, &mut ctx, 0x0A, 0x04); // RST_FIFO
    assert_eq!(r1(&mut c, &mut ctx, 0x2D) & 0x80, 0x80);
    w1(&mut c, &mut ctx, 0x0A, 0x00); // acknowledge
    assert_eq!(r1(&mut c, &mut ctx, 0x2D) & 0x80, 0);
}

#[test]
fn qmi8658a_self_test_ends_after_its_stimulus_and_leaves_the_shift_for_the_driver_to_judge() {
    for (reg, st, lsb_per_unit, floor) in [(0x03u8, 0x23u8, 2048.0, 0.2), (0x04, 0x53, 16.0, 300.0)] {
        let (mut c, mut ctx) = imu_up();
        w1(&mut c, &mut ctx, 0x08, 0x00);
        w1(&mut c, &mut ctx, reg, st | 0x80);
        assert_eq!(r1(&mut c, &mut ctx, 0x2D) & 1, 0, "not yet");
        ctx.now_us += 3_000;
        assert_eq!(r1(&mut c, &mut ctx, 0x2D) & 1, 1);
        w1(&mut c, &mut ctx, reg, st); // the driver clears the trigger ...
        assert_eq!(r1(&mut c, &mut ctx, 0x2D) & 1, 0, "... and the flag falls");
        let mut dv = [0u8; 6]; // ... and reads what moved
        assert!(c.read(&mut ctx, 0x51, &mut dv));
        let shift = i16::from_le_bytes([dv[0], dv[1]]) as f64 / lsb_per_unit;
        assert!(shift > floor, "shift {shift} must pass the driver's floor {floor}");
        assert_eq!(dv[0..2], dv[2..4]);
    }
}

#[test]
fn qmi8658a_fifo_fills_at_a_kilohertz_and_gives_out_the_oldest_sample() {
    let (mut c, mut ctx) = imu_up();
    w1(&mut c, &mut ctx, 0x08, 0x01);
    w1(&mut c, &mut ctx, 0x14, 0x00);
    w1(&mut c, &mut ctx, 0x0A, 0x04);
    w1(&mut c, &mut ctx, 0x0A, 0x00);
    w1(&mut c, &mut ctx, 0x14, 0x0D);
    let mut cnt = [0u8; 2];
    assert!(c.read(&mut ctx, 0x15, &mut cnt));
    assert_eq!(cnt, [0, 0], "empty when armed");
    ctx.now_us += 20_000;
    assert!(c.read(&mut ctx, 0x15, &mut cnt));
    assert_eq!(cnt[0], 60, "20 samples are 60 words");
    // The driver reads the FIFO with auto-increment off: every byte is FIFO_DATA.
    w1(&mut c, &mut ctx, 0x02, 0x18);
    let mut s = [0u8; 6];
    assert!(c.read(&mut ctx, 0x17, &mut s));
    assert_eq!(i16::from_le_bytes([s[4], s[5]]), 4096);
    ctx.now_us += 1_000_000;
    assert!(c.read(&mut ctx, 0x15, &mut cnt));
    assert_eq!(u16::from_le_bytes([cnt[0], cnt[1] & 3]), 128 * 3 - 3, "128 samples at most, one taken");
}

// ---- QMC5883P ----------------------------------------------------------------

fn mag_up() -> (Qmc5883p, Ctx) {
    let (mut c, mut ctx) = fresh::<Qmc5883p>();
    // kdrv_qmc5883p_create(): the signs, then the range, then the mode
    w1(&mut c, &mut ctx, 0x29, 0x06);
    w1(&mut c, &mut ctx, 0x0B, 0x00);
    w1(&mut c, &mut ctx, 0x0A, 0x07); // continuous, 50 Hz (standby(false))
    (c, ctx)
}

fn mag_read(c: &mut Qmc5883p, ctx: &mut Ctx) -> [i32; 3] {
    let mut raw = [0u8; 6];
    assert!(c.read(ctx, 0x01, &mut raw));
    [0, 2, 4].map(|i| i16::from_le_bytes([raw[i], raw[i + 1]]) as i32)
}

#[test]
fn qmc5883p_identity_is_at_zero_not_the_l_parts_layout() {
    let (mut c, mut ctx) = fresh::<Qmc5883p>();
    assert_eq!(r1(&mut c, &mut ctx, 0x00), 0x80);
    assert_eq!(c.addr(), 0x2C);
}

#[test]
fn qmc5883p_nothing_is_ready_in_suspend_and_one_conversion_after_the_mode_is_set() {
    let (mut c, mut ctx) = fresh::<Qmc5883p>();
    w1(&mut c, &mut ctx, 0x0A, 0x00);
    ctx.now_us += 100_000;
    assert_eq!(r1(&mut c, &mut ctx, 0x09), 0, "kdrv parks the part suspended");
    w1(&mut c, &mut ctx, 0x0A, 0x07);
    assert_eq!(r1(&mut c, &mut ctx, 0x09) & 1, 0, "the first conversion is not done");
    ctx.now_us += 21_000;
    assert_eq!(r1(&mut c, &mut ctx, 0x09), 1);
}

#[test]
fn qmc5883p_field_comes_out_as_milligauss_at_the_default_range() {
    let (mut c, mut ctx) = mag_up();
    ctx.now_us += 25_000;
    ctx.motion.mag_mg = [-1234, 250, -380];
    assert_eq!(mag_read(&mut c, &mut ctx), [-1234, 250, -380], "+-30 G is 1000 LSB/G: a count is a milligauss");
}

#[test]
fn qmc5883p_a_field_past_the_range_sets_overflow_and_the_range_bits_change_the_scale() {
    let (mut c, mut ctx) = mag_up();
    ctx.now_us += 25_000;
    ctx.motion.mag_mg = [31_000, 0, 0];
    assert_eq!(r1(&mut c, &mut ctx, 0x09), 0x03, "ready, and overflowed");
    ctx.motion.mag_mg = [29_000, 0, 0];
    assert_eq!(r1(&mut c, &mut ctx, 0x09), 0x01);
    w1(&mut c, &mut ctx, 0x0B, 0x08); // +-8 G
    ctx.motion.mag_mg = [1000, -2000, 0];
    assert_eq!(mag_read(&mut c, &mut ctx), [3750, -7500, 0]);
    ctx.motion.mag_mg = [9000, 0, 0];
    assert_eq!(r1(&mut c, &mut ctx, 0x09) & 2, 2);
}

#[test]
fn qmc5883p_self_test_shifts_the_next_data_then_parks_the_part() {
    let (mut c, mut ctx) = mag_up();
    ctx.now_us += 25_000;
    let before = mag_read(&mut c, &mut ctx);
    w1(&mut c, &mut ctx, 0x0B, 0x40);
    ctx.now_us += 10_000;
    let on = mag_read(&mut c, &mut ctx);
    // kdrv: delta = on - before, then X negated (datasheet 7.3): all three come out positive.
    assert!(-(on[0] - before[0]) > 0 && on[1] - before[1] > 0 && on[2] - before[2] > 0);
    assert_eq!(r1(&mut c, &mut ctx, 0x0A) & 3, 0, "the part drops to suspend once the self-test's data is out");
    assert_eq!(r1(&mut c, &mut ctx, 0x0B) & 0x40, 0, "and the bit clears itself");
    w1(&mut c, &mut ctx, 0x0A, 0x00);
    w1(&mut c, &mut ctx, 0x0A, 0x07);
    ctx.now_us += 25_000;
    assert_eq!(mag_read(&mut c, &mut ctx), before, "back to what it measures");
}

#[test]
fn qmc5883p_soft_reset_puts_it_back_as_powered() {
    let (mut c, mut ctx) = mag_up();
    w1(&mut c, &mut ctx, 0x0B, 0x80);
    assert_eq!(r1(&mut c, &mut ctx, 0x0A), 0);
    assert_eq!(r1(&mut c, &mut ctx, 0x00), 0x80);
}

#[test]
fn the_board_answers_at_the_two_motion_addresses_and_takes_the_motion_the_page_sets() {
    let mut b = Board::new();
    let addrs = b.addrs();
    assert!(addrs.contains(&0x6A) && addrs.contains(&0x2C));
    assert!(!addrs.contains(&0x0D), "the QMC5883L's address stays a NACK: kdrv_qmc5883p_stray() looks there");
    assert_eq!(b.i2c(0x6A, &[0x00], 1, false), Some(vec![0x05]));
    b.set_motion(Motion { accel_mg: [1000, 0, 0], ..Motion::default() });
    b.i2c(0x6A, &[0x02, 0x58], 0, false);
    b.i2c(0x6A, &[0x03, 0x23], 0, false);
    b.i2c(0x6A, &[0x08, 0x01], 0, false);
    let d = b.i2c(0x6A, &[0x33], 14, false).unwrap();
    assert_eq!(i16::from_le_bytes([d[2], d[3]]), 4096);
}

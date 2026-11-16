//! The Kode Dot's I2C chips, made virtual behind unchanged drivers.
//!
//! Each chip is a register file plus the least behaviour its kode-sdk driver
//! relies on - the identity it checks, the status it polls, what it does on
//! a write. Chips are plain logic: they reach the outside only through a
//! [`Ctx`] - the time, the battery they share, and the [`Event`]s they raise
//! (the LED's colour, a buzz, a pin they drive, the board switching off).
//!
//! [`Board`] puts the nine on one bus, as esp-emu's I2C transactions reach
//! them, and takes what a page changes: buttons, a finger, the battery.

mod aw86233;
mod board;
mod cst820;
mod es8311;
mod ktd2026;
mod power;
mod qmc5883p;
mod qmi8658a;
mod tca6408a;

pub use aw86233::Aw86233;
pub use board::Board;
pub use cst820::Cst820;
pub use es8311::Es8311;
pub use ktd2026::Ktd2026;
pub use power::{Bq25896, Cw2217};
pub use qmc5883p::Qmc5883p;
pub use qmi8658a::Qmi8658a;
pub use tca6408a::Tca6408a;

/// The P4 pin the expander's INT reaches, active low (kbsp_pins.h).
pub const PIN_IOEXP_INT: u8 = 6;
/// The sixth button, on a P4 pin of its own, active low.
pub const PIN_BUTTON_TOP: u8 = 35;

/// Buttons as the page sends them (kemu_proto.h's KEMU_BTN_*).
pub const BTN_TOP: u8 = 1 << 0;
pub const BTN_BOT: u8 = 1 << 1;
pub const BTN_LEFT: u8 = 1 << 2;
pub const BTN_RIGHT: u8 = 1 << 3;
pub const BTN_UP: u8 = 1 << 4;
pub const BTN_DOWN: u8 = 1 << 5;

/// The cell's open-circuit voltage against its charge (kbsp_cell.h).
pub const CELL_OCV_MV: [u16; 13] = [3300, 3500, 3600, 3680, 3740, 3800, 3850, 3920, 3980, 4040, 4100, 4150, 4200];
pub const CELL_OCV_PCT: [u8; 13] = [0, 5, 11, 19, 30, 42, 53, 65, 75, 84, 90, 96, 100];

/// What a chip makes happen outside the bus.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    /// The LED's colour, whenever it changes.
    Led([u8; 3]),
    /// How hard the motor is driven (0-100) and for how long it has samples.
    Haptic { strength: u8, ms: u16 },
    /// A pin a chip drives on the P4, such as the expander's INT.
    Pin { pin: u8, level: bool },
    /// The BATFET opened on battery: the board is off.
    PowerOff,
    /// The codec's DAC volume and mute as a gain, 256 = 0 dB, 0 = muted,
    /// whenever it changes.
    Gain(i32),
}

/// The battery the charger and the gauge share, as the page sets it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Battery {
    pub percent: u8,
    pub usb: bool,
}

impl Default for Battery {
    fn default() -> Self {
        Battery { percent: 80, usb: true }
    }
}

impl Battery {
    /// The cell's voltage for this percentage, on the board's own curve, at
    /// the middle of the voltages that read as it: kodeOS shows what the
    /// slider says once its smoothing has caught up.
    pub fn mv(&self) -> u32 {
        let pct = self.percent.min(100) as u32;
        if pct <= CELL_OCV_PCT[0] as u32 {
            return CELL_OCV_MV[0] as u32;
        }
        for i in 1..CELL_OCV_PCT.len() {
            if pct <= CELL_OCV_PCT[i] as u32 {
                let span = (CELL_OCV_MV[i] - CELL_OCV_MV[i - 1]) as u32;
                let rise = (CELL_OCV_PCT[i] - CELL_OCV_PCT[i - 1]) as u32;
                return CELL_OCV_MV[i - 1] as u32 + ((pct - CELL_OCV_PCT[i - 1] as u32) * span + rise / 2) / rise;
            }
        }
        CELL_OCV_MV[CELL_OCV_MV.len() - 1] as u32
    }
}

/// What the motion sensors measure, in the board's own axes (x right, y toward the
/// top, z out of the glass), as the page sets it. At rest face up the
/// accelerometer reads +1 g on z: it measures the push that holds it up.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Motion {
    /// Acceleration, milli-g.
    pub accel_mg: [i32; 3],
    /// Rotation, milli-degrees per second.
    pub gyro_mdps: [i32; 3],
    /// The magnetic field, milligauss.
    pub mag_mg: [i32; 3],
    /// The die's temperature, tenths of a degree.
    pub temp_dc: i32,
}

impl Default for Motion {
    /// Flat on a desk, face up, its top toward north, in a field like Valencia's
    /// (25 uT horizontal, 38 uT down).
    fn default() -> Self {
        Motion { accel_mg: [0, 0, 1000], gyro_mdps: [0; 3], mag_mg: [0, 250, -380], temp_dc: 300 }
    }
}

/// The world as a chip sees it.
#[derive(Debug, Default)]
pub struct Ctx {
    /// The emulated time, in microseconds.
    pub now_us: u64,
    pub battery: Battery,
    pub motion: Motion,
    /// What the chips raised, oldest first, for whoever drains it.
    pub events: Vec<Event>,
}

/// One chip on the bus. A false from `write` or `read` is a NACK.
pub trait Chip {
    fn addr(&self) -> u8;
    fn reset(&mut self, ctx: &mut Ctx);
    /// Write `v` from `reg` on.
    fn write(&mut self, ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool;
    /// Read `v.len()` bytes from `reg` on.
    fn read(&mut self, ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool;
}

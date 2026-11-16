//! The charger (BQ25896) and the fuel gauge (CW2217), sharing one battery.
//!
//! The battery is what the page's slider and USB switch say ([`Ctx::battery`]).
//! The charger's status and ADC registers, and the gauge's SOC and VCELL,
//! are worked out from it on every read; everything else is a register file
//! that keeps what the drivers write, so power_ao's read-back of its policy
//! holds.
//!
//! Opening the BATFET (REG09 bit 5) with no USB is how the board turns itself
//! off - the `sleep` verb - and here it is the one way to [`Event::PowerOff`].

use crate::{Chip, Ctx, Event};

// ---- BQ25896 ----------------------------------------------------------------

const BQ_REG_COUNT: usize = 0x15;
const BQ_BATFET: usize = 0x09;
const BQ_BATFET_DIS: u8 = 0x20;
const BQ_RESET: [u8; BQ_REG_COUNT] = [
    0x08, 0x06, 0x11, 0x1A, 0x08, 0x22, 0x5E, 0x9D, 0x03, 0x44, 0x93, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02,
];

/// Bits the chip clears itself: CONV_START, WD_RST, FORCE_ICO, PUMPX, REG_RST.
fn bq_self_clearing(reg: usize) -> u8 {
    match reg {
        0x02 => 0x80,
        0x03 => 0x40,
        0x09 => 0x83,
        0x14 => 0x80,
        _ => 0x00,
    }
}

pub struct Bq25896 {
    reg: [u8; BQ_REG_COUNT],
}

impl Default for Bq25896 {
    fn default() -> Self {
        Bq25896 { reg: BQ_RESET }
    }
}

impl Bq25896 {
    fn live(&self, ctx: &Ctx, reg: usize) -> u8 {
        let b = ctx.battery;
        let vb = b.mv();
        match reg {
            0x0B => {
                // VBUS, CHRG, PG
                if !b.usb {
                    return 0x00;
                }
                0x20 | if b.percent < 100 { 0x10 } else { 0x18 } | 0x04
            }
            0x0C => 0x00, // no faults
            0x0E => ((vb - 2304) / 20) as u8,
            0x0F => ((if b.usb && vb < 3700 { 3700 } else { vb } - 2304) / 20) as u8,
            0x10 => 0x40, // TS, mid-range
            0x11 => {
                if b.usb {
                    0x80 | ((5000 - 2600) / 100) as u8
                } else {
                    0x00
                }
            }
            0x12 => {
                if b.usb && b.percent < 100 {
                    10
                } else {
                    0
                }
            } // 500 mA
            0x13 => 0x00,
            _ => self.reg[reg],
        }
    }
}

impl Chip for Bq25896 {
    fn addr(&self) -> u8 {
        0x6B
    }

    fn reset(&mut self, _ctx: &mut Ctx) {
        *self = Self::default();
    }

    fn write(&mut self, ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            let r = reg as usize + i;
            if r >= BQ_REG_COUNT {
                return false;
            }
            if (0x0B..=0x13).contains(&r) {
                continue; // status and ADC
            }
            if r == 0x14 {
                if b & 0x80 != 0 {
                    self.reg = BQ_RESET; // REG_RST
                }
                continue;
            }
            self.reg[r] = b & !bq_self_clearing(r);
            if r == BQ_BATFET && b & BQ_BATFET_DIS != 0 && !ctx.battery.usb {
                ctx.events.push(Event::PowerOff);
            }
        }
        true
    }

    fn read(&mut self, ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        for (i, b) in v.iter_mut().enumerate() {
            let r = reg as usize + i;
            if r >= BQ_REG_COUNT {
                return false;
            }
            *b = self.live(ctx, r);
        }
        true
    }
}

// ---- CW2217 -----------------------------------------------------------------

/// 25 C, in the gauge's 0.5 C steps from -40 C.
const CW_TEMP_25C: u8 = 25 * 2 + 80;

pub struct Cw2217 {
    reg: [u8; 256],
}

impl Default for Cw2217 {
    fn default() -> Self {
        let mut reg = [0; 256];
        reg[0x00] = 0xA0; // VERSION: CW221x
        reg[0x08] = 0xF0; // CONFIG: asleep at power-on
        reg[0xA6] = 100; // SOH
        reg[0xA7] = 0x0C; // IC_STATE: ready
        Cw2217 { reg }
    }
}

fn cw_read_only(r: u8) -> bool {
    r <= 0x07 || r == 0x0E || r == 0x0F || (0xA4..=0xA9).contains(&r) || r == 0xAB
}

impl Cw2217 {
    fn live(&self, ctx: &Ctx, r: u8) -> u8 {
        let vcell = ctx.battery.mv() * 16 / 5;
        match r {
            0x02 => (vcell >> 8) as u8,
            0x03 => vcell as u8,
            0x04 => ctx.battery.percent.min(100),
            0x05 => 0x00,
            0x06 => CW_TEMP_25C,
            _ => self.reg[r as usize],
        }
    }
}

impl Chip for Cw2217 {
    fn addr(&self) -> u8 {
        0x64
    }

    fn reset(&mut self, _ctx: &mut Ctx) {
        *self = Self::default();
    }

    fn write(&mut self, _ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            let r = reg.wrapping_add(i as u8);
            if !cw_read_only(r) {
                self.reg[r as usize] = b;
            }
        }
        true
    }

    fn read(&mut self, ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        for (i, b) in v.iter_mut().enumerate() {
            *b = self.live(ctx, reg.wrapping_add(i as u8));
        }
        true
    }
}

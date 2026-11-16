//! The magnetometer, QMC5883P, as kdrv_qmc5883p drives it.
//!
//! Not register-compatible with the QMC5883L it is named after: the chip id
//! is at 0x00, the data at 0x01..=0x06 (X, Y, Z, low byte first), the status
//! at 0x09. The driver writes the axis signs (0x29), the range (CTRL2) and
//! the mode (CTRL1), then reads STATUS for DRDY before it trusts the data.
//!
//! What the sensor measures is [`Motion`](crate::Motion)'s field, in
//! milligauss on the board's axes: it comes out in the range CTRL2 chose, as
//! counts clipped to 16 bits, and a field outside the range sets the
//! overflow flag instead of lying. In suspend nothing converts; in
//! continuous mode (50 Hz) DRDY is up one conversion after the mode was set.
//! The self-test (CTRL2 bit 6) adds a stimulus to the next data and puts the
//! part back to suspend, as the datasheet's does.

use crate::{Chip, Ctx};

const REG_CHIP_ID: u8 = 0x00;
const REG_X_LSB: u8 = 0x01;
const REG_Z_MSB: u8 = 0x06;
const REG_STATUS: u8 = 0x09;
const REG_CTRL1: u8 = 0x0A;
const REG_CTRL2: u8 = 0x0B;
const REG_SIGN: u8 = 0x29;

const CHIP_ID: u8 = 0x80;
const STATUS_DRDY: u8 = 0x01;
const STATUS_OVFL: u8 = 0x02;
const MODE_MASK: u8 = 0x03;
const MODE_SUSPEND: u8 = 0x00;
const MODE_CONTINUOUS: u8 = 0x03;
const CTRL2_SELF_TEST: u8 = 0x40;
const CTRL2_SOFT_RESET: u8 = 0x80;
/// One conversion at 50 Hz, microseconds.
const CONVERSION_US: u64 = 20_000;
/// What the self-test adds, milligauss: the datasheet's arithmetic subtracts X the other way round.
const ST_MG: [i32; 3] = [-1500, 1500, 1500];

pub struct Qmc5883p {
    reg: [u8; 256],
    /// When the mode last became continuous.
    since: Option<u64>,
    stimulus: bool,
}

impl Default for Qmc5883p {
    fn default() -> Self {
        let mut reg = [0; 256];
        reg[REG_CHIP_ID as usize] = CHIP_ID;
        Qmc5883p { reg, since: None, stimulus: false }
    }
}

impl Qmc5883p {
    fn r(&self, reg: u8) -> u8 {
        self.reg[reg as usize]
    }

    /// LSB per gauss for CTRL2's RNG (bits 3:2): +-30, +-12, +-8, +-2 G.
    fn lsb_per_g(&self) -> i64 {
        [1000, 2500, 3750, 15000][((self.r(REG_CTRL2) >> 2) & 3) as usize]
    }

    /// The limit of the range, milligauss.
    fn range_mg(&self) -> i64 {
        [30000, 12000, 8000, 2000][((self.r(REG_CTRL2) >> 2) & 3) as usize]
    }

    fn converting(&self, ctx: &Ctx) -> bool {
        self.r(REG_CTRL1) & MODE_MASK == MODE_CONTINUOUS
            && self.since.is_some_and(|t| ctx.now_us.saturating_sub(t) >= CONVERSION_US)
    }

    fn field(&self, ctx: &Ctx) -> [i32; 3] {
        let mut f = ctx.motion.mag_mg;
        if self.stimulus {
            for i in 0..3 {
                f[i] = f[i].saturating_add(ST_MG[i]);
            }
        }
        f
    }

    fn overflowed(&self, ctx: &Ctx) -> bool {
        self.field(ctx).iter().any(|&v| (v as i64).abs() > self.range_mg())
    }

    fn one(&mut self, ctx: &Ctx, reg: u8) -> u8 {
        match reg {
            REG_STATUS => {
                let mut s = 0;
                if self.converting(ctx) {
                    s |= STATUS_DRDY;
                    if self.overflowed(ctx) {
                        s |= STATUS_OVFL;
                    }
                }
                s
            }
            REG_X_LSB..=REG_Z_MSB => {
                let i = (reg - REG_X_LSB) as usize;
                let counts = (self.field(ctx)[i / 2] as i64 * self.lsb_per_g() / 1000)
                    .clamp(i16::MIN as i64, i16::MAX as i64) as i16;
                let b = counts.to_le_bytes()[i % 2];
                if reg == REG_Z_MSB && self.stimulus {
                    // The stimulus is over once its data has been read: the part parks itself.
                    self.stimulus = false;
                    self.reg[REG_CTRL2 as usize] &= !CTRL2_SELF_TEST;
                    self.reg[REG_CTRL1 as usize] = (self.r(REG_CTRL1) & !MODE_MASK) | MODE_SUSPEND;
                    self.since = None;
                }
                b
            }
            r => self.r(r),
        }
    }

    fn put(&mut self, ctx: &Ctx, reg: u8, v: u8) {
        match reg {
            REG_CHIP_ID | REG_STATUS | REG_X_LSB..=REG_Z_MSB => {}
            REG_CTRL1 => {
                self.reg[reg as usize] = v;
                if v & MODE_MASK == MODE_CONTINUOUS {
                    if self.since.is_none() {
                        self.since = Some(ctx.now_us);
                    }
                } else {
                    self.since = None;
                }
            }
            REG_CTRL2 => {
                if v & CTRL2_SOFT_RESET != 0 {
                    *self = Self::default();
                    return;
                }
                self.reg[reg as usize] = v;
                self.stimulus = v & CTRL2_SELF_TEST != 0;
            }
            REG_SIGN => self.reg[reg as usize] = v,
            r => self.reg[r as usize] = v,
        }
    }
}

impl Chip for Qmc5883p {
    fn addr(&self) -> u8 {
        0x2C
    }

    fn reset(&mut self, _ctx: &mut Ctx) {
        *self = Self::default();
    }

    fn write(&mut self, ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            self.put(ctx, reg.wrapping_add(i as u8), b);
        }
        true
    }

    fn read(&mut self, ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        for (i, b) in v.iter_mut().enumerate() {
            *b = self.one(ctx, reg.wrapping_add(i as u8));
        }
        true
    }
}

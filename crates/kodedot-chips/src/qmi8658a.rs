//! The accelerometer and gyroscope, QMI8658A, as kdrv_qmi8658 drives it.
//!
//! What the driver relies on: WHO_AM_I and the revision byte; CTRL1's
//! endianness and register auto-increment (a part left big-endian with no
//! auto-increment turns a 14-byte burst into one byte, so both are modelled);
//! the scales in CTRL2 and CTRL3 and the enables in CTRL7; one burst from
//! TEMP_L holding the temperature, the acceleration and the rotation of the
//! same instant; the CTRL9 handshake on STATUSINT bit 7 (CTRL8 bit 7); the
//! self-test, whose end shows on STATUSINT bit 0 and whose result is in the
//! dV registers; and the FIFO the haptic witness drains.
//!
//! What the sensors measure is [`Motion`](crate::Motion): the page sets it, in
//! the board's own axes, and every read converts it with the scale the driver
//! chose, clipped to 16 bits as the part does.

use crate::{Chip, Ctx};

const REG_WHO_AM_I: u8 = 0x00;
const REG_REVISION: u8 = 0x01;
const REG_CTRL1: u8 = 0x02;
const REG_CTRL2: u8 = 0x03;
const REG_CTRL3: u8 = 0x04;
const REG_CTRL7: u8 = 0x08;
const REG_CTRL8: u8 = 0x09;
const REG_CTRL9: u8 = 0x0A;
const REG_FIFO_CTRL: u8 = 0x14;
const REG_FIFO_CNT: u8 = 0x15;
const REG_FIFO_STATUS: u8 = 0x16;
const REG_FIFO_DATA: u8 = 0x17;
const REG_STATUSINT: u8 = 0x2D;
const REG_STATUS0: u8 = 0x2E;
const REG_TEMP_L: u8 = 0x33;
const REG_GZ_H: u8 = 0x40;
const REG_DVX_L: u8 = 0x51;
const REG_DVZ_H: u8 = 0x56;

const WHO_AM_I: u8 = 0x05;
const REVISION: u8 = 0x7C;
/// CTRL1's reset value: big-endian, no auto-increment.
const CTRL1_POR: u8 = 0x20;
const CTRL1_AUTO_INC: u8 = 0x40;
const CTRL1_BIG_ENDIAN: u8 = 0x20;
const CTRL_ST: u8 = 0x80;
const CTRL7_A_EN: u8 = 0x01;
const CTRL7_G_EN: u8 = 0x02;
const CTRL8_HANDSHAKE: u8 = 0x80;
const CTRL9_RST_FIFO: u8 = 0x04;
const STATUSINT_ST_DONE: u8 = 0x01;
const STATUSINT_CMD_DONE: u8 = 0x80;
const FIFO_MODE_FIFO: u8 = 0x01;
/// Samples the FIFO holds (FIFO_SIZE 11).
const FIFO_DEPTH: u64 = 128;
/// The accelerometer's rate while the FIFO fills.
const FIFO_RATE_HZ: u64 = 1000;
/// How long the self-test takes before it reports done.
const SELF_TEST_US: u64 = 2_000;
/// The self-test's stimulus as the dV registers hold it (2048 LSB/g, 16 LSB/dps).
const ST_XL_LSB: i16 = 1100; // about 0.54 g
const ST_GY_LSB: i16 = 8000; // 500 dps

pub struct Qmi8658a {
    reg: [u8; 256],
    st_at: Option<u64>,
    st_gyro: bool,
    fifo_from: u64,
    /// Bytes of the FIFO taken since it was reset, six to a sample.
    fifo_bytes: u64,
}

impl Default for Qmi8658a {
    fn default() -> Self {
        let mut reg = [0; 256];
        reg[REG_WHO_AM_I as usize] = WHO_AM_I;
        reg[REG_REVISION as usize] = REVISION;
        reg[REG_CTRL1 as usize] = CTRL1_POR;
        Qmi8658a { reg, st_at: None, st_gyro: false, fifo_from: 0, fifo_bytes: 0 }
    }
}

impl Qmi8658a {
    fn r(&self, reg: u8) -> u8 {
        self.reg[reg as usize]
    }

    /// LSB per g from CTRL2's aFS (bits 6:4): 2, 4, 8 or 16 g full scale.
    fn lsb_per_g(&self) -> i64 {
        16384 >> ((self.r(REG_CTRL2) >> 4) & 3)
    }

    /// LSB per dps from CTRL3's gFS (bits 6:4): 16 dps up to 2048 dps full scale.
    fn lsb_per_dps(&self) -> i64 {
        2048 >> ((self.r(REG_CTRL3) >> 4) & 7)
    }

    fn clip(v: i64) -> i16 {
        v.clamp(i16::MIN as i64, i16::MAX as i64) as i16
    }

    fn word(&self, v: i16) -> [u8; 2] {
        if self.r(REG_CTRL1) & CTRL1_BIG_ENDIAN != 0 {
            v.to_be_bytes()
        } else {
            v.to_le_bytes()
        }
    }

    /// Registers 0x33..=0x40 as they read now: temperature, acceleration, rotation.
    fn data(&self, ctx: &Ctx) -> [u8; 14] {
        let m = &ctx.motion;
        let on = self.r(REG_CTRL7);
        let (xl, gy) = (self.lsb_per_g(), self.lsb_per_dps());
        let mut d = [0u8; 14];
        // 8.8 fixed point; the die is warm while either sensor runs.
        let temp = if on & (CTRL7_A_EN | CTRL7_G_EN) != 0 { Self::clip(m.temp_dc as i64 * 256 / 10) } else { 0 };
        d[0..2].copy_from_slice(&self.word(temp));
        for i in 0..3 {
            let a = if on & CTRL7_A_EN != 0 { Self::clip(m.accel_mg[i] as i64 * xl / 1000) } else { 0 };
            let g = if on & CTRL7_G_EN != 0 { Self::clip(m.gyro_mdps[i] as i64 * gy / 1000) } else { 0 };
            d[2 + 2 * i..4 + 2 * i].copy_from_slice(&self.word(a));
            d[8 + 2 * i..10 + 2 * i].copy_from_slice(&self.word(g));
        }
        d
    }

    fn fifo_on(&self) -> bool {
        self.r(REG_FIFO_CTRL) & 3 == FIFO_MODE_FIFO
    }

    /// Samples the FIFO holds: the accelerometer at 1 kHz since the FIFO was armed, 128 at most.
    fn fifo_samples(&self, ctx: &Ctx) -> u64 {
        if !self.fifo_on() {
            return 0;
        }
        let made = (ctx.now_us.saturating_sub(self.fifo_from) * FIFO_RATE_HZ / 1_000_000).min(FIFO_DEPTH);
        made.saturating_sub(self.fifo_bytes / 6)
    }

    fn fifo_reset(&mut self, ctx: &Ctx) {
        self.fifo_from = ctx.now_us;
        self.fifo_bytes = 0;
    }

    fn statusint(&mut self, ctx: &Ctx) -> u8 {
        if self.st_at.is_some_and(|at| ctx.now_us.saturating_sub(at) >= SELF_TEST_US) {
            self.reg[REG_STATUSINT as usize] |= STATUSINT_ST_DONE;
        }
        self.r(REG_STATUSINT)
    }

    fn fifo_byte(&mut self, ctx: &Ctx) -> u8 {
        // Every sample is the acceleration as it reads now: there is one world, not a history.
        let d = self.data(ctx);
        let b = d[2 + (self.fifo_bytes % 6) as usize];
        if self.fifo_samples(ctx) > 0 || !self.fifo_bytes.is_multiple_of(6) {
            self.fifo_bytes += 1;
        }
        b
    }

    fn dv(&self, i: u8) -> u8 {
        let b = self.word(if self.st_gyro { ST_GY_LSB } else { ST_XL_LSB });
        b[(i & 1) as usize]
    }

    fn one(&mut self, ctx: &Ctx, reg: u8) -> u8 {
        match reg {
            REG_STATUSINT => self.statusint(ctx),
            REG_STATUS0 => self.r(REG_CTRL7) & (CTRL7_A_EN | CTRL7_G_EN), // new data in each sensor that is on
            REG_FIFO_CNT => (self.fifo_samples(ctx) * 3) as u8,
            REG_FIFO_STATUS => (((self.fifo_samples(ctx) * 3) >> 8) & 3) as u8,
            REG_FIFO_DATA => self.fifo_byte(ctx),
            REG_TEMP_L..=REG_GZ_H => self.data(ctx)[(reg - REG_TEMP_L) as usize],
            REG_DVX_L..=REG_DVZ_H => self.dv(reg - REG_DVX_L),
            r => self.r(r),
        }
    }

    fn put(&mut self, ctx: &Ctx, reg: u8, v: u8) {
        match reg {
            REG_WHO_AM_I | REG_REVISION | REG_STATUSINT | REG_STATUS0 | REG_FIFO_CNT | REG_FIFO_STATUS
            | REG_FIFO_DATA => {}
            REG_TEMP_L..=REG_GZ_H | REG_DVX_L..=REG_DVZ_H => {}
            REG_CTRL2 | REG_CTRL3 => {
                let was = self.r(reg) & CTRL_ST != 0;
                self.reg[reg as usize] = v;
                let now = v & CTRL_ST != 0;
                if now && !was {
                    // The self-test starts: the flag rises once the stimulus has settled.
                    self.st_at = Some(ctx.now_us);
                    self.st_gyro = reg == REG_CTRL3;
                    self.reg[REG_STATUSINT as usize] &= !STATUSINT_ST_DONE;
                } else if !now && was {
                    self.st_at = None;
                    self.reg[REG_STATUSINT as usize] &= !STATUSINT_ST_DONE;
                }
            }
            REG_CTRL9 => {
                self.reg[reg as usize] = v;
                if v == 0 {
                    self.reg[REG_STATUSINT as usize] &= !STATUSINT_CMD_DONE;
                } else {
                    if v == CTRL9_RST_FIFO {
                        self.fifo_reset(ctx);
                    }
                    if self.r(REG_CTRL8) & CTRL8_HANDSHAKE != 0 {
                        self.reg[REG_STATUSINT as usize] |= STATUSINT_CMD_DONE;
                    }
                }
            }
            REG_FIFO_CTRL => {
                let was = self.fifo_on();
                self.reg[reg as usize] = v;
                if self.fifo_on() && !was {
                    self.fifo_reset(ctx);
                }
            }
            r => self.reg[r as usize] = v,
        }
    }

    fn step(&self, reg: u8, i: usize) -> u8 {
        if self.r(REG_CTRL1) & CTRL1_AUTO_INC != 0 {
            reg.wrapping_add(i as u8)
        } else {
            reg
        }
    }
}

impl Chip for Qmi8658a {
    fn addr(&self) -> u8 {
        0x6A
    }

    fn reset(&mut self, _ctx: &mut Ctx) {
        *self = Self::default();
    }

    fn write(&mut self, ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            let r = self.step(reg, i);
            self.put(ctx, r, b);
        }
        true
    }

    fn read(&mut self, ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        // The burst is one instant: the 14 data bytes come from one snapshot.
        for (i, b) in v.iter_mut().enumerate() {
            let r = self.step(reg, i);
            *b = self.one(ctx, r);
        }
        true
    }
}

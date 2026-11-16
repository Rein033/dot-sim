//! The haptic driver, AW86233, in RTP mode as kdrv_aw86233 plays it.
//!
//! GO in RTP mode puts GLB_STATE at "RTP"; STOP takes it back. Samples
//! written to RTPDATA (which does not auto-increment) fill a FIFO that drains
//! at the rate SYSCTRL2 chose, and SYSST reports "almost full" the way the
//! driver polls for. What comes out is how hard the motor is driven: the
//! samples' mean amplitude scaled by the gain, as a percentage, raised when
//! it moves or every 50 ms while it plays.

use crate::{Chip, Ctx, Event};

const REG_SYSST: u8 = 0x01;
const REG_PLAYCFG2: usize = 0x07;
const REG_PLAYCFG3: usize = 0x08;
const REG_PLAYCFG4: u8 = 0x09;
const REG_RTPDATA: u8 = 0x32;
const REG_GLBRD5: usize = 0x3F;
const REG_SYSCTRL2: usize = 0x46;
const REG_CHIPID_H: usize = 0x57;
const REG_CHIPID_L: usize = 0x69;
const GLB_STANDBY: u8 = 0x00;
const GLB_RTP: u8 = 0x08;
const FF_AFS: u8 = 0x08;
const FIFO_BYTES: u32 = 2048;
const FIFO_AFS: u32 = 1536;
const SEND_EVERY_US: u64 = 50_000;

pub struct Aw86233 {
    reg: [u8; 256],
    fill: u32,
    fill_at: u64,
    strength: u8,
    sent_at: u64,
    sent_since_go: bool,
}

impl Default for Aw86233 {
    fn default() -> Self {
        let mut reg = [0; 256];
        reg[REG_CHIPID_H] = 0x23;
        reg[REG_CHIPID_L] = 0x30;
        reg[REG_SYSCTRL2] = 0x40; // standby, until the driver wakes it
        Aw86233 { reg, fill: 0, fill_at: 0, strength: 0, sent_at: 0, sent_since_go: false }
    }
}

impl Aw86233 {
    fn rate_hz(&self) -> u64 {
        if self.reg[REG_SYSCTRL2] & 3 == 2 {
            12_000
        } else {
            24_000
        }
    }

    fn drain(&mut self, ctx: &Ctx) {
        let gone = ctx.now_us.saturating_sub(self.fill_at) * self.rate_hz() / 1_000_000;
        self.fill = self.fill.saturating_sub(gone.min(u32::MAX as u64) as u32);
        self.fill_at = ctx.now_us;
    }

    fn send(&mut self, ctx: &mut Ctx, strength: u8, ms: u16) {
        self.strength = strength;
        self.sent_at = ctx.now_us;
        self.sent_since_go = true;
        ctx.events.push(Event::Haptic { strength, ms });
    }

    fn rtp(&mut self, ctx: &mut Ctx, v: &[u8]) {
        if v.is_empty() {
            return;
        }
        self.drain(ctx);
        let sum: u32 = v.iter().map(|&b| (b as i8).unsigned_abs() as u32).sum();
        self.fill = (self.fill + v.len() as u32).min(FIFO_BYTES);
        let pct = sum * 100 * self.reg[REG_PLAYCFG2] as u32 / (v.len() as u32 * 127 * 255);
        let strength = pct.min(100) as u8;
        if !self.sent_since_go
            || strength.abs_diff(self.strength) >= 5
            || ctx.now_us.saturating_sub(self.sent_at) >= SEND_EVERY_US
        {
            let ms = (self.fill as u64 * 1000 / self.rate_hz() + 1) as u16;
            self.send(ctx, strength, ms);
        }
    }
}

impl Chip for Aw86233 {
    fn addr(&self) -> u8 {
        0x5A
    }

    fn reset(&mut self, ctx: &mut Ctx) {
        *self = Self::default();
        self.fill_at = ctx.now_us;
    }

    fn write(&mut self, ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        if reg == REG_RTPDATA {
            self.rtp(ctx, v);
            return true;
        }
        for (i, &b) in v.iter().enumerate() {
            let r = reg.wrapping_add(i as u8);
            match r as usize {
                REG_CHIPID_H | REG_CHIPID_L | REG_GLBRD5 => continue,
                _ if r == REG_SYSST => continue,
                0x00 if b == 0xAA => self.reset(ctx), // soft reset
                _ if r == REG_PLAYCFG4 => {
                    if b & 0x02 != 0 {
                        // STOP
                        self.reg[REG_GLBRD5] = GLB_STANDBY;
                        self.fill = 0;
                        self.send(ctx, 0, 0);
                        self.sent_since_go = false;
                    } else if b & 0x01 != 0 && self.reg[REG_PLAYCFG3] & 3 == 1 {
                        // GO, in RTP
                        self.reg[REG_GLBRD5] = GLB_RTP;
                        self.fill = 0;
                        self.fill_at = ctx.now_us;
                        self.sent_since_go = false;
                    }
                }
                ri => self.reg[ri] = b,
            }
        }
        true
    }

    fn read(&mut self, ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        for (i, b) in v.iter_mut().enumerate() {
            let r = reg.wrapping_add(i as u8);
            *b = if r == REG_SYSST {
                self.drain(ctx);
                if self.fill >= FIFO_AFS {
                    FF_AFS
                } else {
                    0
                }
            } else {
                self.reg[r as usize]
            };
        }
        true
    }
}

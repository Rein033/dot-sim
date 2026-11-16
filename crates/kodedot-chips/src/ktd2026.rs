//! The status LED's driver, KTD2026, as kdrv_led drives it.
//!
//! OUT1 is the green die, OUT2 the blue, OUT3 the red. A channel is lit when
//! its two LED_EN bits are not 00 and the chip is in "always on" (EN_RST
//! [4:3] = 11); its brightness is the current, 0.125 mA x (ISET + 1), against
//! the 5 mA kdrv_led treats as full scale. The colour is raised whenever it
//! changes.

use crate::{Chip, Ctx, Event};

const REG_EN_RST: usize = 0x00;
const REG_LED_EN: usize = 0x04;
const REG_ISET1: usize = 0x06;
const REG_COUNT: usize = 0x0A;
const RESET_FULL: u8 = 0x07;
const FULL_SCALE_UA: u32 = 5000;

#[derive(Default)]
pub struct Ktd2026 {
    reg: [u8; REG_COUNT],
    shown: [u8; 3],
}

impl Ktd2026 {
    fn level(&self, out: usize) -> u8 {
        if (self.reg[REG_EN_RST] >> 3) & 3 != 3 || (self.reg[REG_LED_EN] >> (2 * out)) & 3 == 0 {
            return 0;
        }
        let ua = 125 * (self.reg[REG_ISET1 + out] as u32 + 1);
        ((ua * 255 + FULL_SCALE_UA / 2) / FULL_SCALE_UA).min(255) as u8
    }

    fn show(&mut self, ctx: &mut Ctx) {
        let rgb = [self.level(2), self.level(0), self.level(1)];
        if rgb != self.shown {
            self.shown = rgb;
            ctx.events.push(Event::Led(rgb));
        }
    }
}

impl Chip for Ktd2026 {
    fn addr(&self) -> u8 {
        0x30
    }

    fn reset(&mut self, _ctx: &mut Ctx) {
        *self = Self::default();
    }

    fn write(&mut self, ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            let r = reg as usize + i;
            if r >= REG_COUNT {
                return false;
            }
            if r == REG_EN_RST && b & 7 == RESET_FULL {
                self.reg = [0; REG_COUNT];
            } else {
                self.reg[r] = b;
            }
        }
        self.show(ctx);
        true
    }

    fn read(&mut self, _ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        for (i, b) in v.iter_mut().enumerate() {
            *b = self.reg.get(reg as usize + i).copied().unwrap_or(0);
        }
        true
    }
}

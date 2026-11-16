//! The expander, TCA6408A: five buttons, the IMU's and the touch's IRQs, the
//! panel's VCI_EN.
//!
//! Pins configured as inputs read what the page says - a button is active
//! low, so released reads 1, and the touch controller's INT on P6 is active
//! low too. An output reads its own latch. INT (a P4 pin, active low) is
//! held while the inputs differ from what was last read from the input
//! register, and released by reading it, which is the chip's own rule.

use crate::{Chip, Ctx, Event, BTN_BOT, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_UP, PIN_IOEXP_INT};

const REG_INPUT: u8 = 0x00;
const REG_OUTPUT: u8 = 0x01;
const REG_POLARITY: u8 = 0x02;
const REG_CONFIG: u8 = 0x03;
/// Which expander pin each button is on (kbsp_pins.h, KBSP_IOEXP_BIT_*).
const BUTTON_BITS: [(u8, u8); 5] = [(BTN_BOT, 0), (BTN_UP, 1), (BTN_LEFT, 2), (BTN_DOWN, 3), (BTN_RIGHT, 4)];
const BIT_TP_INT: u8 = 6;

pub struct Tca6408a {
    out: u8,
    pol: u8,
    cfg: u8,
    /// What drives the input pins, 1 = high.
    pins: u8,
    /// The input register as last read.
    seen: u8,
}

impl Default for Tca6408a {
    fn default() -> Self {
        Tca6408a { out: 0xFF, pol: 0x00, cfg: 0xFF, pins: 0xFF, seen: 0xFF }
    }
}

impl Tca6408a {
    fn input(&self) -> u8 {
        ((self.pins & self.cfg) | (self.out & !self.cfg)) ^ self.pol
    }

    fn update_int(&self, ctx: &mut Ctx) {
        ctx.events.push(Event::Pin { pin: PIN_IOEXP_INT, level: self.input() == self.seen });
    }

    /// The five expander buttons, as `BTN_*` (TOP is a P4 pin, not here).
    pub fn set_buttons(&mut self, ctx: &mut Ctx, mask: u8) {
        for (btn, bit) in BUTTON_BITS {
            if mask & btn != 0 {
                self.pins &= !(1 << bit);
            } else {
                self.pins |= 1 << bit;
            }
        }
        self.update_int(ctx);
    }

    /// The touch controller's INT line, which reaches the P4 through P6.
    pub fn set_touch_irq(&mut self, ctx: &mut Ctx, asserted: bool) {
        if asserted {
            self.pins &= !(1 << BIT_TP_INT);
        } else {
            self.pins |= 1 << BIT_TP_INT;
        }
        self.update_int(ctx);
    }
}

impl Chip for Tca6408a {
    fn addr(&self) -> u8 {
        0x20
    }

    fn reset(&mut self, ctx: &mut Ctx) {
        *self = Self::default();
        self.seen = self.input();
        self.update_int(ctx);
    }

    fn write(&mut self, ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            match reg.wrapping_add(i as u8) {
                REG_INPUT => {} // read-only
                REG_OUTPUT => self.out = b,
                REG_POLARITY => self.pol = b,
                REG_CONFIG => self.cfg = b,
                _ => return false,
            }
        }
        self.update_int(ctx);
        true
    }

    fn read(&mut self, ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        for (i, b) in v.iter_mut().enumerate() {
            *b = match reg.wrapping_add(i as u8) {
                REG_INPUT => {
                    self.seen = self.input();
                    self.seen
                }
                REG_OUTPUT => self.out,
                REG_POLARITY => self.pol,
                REG_CONFIG => self.cfg,
                _ => return false,
            };
        }
        self.update_int(ctx);
        true
    }
}

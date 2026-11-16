//! The nine chips on the Kode Dot's I2C bus, as esp-emu's transactions
//! reach them.
//!
//! esp-emu delivers one logical transaction at a time: the bytes the master
//! clocked out, and how many it expects back. The first byte written is the
//! register; a read with no write carries on from where the last access to
//! that chip left off, as a read the driver split in pieces does on the wire.

use crate::{
    Aw86233, Battery, Bq25896, Chip, Cst820, Ctx, Cw2217, Es8311, Event, Ktd2026, Motion, Qmc5883p, Qmi8658a, Tca6408a,
};

pub struct Board {
    ctx: Ctx,
    pub ktd2026: Ktd2026,
    pub tca6408a: Tca6408a,
    pub cst820: Cst820,
    pub aw86233: Aw86233,
    pub bq25896: Bq25896,
    pub cw2217: Cw2217,
    pub es8311: Es8311,
    pub qmi8658a: Qmi8658a,
    pub qmc5883p: Qmc5883p,
    /// Each chip's register pointer, for a read that continues.
    pointer: [u8; 128],
    /// The codec's gain as last raised.
    gain: i32,
}

impl Default for Board {
    fn default() -> Self {
        Self::new()
    }
}

impl Board {
    /// The board just powered: every chip at its reset state.
    pub fn new() -> Self {
        let mut b = Board {
            ctx: Ctx::default(),
            ktd2026: Ktd2026::default(),
            tca6408a: Tca6408a::default(),
            cst820: Cst820::default(),
            aw86233: Aw86233::default(),
            bq25896: Bq25896::default(),
            cw2217: Cw2217::default(),
            es8311: Es8311::default(),
            qmi8658a: Qmi8658a::default(),
            qmc5883p: Qmc5883p::default(),
            pointer: [0; 128],
            gain: 0,
        };
        b.reset();
        b
    }

    fn chips(&mut self) -> [&mut dyn Chip; 9] {
        [
            &mut self.ktd2026,
            &mut self.tca6408a,
            &mut self.cst820,
            &mut self.aw86233,
            &mut self.bq25896,
            &mut self.cw2217,
            &mut self.es8311,
            &mut self.qmi8658a,
            &mut self.qmc5883p,
        ]
    }

    /// Every chip back to its power-on state; the battery stays as set.
    pub fn reset(&mut self) {
        let mut ctx = std::mem::take(&mut self.ctx);
        for c in self.chips() {
            c.reset(&mut ctx);
        }
        self.ctx = ctx;
        self.pointer = [0; 128];
        self.gain = self.es8311.out_gain_q8();
    }

    /// The addresses the board answers.
    pub fn addrs(&mut self) -> Vec<u8> {
        self.chips().iter().map(|c| c.addr()).collect()
    }

    /// One I2C transaction: `write` clocked out, `read_len` bytes expected
    /// back, `continues` for the next piece of a split read. None is a NACK.
    pub fn i2c(&mut self, addr: u8, write: &[u8], read_len: usize, continues: bool) -> Option<Vec<u8>> {
        let mut ctx = std::mem::take(&mut self.ctx);
        let ptr = self.pointer[addr as usize & 0x7F];
        let chip = self.chips().into_iter().find(|c| c.addr() == addr);
        let out = chip.and_then(|chip| {
            if write.is_empty() && read_len == 0 {
                return Some((ptr, vec![])); // a probe
            }
            let reg = match write.first() {
                Some(&r) if !continues => r,
                _ => ptr,
            };
            if read_len == 0 {
                return chip.write(&mut ctx, reg, &write[1..]).then(|| (reg, vec![]));
            }
            let mut v = vec![0; read_len];
            chip.read(&mut ctx, reg, &mut v).then(|| (reg.wrapping_add(read_len as u8), v))
        });
        self.ctx = ctx;
        let gain = self.es8311.out_gain_q8();
        if gain != self.gain {
            self.gain = gain;
            self.ctx.events.push(Event::Gain(gain));
        }
        out.map(|(next, v)| {
            self.pointer[addr as usize & 0x7F] = next;
            v
        })
    }

    /// The five expander buttons and TOP, as `BTN_*`; TOP is a P4 pin, so it
    /// comes out as an [`Event::Pin`] on 35 (active low).
    pub fn set_buttons(&mut self, mask: u8) {
        self.tca6408a.set_buttons(&mut self.ctx, mask);
        self.ctx.events.push(Event::Pin { pin: crate::PIN_BUTTON_TOP, level: mask & crate::BTN_TOP == 0 });
    }

    /// A finger on the glass, or none: the touch frame and its IRQ on P6.
    pub fn set_touch(&mut self, pressed: bool, x: u16, y: u16) {
        self.cst820.set_touch(pressed, x, y);
        self.tca6408a.set_touch_irq(&mut self.ctx, pressed);
    }

    pub fn set_battery(&mut self, percent: u8, usb: bool) {
        self.ctx.battery = Battery { percent: percent.min(100), usb };
    }

    /// What the motion sensors measure, as the page sets it.
    pub fn set_motion(&mut self, motion: Motion) {
        self.ctx.motion = motion;
    }

    /// The emulated time, which the haptic FIFO drains against.
    pub fn set_now_us(&mut self, us: u64) {
        self.ctx.now_us = us;
    }

    /// What the chips raised since the last call, oldest first.
    pub fn take_events(&mut self) -> Vec<Event> {
        std::mem::take(&mut self.ctx.events)
    }
}

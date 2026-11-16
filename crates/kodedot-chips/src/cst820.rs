//! The touch controller, CST820: an identity and a finger.
//!
//! kdrv_cst820 checks the family nibble of CHIP_ID (0xBx) and reads six bytes
//! from 0x01: gesture, fingers, event|x[11:8], x[7:0], event|y[11:8],
//! y[7:0]. Everything else is a register file that keeps what it is given.

use crate::{Chip, Ctx};

const REG_FRAME: usize = 0x01;
const REG_CHIP_ID: usize = 0xA7;
const EV_CONTACT: u8 = 0x80;
const REG_IRQ_CTL: usize = 0xFA;
/// What the controller's own reset leaves in IRQ_CTL.
const IRQ_CTL_POR: u8 = 0x60;

pub struct Cst820 {
    reg: [u8; 256],
}

impl Default for Cst820 {
    fn default() -> Self {
        let mut reg = [0; 256];
        reg[REG_CHIP_ID] = 0xB7; // CST820
        reg[REG_CHIP_ID + 1] = 0x00; // project
        reg[REG_CHIP_ID + 2] = 0x01; // firmware
        reg[REG_IRQ_CTL] = IRQ_CTL_POR;
        Cst820 { reg }
    }
}

impl Cst820 {
    pub fn set_touch(&mut self, pressed: bool, x: u16, y: u16) {
        let f = &mut self.reg[REG_FRAME..REG_FRAME + 6];
        f[0] = 0;
        f[1] = pressed as u8;
        f[2] = if pressed { EV_CONTACT } else { 0 } | ((x >> 8) as u8 & 0x0F);
        f[3] = x as u8;
        f[4] = (y >> 8) as u8 & 0x0F;
        f[5] = y as u8;
    }
}

impl Chip for Cst820 {
    fn addr(&self) -> u8 {
        0x15
    }

    fn reset(&mut self, _ctx: &mut Ctx) {
        *self = Self::default();
    }

    fn write(&mut self, _ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            let r = reg.wrapping_add(i as u8) as usize;
            if r < REG_FRAME + 6 || (REG_CHIP_ID..REG_CHIP_ID + 3).contains(&r) {
                continue; // status and identity are read-only
            }
            self.reg[r] = b;
        }
        true
    }

    fn read(&mut self, _ctx: &mut Ctx, reg: u8, v: &mut [u8]) -> bool {
        for (i, b) in v.iter_mut().enumerate() {
            *b = self.reg[reg.wrapping_add(i as u8) as usize];
        }
        true
    }
}

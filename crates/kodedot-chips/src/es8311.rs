//! The codec, ES8311: its identity, and the DAC's volume and mute.
//!
//! kdrv_es8311 checks CHD1/CHD2 (0xFD/0xFE) against 0x83/0x11 and then hands
//! the chip to esp_codec_dev, whose es8311 driver programs it through these
//! same registers. The clocking and the ADC path need no behaviour here:
//! what matters to the page is how loud the DAC plays, which is REG32 (0xBF
//! is 0 dB, 0.5 dB a step) unless REG31's DAC mute bits are set.

use crate::{Chip, Ctx};

const REG_DAC_MUTE: usize = 0x31;
const DAC_MUTE_BITS: u8 = 0x60;
const REG_DAC_VOL: usize = 0x32;
const VOL_0DB: u8 = 0xBF;

pub struct Es8311 {
    reg: [u8; 256],
}

impl Default for Es8311 {
    fn default() -> Self {
        let mut reg = [0; 256];
        reg[0xFD] = 0x83;
        reg[0xFE] = 0x11;
        Es8311 { reg }
    }
}

impl Es8311 {
    /// The DAC's volume and mute as a gain, 256 = 0 dB, 0 = muted.
    pub fn out_gain_q8(&self) -> i32 {
        if self.reg[REG_DAC_MUTE] & DAC_MUTE_BITS != 0 {
            return 0;
        }
        let db = (self.reg[REG_DAC_VOL] as f32 - VOL_0DB as f32) * 0.5;
        (256.0 * 10f32.powf(db / 20.0)).round() as i32
    }
}

impl Chip for Es8311 {
    fn addr(&self) -> u8 {
        0x18
    }

    fn reset(&mut self, _ctx: &mut Ctx) {
        *self = Self::default();
    }

    fn write(&mut self, _ctx: &mut Ctx, reg: u8, v: &[u8]) -> bool {
        for (i, &b) in v.iter().enumerate() {
            let r = reg.wrapping_add(i as u8) as usize;
            if r < 0xFD {
                self.reg[r] = b;
            }
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

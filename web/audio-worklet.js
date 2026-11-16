// The Kode Dot's speaker and microphone, on the audio thread.

// kode-speaker: plays the firmware's PCM_OUT. A ring holds ~1 s; playback
// starts once 150 ms are buffered and plays silence when the ring runs dry,
// so a slow firmware is heard as a gap, never as a stall.
class KodeSpeaker extends AudioWorkletProcessor {
  constructor() {
    super();
    this.size = 48000;
    this.l = new Float32Array(this.size);
    this.r = new Float32Array(this.size);
    this.w = 0; this.rd = 0; this.started = false;
    this.port.onmessage = (m) => this.push(m.data);
  }

  push({ rate, ch, s }) {
    // Resample by nearest step when the firmware's rate is not the context's.
    const step = rate / sampleRate, frames = Math.floor(s.length / ch / step);
    for (let i = 0; i < frames; i++) {
      if (this.w - this.rd >= this.size) this.rd = this.w - this.size + 1;   // overrun: drop oldest
      const f = Math.floor(i * step) * ch;
      this.l[this.w % this.size] = s[f] / 32768;
      this.r[this.w % this.size] = s[f + (ch > 1 ? 1 : 0)] / 32768;
      this.w++;
    }
  }

  process(_, outputs) {
    const [L, R] = outputs[0];
    if (!this.started && this.w - this.rd >= sampleRate * 0.15) this.started = true;
    for (let i = 0; i < L.length; i++) {
      if (this.started && this.rd < this.w) {
        L[i] = this.l[this.rd % this.size]; R[i] = this.r[this.rd % this.size]; this.rd++;
      } else {
        L[i] = 0; R[i] = 0; this.started = false;
      }
    }
    return true;
  }
}

// kode-mic: the Mac's microphone, mono, resampled to the firmware's rate and
// sent as s16le in 20 ms chunks.
class KodeMic extends AudioWorkletProcessor {
  constructor({ processorOptions }) {
    super();
    this.rate = processorOptions.rate;
    this.chunk = Math.round(this.rate / 50);
    this.buf = new Int16Array(this.chunk);
    this.n = 0;
    this.pos = 0;
  }

  process(inputs) {
    const ch = inputs[0] && inputs[0][0];
    if (!ch) return true;
    const step = sampleRate / this.rate;
    for (; this.pos < ch.length; this.pos += step) {
      const v = Math.max(-1, Math.min(1, ch[Math.floor(this.pos)]));
      this.buf[this.n++] = v * 32767;
      if (this.n === this.chunk) {
        this.port.postMessage(this.buf.buffer.slice(0));
        this.n = 0;
      }
    }
    this.pos -= ch.length;
    return true;
  }
}

registerProcessor("kode-speaker", KodeSpeaker);
registerProcessor("kode-mic", KodeMic);

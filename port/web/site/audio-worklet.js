/*
AUDIO-WORKLET.JS

Plays the game's sound: the mixer (port/linux/src/dsound_sdl.c, through
port/web/src/web_sdl.c) writes interleaved stereo float frames at 48 kHz
into a ring in the WebAssembly memory, which is a SharedArrayBuffer, and
this processor reads them at the audio context's rate.
*/

'use strict';

class HaloAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { buffer, ring, ringFrames, write, read, underruns, rate } = options.processorOptions;
    this.ring = new Float32Array(buffer, ring, ringFrames * 2);
    this.words = new Int32Array(buffer);
    this.ringMask = ringFrames - 1;
    this.writeIndex = write >> 2;
    this.readIndex = read >> 2;
    this.underrunIndex = underruns >> 2;
    // source frames per output frame
    this.step = rate / sampleRate;
    this.position = 0;
    this.port.onmessage = (event) => {
      if (event.data && event.data.volume !== undefined) this.volume = event.data.volume;
    };
    this.volume = 1;
  }

  process(inputs, outputs) {
    const output = outputs[0];
    const left = output[0];
    const right = output[1] || output[0];
    const frames = left.length;
    const write = Atomics.load(this.words, this.writeIndex);
    let read = Atomics.load(this.words, this.readIndex);
    const needed = Math.ceil(this.position + frames * this.step);

    if (write - read < needed) {
      // not enough yet: silence, and let the ring fill
      left.fill(0);
      if (right !== left) right.fill(0);
      Atomics.add(this.words, this.underrunIndex, 1);
      return true;
    }
    const ring = this.ring;
    const mask = this.ringMask;
    const volume = this.volume;
    if (this.step === 1) {
      for (let i = 0; i < frames; i++) {
        const slot = ((read + i) & mask) * 2;
        left[i] = ring[slot] * volume;
        right[i] = ring[slot + 1] * volume;
      }
      read += frames;
    } else {
      // linear interpolation when the device does not run at 48 kHz
      let position = this.position;
      for (let i = 0; i < frames; i++) {
        const whole = Math.floor(position);
        const fraction = position - whole;
        const a = ((read + whole) & mask) * 2;
        const b = ((read + whole + 1) & mask) * 2;
        left[i] = (ring[a] + (ring[b] - ring[a]) * fraction) * volume;
        right[i] = (ring[a + 1] + (ring[b + 1] - ring[a + 1]) * fraction) * volume;
        position += this.step;
      }
      const consumed = Math.floor(position);
      read += consumed;
      this.position = position - consumed;
    }
    Atomics.store(this.words, this.readIndex, read);
    return true;
  }
}

registerProcessor('halo-audio', HaloAudioProcessor);

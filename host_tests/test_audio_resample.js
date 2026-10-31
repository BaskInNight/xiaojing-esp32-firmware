'use strict';
// Exercise the production C filter compiled as freestanding WebAssembly.
const assert = require('assert');
const fs = require('fs');

(async () => {
  const {instance} = await WebAssembly.instantiate(fs.readFileSync(process.argv[2]));
  const e = instance.exports;
  e.memory.grow(2);
  const address = Number(e.__heap_base.value);
  const input = new Int16Array(e.memory.buffer, address, 24000);
  const count = e.audio_pcm_24k_to_16k_count(input.length);
  assert.strictEqual(count, 16000, 'one second must remain one second');
  assert.strictEqual(e.audio_pcm_24k_to_16k_count(24001), 16000);
  assert.strictEqual(e.audio_pcm_24k_to_16k_count(24002), 16001);
  for (const value of [32767, -32768, 12345]) {
    input.fill(value);
    for (const i of [0, 1, 159, 160, count - 1])
      assert.strictEqual(e.audio_pcm_24k_to_16k_sample(address, input.length, i), value);
  }
  function render(freq) {
    for (let i = 0; i < input.length; i++)
      input[i] = Math.round(12000 * Math.sin(2 * Math.PI * freq * i / 24000));
    return Array.from({length: count}, (_, i) =>
      e.audio_pcm_24k_to_16k_sample(address, input.length, i));
  }
  const tone = render(1000);
  let crossings = 0;
  for (let i = 1; i < tone.length; i++) if (tone[i - 1] <= 0 && tone[i] > 0) crossings++;
  assert.ok(Math.abs(crossings - 1000) <= 1, 'speech pitch must not shift to 667 Hz');
  const rms = x => Math.sqrt(x.slice(100, -100).reduce((s, v) => s + v*v, 0) / (x.length - 200));
  assert.ok(Math.abs(rms(tone) / (12000 / Math.SQRT2) - 1) < 0.02, 'pass-band gain');
  const aliasRatio = rms(render(10000)) / rms(tone);
  assert.ok(aliasRatio < 0.03, 'high frequencies must not alias into the voice band');
  console.log(`AUDIO_RESAMPLE PASS: 24000->16000 samples, pitch=${crossings}Hz, alias=${(20*Math.log10(aliasRatio)).toFixed(1)}dB`);
})().catch(error => { console.error(error); process.exitCode = 1; });

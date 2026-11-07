const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../platform/web/media_codec_encoder.js'), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));
function environment() {
  const probes = [], encoders = [], frames = [];
  class Encoder {
    static isConfigSupported(config) { return new Promise((resolve, reject) => probes.push({config, resolve, reject})); }
    constructor(callbacks) { this.callbacks = callbacks; this.state = 'unconfigured'; this.closed = 0; this.encodeQueueSize = 0; encoders.push(this); }
    configure(config) { this.config = config; this.state = 'configured'; }
    encode(frame) { this.frame = frame; }
    flush() { return new Promise((resolve, reject) => { this.resolveFlush = resolve; this.rejectFlush = reject; }); }
    close() { this.state = 'closed'; ++this.closed; }
  }
  class AudioData {
    constructor(options) { Object.assign(this, options); this.closed = 0; frames.push(this); }
    close() { ++this.closed; }
  }
  const context = vm.createContext({AudioEncoder: Encoder, AudioData, Uint8Array, ArrayBuffer});
  vm.runInContext(source, context);
  return {api: context.HuxerMediaAudioEncoder, probes, encoders, frames};
}
const options = {rate: 48000, channels: 2, bitrate: 128000};
function chunk(timestamp = 0, size = 32) {
  return {timestamp, byteLength: size, copyTo(bytes) { bytes.fill(37); }};
}
function metadata(description = new Uint8Array([0x11, 0x90])) {
  return {decoderConfig: {codec: 'mp4a.40.2', sampleRate: 48000, numberOfChannels: 2, description}};
}
async function ready(env) {
  const id = env.api.open(options);
  const probe = env.probes.shift(); probe.resolve({supported: true, config: probe.config}); await turn();
  assert.equal(env.api.ready(id), true);
  return id;
}
async function run() {
  const env = environment();
  const canceled = env.api.open(options); const pending = env.probes.shift();
  assert.equal(pending.config.aac.format, 'aac');
  assert.equal(env.api.close(canceled), '');
  pending.resolve({supported: true, config: pending.config}); await turn();
  assert.equal(env.encoders.length, 0, 'Late probe created a retired encoder');

  const id = await ready(env); const encoder = env.encoders.at(-1);
  encoder.encodeQueueSize = 8;
  assert.equal(env.api.submit(id, new Uint8Array(4096), 0, 1024), false);
  assert.equal(env.frames.length, 0);
  encoder.encodeQueueSize = 0;
  assert.equal(env.api.submit(id, new Uint8Array(4096), 0, 1024), true);
  assert.equal(env.frames.at(-1).format, 's16'); assert.equal(env.frames.at(-1).closed, 1);
  const description = new Uint8Array([0x11, 0x90]);
  encoder.callbacks.output(chunk(-21333), metadata(description)); description.fill(0);
  assert.equal(env.api.poll(id).description[0], 0x11, 'ASC did not own its bytes');
  const packet = env.api.poll(id); assert.equal(packet.pts, -21333); assert.equal(packet.data[0], 37);
  encoder.callbacks.output(chunk(0)); assert.equal(env.api.poll(id).kind, 'packet');
  env.api.finish(id); assert.equal(env.api.ended(id), false);
  encoder.resolveFlush(); await turn(); assert.equal(env.api.ended(id), true);
  assert.equal(env.api.close(id), ''); assert.equal(encoder.closed, 1);
  let copied = false;
  encoder.callbacks.output({get byteLength() { copied = true; return 1; }});
  assert.equal(copied, false, 'Late output copied retired bytes');
  assert.equal(env.api.close(id), '');

  const flushing = await ready(env); const flushingEncoder = env.encoders.at(-1);
  env.api.finish(flushing); env.api.close(flushing); flushingEncoder.rejectFlush(new Error('late flush')); await turn();
  assert.equal(env.api.error(flushing), '');
  const failedProbe = env.api.open(options); const unsupported = env.probes.shift();
  unsupported.resolve({supported: false, config: unsupported.config}); await turn();
  assert.match(env.api.error(failedProbe), /does not support/); env.api.close(failedProbe);

  for (const bad of ['oversized', 'unsafe PTS', 'missing ASC', 'changed ASC', 'unreserved output']) {
    const session = await ready(env); const encoder = env.encoders.at(-1);
    if (bad !== 'unreserved output') env.api.submit(session, new Uint8Array(4096), 0, 1024);
    if (bad === 'changed ASC') encoder.callbacks.output(chunk(), metadata());
    encoder.callbacks.output(chunk(bad === 'unsafe PTS' ? Number.MAX_SAFE_INTEGER + 1 : 0, bad === 'oversized' ? 65537 : 32),
      bad === 'missing ASC' ? undefined : metadata(bad === 'changed ASC' ? new Uint8Array([0x12, 0x10]) : undefined));
    assert.ok(env.api.error(session), bad + ' was accepted'); env.api.close(session);
  }
  const quota = await ready(env); const quotaEncoder = env.encoders.at(-1);
  env.api.submit(quota, new Uint8Array(4096), 0, 1024);
  for (let i = 0; i < 10; ++i) quotaEncoder.callbacks.output(chunk(i * 21333), i ? undefined : metadata());
  assert.match(env.api.error(quota), /reserved/); env.api.close(quota);
  console.log('Web AAC encoder support, bounds, metadata, input release, flush and retirement contracts passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });

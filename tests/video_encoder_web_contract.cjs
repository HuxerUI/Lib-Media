const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../platform/web/media_codec_encoder.js'), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));
function environment(extra = {}) {
  const probes = [], encoders = [], frames = [];
  class Encoder {
    static isConfigSupported(config) { return new Promise((resolve, reject) => probes.push({config, resolve, reject})); }
    constructor(callbacks) {
      this.callbacks = callbacks; this.state = 'unconfigured'; this.closed = 0; this.encodeQueueSize = 0; encoders.push(this);
    }
    configure(config) { this.config = config; this.state = 'configured'; }
    encode(frame, options) { this.last = {frame, options}; }
    flush() { return new Promise((resolve, reject) => { this.resolveFlush = resolve; this.rejectFlush = reject; }); }
    close() { this.state = 'closed'; ++this.closed; }
  }
  class Frame {
    constructor(data, init) { this.data = ArrayBuffer.isView(data) ? data.slice() : data; this.init = init; this.closed = 0; frames.push(this); }
    close() { ++this.closed; }
  }
  const context = vm.createContext({VideoEncoder: Encoder, VideoFrame: Frame, Uint8Array, ArrayBuffer, ...extra});
  vm.runInContext(source, context);
  return {api: context.HuxerMediaVideoEncoder, probes, encoders, frames};
}
const options = {width: 320, height: 240, rateNumerator: 30000, rateDenominator: 1001, bitrate: 2000000,
  profile: 66, maxB: 0, layout: 'NV12', color: {}, keyInterval: 60, maximum: 1024, window: 4};
const metadata = (description = new Uint8Array([1, 66, 0, 31, 255, 225, 0])) =>
  ({decoderConfig: {codec: 'avc1.42e01f', description}});
const chunk = (pts = 0, size = 32) => ({timestamp: pts, byteLength: size, copyTo(data) { data.fill(37); }});
async function ready(env, config = options) {
  const id = env.api.open(config), probe = env.probes.shift();
  probe.resolve({supported: true, config: probe.config}); await turn();
  assert.equal(env.api.ready(id), true); return id;
}
async function run() {
  {
    const env = environment({VideoEncoder: undefined}), id = env.api.open(options);
    assert.match(env.api.error(id), /unavailable/); env.api.close(id);
  }
  for (const profile of [77, 100]) for (const maxB of [0, 1, 2]) {
    const env = environment(), id = env.api.open({...options, profile, maxB});
    assert.match(env.api.error(id), /numeric B-picture/); assert.equal(env.probes.length, 0); env.api.close(id);
  }
  for (const profile of [66, 77, 100]) {
    const env = environment(), id = await ready(env, {...options, profile, maxB: profile === 66 ? 0 : null});
    const encoder = env.encoders[0];
    assert.equal(encoder.config.latencyMode, 'quality');
    assert.equal(encoder.config.avc.format, 'avc');
    assert.equal(encoder.config.framerate, 30000 / 1001);
    assert.equal(encoder.config.codec.slice(5, 7), profile.toString(16));
    env.api.close(id);
  }
  {
    const env = environment(), id = env.api.open(options);
    const probe = env.probes.shift();
    probe.resolve({supported: true, config: {...probe.config, bitrate: 2400000}});
    await turn();
    assert.equal(env.api.ready(id), true);
    assert.equal(env.encoders[0].config.bitrate, options.bitrate);
    env.api.close(id);
  }
  {
    const env = environment(), id = await ready(env, {...options, layout: 'Opaque'});
    const source = {timestamp: 123, copyTo() { throw new Error('Unexpected pixel readback'); }};
    assert.equal(env.api.submit(id, source, 0, 33367), true);
    const {frame} = env.encoders[0].last;
    assert.equal(frame.data, source);
    assert.equal(frame.init.timestamp, 0);
    assert.equal(frame.init.duration, 33367);
    assert.equal(frame.init.format, undefined);
    assert.equal(frame.closed, 1);
    assert.equal(source.timestamp, 123);
    assert.equal(env.api.error(id), '');
    env.api.close(id);
  }
  {
    const env = environment(), id = await ready(env), encoder = env.encoders[0];
    const pixels = new Uint8Array([91, 91]);
    assert.equal(env.api.submit(id, pixels, 0, 33367), true); pixels.fill(0);
    assert.equal(env.frames[0].closed, 1); assert.equal(env.frames[0].data[0], 91);
    assert.equal(encoder.last.options.keyFrame, true);
    for (let i = 1; i < 4; ++i) assert.equal(env.api.submit(id, pixels, i * 33367, null), true);
    assert.equal(env.api.submit(id, pixels, 200000, null), false);
    env.api.progress(id);
    assert.equal(env.api.submit(id, pixels, 200000, null), false);
    for (const pts of [0, 100101, 33367, 66734]) encoder.callbacks.output(chunk(pts), metadata());
    encoder.resolveFlush(); await turn();
    assert.equal(env.api.error(id), '');
    assert.equal(env.api.poll(id).kind, 'format');
    assert.deepEqual([0, 1, 2, 3].map(() => env.api.poll(id).pts), [0, 100101, 33367, 66734]);
    env.api.requestKeyFrame(id);
    assert.equal(env.api.submit(id, pixels, 200000, null), true);
    assert.equal(encoder.last.options.keyFrame, true);
    encoder.callbacks.output(chunk(200000), metadata());
    env.api.finish(id); encoder.resolveFlush(); await turn();
    assert.equal(env.api.ended(id), false); assert.equal(env.api.poll(id).pts, 200000); assert.equal(env.api.ended(id), true);
    assert.equal(env.api.close(id), ''); assert.equal(encoder.closed, 1);
    encoder.callbacks.output(chunk(300000), metadata()); assert.equal(env.api.close(id), '');
  }
  for (const bad of ['capacity', 'timestamp', 'initialization', 'lost frame', 'changed format']) {
    const env = environment(), id = await ready(env), encoder = env.encoders[0];
    env.api.submit(id, new Uint8Array(1), 0, null);
    if (bad === 'lost frame') { env.api.finish(id); encoder.resolveFlush(); await turn(); }
    else if (bad === 'initialization') encoder.callbacks.output(chunk(), {});
    else if (bad === 'changed format') {
      encoder.callbacks.output(chunk(), metadata()); env.api.submit(id, new Uint8Array(1), 33367, null);
      encoder.callbacks.output(chunk(33367), metadata(new Uint8Array([1, 77, 0, 31, 255, 225, 0])));
    } else encoder.callbacks.output(chunk(bad === 'timestamp' ? Number.MAX_SAFE_INTEGER + 1 : 0, bad === 'capacity' ? 1025 : 32), metadata());
    assert.notEqual(env.api.error(id), '', bad); env.api.close(id);
  }
  {
    const env = environment(), id = env.api.open(options), probe = env.probes.shift();
    env.api.close(id); probe.resolve({supported: true, config: probe.config}); await turn();
    assert.equal(env.encoders.length, 0, 'Retired probe created an encoder');
  }
  {
    const env = environment(), id = await ready(env), encoder = env.encoders[0];
    env.api.submit(id, new Uint8Array(1), 0, null); env.api.finish(id); env.api.close(id);
    encoder.resolveFlush(); await turn(); assert.equal(encoder.closed, 1);
  }
  console.log('Web H.264 capabilities, no-drop mode, reorder identity, bounded batches, metadata, flush and retirement passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });

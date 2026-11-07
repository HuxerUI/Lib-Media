const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../platform/web/media_codec_decoder.js'), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));
function environment() {
  const probes = [], decoders = [];
  class Decoder {
    static isConfigSupported(config) { return new Promise(resolve => probes.push(() => resolve({supported: true, config}))); }
    constructor(callbacks) { this.callbacks = callbacks; this.state = 'unconfigured'; this.closed = 0; decoders.push(this); }
    configure() { this.state = 'configured'; }
    decode(chunk) { this.chunk = chunk; }
    flush() { return new Promise(resolve => { this.resolveFlush = resolve; }); }
    close() { this.state = 'closed'; ++this.closed; }
  }
  const context = vm.createContext({AudioDecoder: Decoder, VideoDecoder: Decoder, Uint8Array,
    EncodedAudioChunk: class { constructor(data) { Object.assign(this, data); } },
    EncodedVideoChunk: class { constructor(data) { Object.assign(this, data); } }});
  vm.runInContext(source, context);
  return {api: context.HuxerMediaCodec, probes, decoders};
}
function audio() { return {audio: true, rate: 48000, channels: 2, planar: false, sampleFormat: 'f32', description: new Uint8Array([0x11, 0x90])}; }
async function run() {
  const {api, probes, decoders} = environment();
  const canceled = api.open(audio());
  assert.equal(api.ready(canceled), false);
  assert.equal(api.close(canceled), '');
  probes.shift()(); await turn();
  assert.equal(decoders.length, 0, 'Retired support promise created a codec');

  const id = api.open(audio()); probes.shift()(); await turn();
  assert.equal(api.ready(id), true);
  api.submit(id, new Uint8Array([1]), -100, 100, true);
  assert.equal(decoders[0].chunk.timestamp, -100);
  let closed = 0;
  decoders[0].callbacks.output({timestamp: -100, duration: null, sampleRate: 48000, numberOfChannels: 2, numberOfFrames: 2,
    allocationSize() { return 16; }, copyTo(bytes) { bytes.fill(3); }, close() { ++closed; }});
  assert.equal(closed, 1);
  assert.equal(api.poll(id).planes[0][0], 3);
  api.finish(id);
  assert.equal(api.ended(id), false);
  decoders[0].resolveFlush(); await turn();
  assert.equal(api.ended(id), true);
  assert.equal(api.close(id), '');
  assert.equal(decoders[0].closed, 1);
  decoders[0].callbacks.output({close() { ++closed; }});
  assert.equal(closed, 2, 'Late frame was not released');

  const video = api.open({audio: false, width: 4, height: 4, description: new Uint8Array([1,77,0,30])});
  probes.shift()(); await turn();
  let copied, frameClosed = false;
  decoders[1].callbacks.output({timestamp: 0, duration: 1, codedWidth: 4, codedHeight: 4, displayWidth: 4, displayHeight: 4,
    format: 'NV12', visibleRect: {x: 0, y: 0, width: 4, height: 4}, colorSpace: {toJSON() { return {}; }},
    allocationSize() { return 24; }, copyTo() { return new Promise(resolve => { copied = resolve; }); }, close() { frameClosed = true; }});
  api.finish(video);
  api.close(video);
  decoders[1].resolveFlush();
  copied([{offset: 0, stride: 4}, {offset: 16, stride: 4}]);
  await turn();
  assert.equal(frameClosed, true, 'Retired asynchronous copy leaked its platform frame');
  assert.equal(decoders[1].closed, 1);
  assert.equal(api.close(video), '');
  console.log('Web codec pending-probe, copy, flush, and late-callback retirement contracts passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });

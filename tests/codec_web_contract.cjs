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
  const hevc = api.open({audio: false, codec: 'hvc1.2.4.L30.B0', tenBit: true, width: 4, height: 2,
    description: new Uint8Array([1, 2])});
  probes.shift()(); await turn();
  decoders[2].callbacks.output({timestamp: 0, duration: 1, codedWidth: 4, codedHeight: 2, displayWidth: 4, displayHeight: 2,
    format: 'I420P10', visibleRect: {x: 0, y: 0, width: 4, height: 2}, colorSpace: {toJSON() { return {}; }},
    allocationSize() { return 40; },
    async copyTo(bytes) {
      const view = new DataView(bytes.buffer);
      for (const [offset, sample] of [[0, 65], [10, 1023], [24, 100], [26, 101], [32, 500], [34, 501]])
        view.setUint16(offset, sample, true);
      return [{offset: 0, stride: 10}, {offset: 24, stride: 6}, {offset: 32, stride: 6}];
    },
    close() { ++closed; }
  });
  await turn();
  const p010 = api.poll(hevc);
  assert.equal(p010.format, 'P010');
  assert.deepEqual(Array.from(p010.strides), [8, 8]);
  const y = new DataView(p010.planes[0].buffer), uv = new DataView(p010.planes[1].buffer);
  assert.equal(y.getUint16(0, true), 65 << 6);
  assert.equal(y.getUint16(8, true), 1023 << 6);
  assert.deepEqual([0, 2, 4, 6].map(offset => uv.getUint16(offset, true) >>> 6), [100, 500, 101, 501]);
  assert.equal(api.close(hevc), '');

  const platform = api.open({audio: false, platform: true, width: 8, height: 4,
    description: new Uint8Array([1,77,0,30])});
  probes.shift()(); await turn();
  let reads = 0;
  const browserFrame = timestamp => ({timestamp, duration: 1, codedWidth: 8, codedHeight: 6,
    displayWidth: 12, displayHeight: 4, format: null, visibleRect: {x: 2, y: 0, width: 6, height: 4},
    colorSpace: {toJSON() { return {}; }}, closed: 0,
    allocationSize() { ++reads; throw new Error('Unexpected pixel allocation'); },
    copyTo() { ++reads; throw new Error('Unexpected pixel read'); },
    close() { ++this.closed; }});
  const read = browserFrame(0), queued = browserFrame(1);
  const decoder = decoders[3];
  decoder.callbacks.output(read);
  decoder.callbacks.output(queued);
  const retained = api.poll(platform);
  assert.equal(retained.frame, read);
  assert.equal(retained.height, 4, 'Decoder bounds included browser allocation padding');
  assert.equal(retained.planes.length, 0);
  assert.equal(retained.visible.x, 2);
  assert.equal(retained.visible.width, 6);
  assert.equal(retained.aspectWidth / retained.aspectHeight, 2);
  assert.equal(reads, 0);
  api.finish(platform);
  decoder.resolveFlush(); await turn();
  assert.equal(api.ended(platform), false, 'Flush ended with a queued platform frame');
  assert.equal(api.close(platform), '');
  assert.equal(queued.closed, 1, 'Queued platform frame was not closed');
  assert.equal(read.closed, 0, 'Transferred platform frame was closed by the decoder');
  const late = browserFrame(2);
  decoder.callbacks.output(late);
  assert.equal(late.closed, 1, 'Late platform frame was not closed');
  read.close();
  console.log('Web codec pending-probe, CPU copy, platform ownership, flush, and retirement contracts passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../platform/web/playback.js'), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));

const contexts = [];
let now = 100;
let processor, workletScope;
class Context {
  constructor({sampleRate}) {
    this.sampleRate = sampleRate;
    this.state = 'suspended';
    this.outputFrame = 0;
    this.audioWorklet = {addModule: source => new Promise(resolve => {
      this.finishModule = () => {
        workletScope = vm.createContext({
          AudioWorkletProcessor: class { constructor() { this.port = {}; } },
          registerProcessor(_name, type) { processor = type; }
        });
        vm.runInContext(source, workletScope);
        resolve();
      };
    })};
    contexts.push(this);
  }
  getOutputTimestamp() { return {contextTime: this.outputFrame / this.sampleRate, performanceTime: 1}; }
  suspend() { this.state = 'suspended'; return Promise.resolve(); }
  resume() { this.state = 'running'; return Promise.resolve(); }
  close() { return new Promise(resolve => {
    this.finishClose = () => { this.state = 'closed'; resolve(); };
  }); }
}
class Node {
  constructor(context, _name, {processorOptions}) {
    this.output = new processor({processorOptions});
    this.port = {
      postMessage: (data, transfer) => this.output.port.onmessage({data: structuredClone(data, {transfer})}),
      close: () => { this.port.closed = true; }
    };
    this.output.port.postMessage = data => this.port.onmessage?.({data});
    context.node = this;
  }
  connect() {}
  disconnect() { this.disconnected = true; }
  render(start, frames) {
    workletScope.currentFrame = start;
    const output = [new Float32Array(frames)];
    this.output.process([], [output]);
    return [...output[0]];
  }
}
const sandbox = vm.createContext({
  AudioContext: Context, AudioWorkletNode: Node, performance: {now: () => now},
  Blob: class { constructor([value]) { this.source = value; } },
  URL: {createObjectURL: blob => blob.source, revokeObjectURL() {}}
});
vm.runInContext(source, sandbox);

async function run() {
  const api = sandbox.HuxerMediaPlayback;
  const retired = api.open(48000, 1, 4);
  assert.equal(api.close(retired), false);
  contexts[0].finishModule();
  await turn();
  assert.equal(contexts[0].node, undefined, 'A retired setup promise created a worklet');
  contexts[0].finishClose();
  await turn();
  assert.equal(api.close(retired), true);

  const live = api.open(48000, 1, 4);
  const context = contexts[1];
  context.finishModule();
  await turn();
  assert.equal(live.ready, true);

  const resume = context.resume.bind(context);
  const suspend = context.suspend.bind(context);
  let rejectOldResume, suspended = 0;
  context.resume = () => new Promise((_resolve, reject) => { rejectOldResume = reject; });
  context.suspend = () => { ++suspended; return suspend(); };
  assert.equal(api.transport(live, true), false);
  assert.equal(api.transport(live, false), false);
  assert.equal(suspended, 1, 'Pause did not reach the already suspended context');
  await turn();
  assert.equal(api.transport(live, false), true);
  now += 5100;
  assert.equal(api.error(live), '', 'A canceled resume still timed out');
  context.resume = resume;
  assert.equal(api.transport(live, true), false);
  rejectOldResume(new Error('Late resume rejection'));
  await turn();
  assert.equal(api.error(live), '', 'A canceled resume changed the new transport request');
  assert.equal(api.transport(live, true), true);
  assert.equal(api.write(live, new Int16Array([8192, 16384, -32768])), 3);
  assert.deepEqual(context.node.render(0, 2), [0.25, 0.5]);
  assert.equal(api.observe(live), 0, 'Rendering alone confirmed output');
  context.outputFrame = 1;
  assert.equal(api.observe(live), 1);
  assert.deepEqual(context.node.render(2, 4), [-1, 0, 0, 0]);
  assert.equal(api.write(live, new Int16Array([-8192, 16384, 32767, 8192])), 4);
  assert.deepEqual(context.node.render(6, 5), [-0.25, 0.5, 32767 / 32768, 0.25, 0]);
  context.outputFrame = 5;
  assert.equal(api.observe(live), 3, 'Underrun silence counted as media');
  context.outputFrame = 10;
  assert.equal(api.observe(live), 7);
  context.outputFrame = 4;
  assert.equal(api.observe(live), 7, 'A stale timestamp moved the clock backwards');

  assert.equal(api.write(live, new Int16Array([1, 2])), 2);
  context.node.render(11, 2);
  context.outputFrame = 11;
  assert.equal(api.observe(live), 7);
  now += 4000;
  assert.equal(api.transport(live, false), false);
  await turn();
  assert.equal(api.transport(live, false), true);
  now += 6000;
  assert.equal(api.observe(live), 7);
  assert.equal(api.error(live), '', 'Time spent paused caused a clock failure');

  assert.equal(api.transport(live, true), false);
  await turn();
  assert.equal(api.transport(live, true), true);
  assert.equal(api.observe(live), 7);
  now += 4999;
  context.outputFrame = 12;
  assert.equal(api.observe(live), 8, 'The clock did not resume from the confirmed position');
  now += 4999;
  assert.equal(api.observe(live), 8);
  assert.equal(api.error(live), '', 'Valid progress did not restart the clock deadline');
  ++now;
  assert.equal(api.observe(live), 8);
  assert.match(api.error(live), /output clock stalled/);

  const lateReport = context.node.port.onmessage;
  assert.equal(api.close(live), false);
  assert.equal(context.node.port.closed && context.node.disconnected, true);
  lateReport({data: [11, 1]});
  assert.equal(live.ranges.length, 0, 'A retired worklet reported new output');
  context.finishClose();
  await turn();
  assert.equal(api.close(live), true);
  console.log('Web PCM render ranges, silence, canceled startup, stalled clock, and retirement passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });

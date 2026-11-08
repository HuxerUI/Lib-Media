const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../platform/web/media_codec_capabilities.js'), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));
function environment(available = true) {
  const probes = [];
  class Codec {
    constructor() { throw new Error('A capability query created a WebCodecs session'); }
    static isConfigSupported(config) { return new Promise((resolve, reject) => probes.push({config, resolve, reject})); }
  }
  const context = vm.createContext(available ? {AudioDecoder: Codec, AudioEncoder: Codec,
    VideoDecoder: Codec, VideoEncoder: Codec, Uint8Array} : {});
  vm.runInContext(source, context);
  return {api: context.HuxerMediaCodecCapabilities, probes};
}
async function run() {
  const missing = environment(false);
  for (const kind of [0, 1, 2, 3]) {
    const id = missing.api.open(kind);
    assert.equal(missing.api.poll(id).availability, 1);
    missing.api.close(id);
  }
  const env = environment();
  const retired = env.api.open(3); env.api.close(retired); await turn();
  assert.equal(env.probes.length, 0, 'A retired query still scheduled support checks');
  assert.match(env.api.poll(retired).error, /closed/);
  assert.equal(env.api.open(4), 0);

  for (const kind of [0, 1, 2, 3]) {
    const id = env.api.open(kind); await turn();
    assert.equal(env.api.poll(id), null, 'Pending support checks were reported as complete');
    assert.equal(env.probes.length, kind === 0 || kind === 2 ? 4 : 3, 'Query performed an unbounded configuration search');
    for (const probe of env.probes.splice(0)) probe.resolve({supported: false, config: probe.config});
    await turn();
    const report = env.api.poll(id);
    assert.equal(report.availability, 0, 'One failed reference configuration ruled out the codec family');
    assert.ok(report.profiles.every(value => value === 0));
    assert.ok(report.rates.every(value => value === 0));
    env.api.close(id);
  }
  const video = env.api.open(3); await turn();
  const profiles = env.probes.splice(0);
  profiles[0].resolve({supported: true, config: profiles[0].config});
  profiles[1].reject(new Error('No profile metadata'));
  profiles[2].resolve({supported: false, config: profiles[2].config});
  await turn();
  assert.deepEqual(Array.from(env.api.poll(video).profiles), [2, 0, 0]);
  assert.equal(env.api.poll(video).availability, 2);
  env.api.close(video);

  const audio = env.api.open(2); await turn();
  const rates = env.probes.splice(0);
  for (const probe of rates) {
    if (probe.config.sampleRate === 48000 && probe.config.numberOfChannels === 2)
      probe.resolve({supported: true, config: probe.config});
    else probe.resolve({supported: true, config: {...probe.config, sampleRate: 32000}});
  }
  await turn();
  assert.deepEqual(Array.from(env.api.poll(audio).rates), [0, 2]);
  assert.deepEqual(Array.from(env.api.poll(audio).channels), [0, 2]);
  env.api.close(audio);

  const pending = env.api.open(1); await turn();
  const late = env.probes.splice(0); env.api.close(pending);
  for (const probe of late) probe.resolve({supported: true, config: probe.config});
  await turn();
  assert.match(env.api.poll(pending).error, /closed/, 'Late promises revived a closed query');
  assert.equal(env.api.close(pending), '');
  console.log('Web capability evidence, bounded checks and promise retirement passed');
}
run().catch(error => { console.error(error); process.exitCode = 1; });

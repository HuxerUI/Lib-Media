// Promise continuations retain JS query state only; closing a query retires every late result.
(function(root) {
  'use strict';
  const queries = new Map();
  let nextId = 1;
  const unknown = 0, unsupported = 1, supported = 2;
  const decoderRates = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350];
  function sameConfiguration(actual, expected, fields) {
    return actual && fields.every(field => actual[field] === expected[field]);
  }
  async function query(state, kind) {
    const audio = kind === 0 || kind === 2, encoding = kind === 2 || kind === 3;
    const codec = root[(audio ? 'Audio' : 'Video') + (encoding ? 'Encoder' : 'Decoder')];
    const result = {availability: unknown, representations: unknown, diagnostic: '',
      rates: (encoding ? [44100, 48000] : decoderRates).map(() => unknown),
      channels: [unknown, unknown], profiles: [unknown, unknown, unknown]};
    if (!codec || typeof codec.isConfigSupported !== 'function') {
      result.availability = unsupported;
      result.diagnostic = 'The required WebCodecs API is unavailable';
    } else {
      result.representations = supported;
      result.diagnostic = 'Positive evidence uses small reference configurations; WebCodecs does not enumerate global limits or hardware use';
      const checks = [];
      if (audio) {
        for (const rate of [44100, 48000]) for (const channels of [1, 2]) {
          const config = {codec: 'mp4a.40.2', sampleRate: rate, numberOfChannels: channels};
          if (encoding) { config.bitrate = 128000; config.aac = {format: 'aac'}; }
          else config.description = new Uint8Array([rate === 44100 ? 0x12 : 0x11, (rate === 44100 ? 0 : 0x80) | (channels << 3)]);
          checks.push(Promise.resolve().then(() => state.active ? codec.isConfigSupported(config) : null).then(answer => {
            if (!state.active || !answer || !answer.supported ||
                !sameConfiguration(answer.config, config, ['codec', 'sampleRate', 'numberOfChannels'])) return;
            if (encoding && (!sameConfiguration(answer.config, config, ['bitrate']) ||
                !answer.config.aac || answer.config.aac.format !== 'aac')) return;
            result.availability = supported;
            result.rates[(encoding ? [44100, 48000] : decoderRates).indexOf(rate)] = supported;
            result.channels[channels - 1] = supported;
          }).catch(() => {}));
        }
      } else {
        for (const [index, profile] of [66, 77, 100].entries()) {
          const config = {codec: 'avc1.' + profile.toString(16) + (profile === 66 ? 'e0' : '00') + '1f'};
          if (encoding) Object.assign(config, {width: 320, height: 240, framerate: 30,
            bitrate: 1000000, latencyMode: 'quality', avc: {format: 'avc'}});
          else Object.assign(config, {codedWidth: 320, codedHeight: 240});
          checks.push(Promise.resolve().then(() => state.active ? codec.isConfigSupported(config) : null).then(answer => {
            if (!state.active || !answer || !answer.supported || !sameConfiguration(answer.config, config,
                encoding ? ['codec', 'width', 'height', 'framerate', 'bitrate', 'latencyMode'] : ['codec', 'codedWidth', 'codedHeight'])) return;
            if (encoding && (!answer.config.avc || answer.config.avc.format !== 'avc')) return;
            result.availability = supported; result.profiles[index] = supported;
          }).catch(() => {}));
        }
      }
      await Promise.all(checks);
    }
    if (state.active) state.result = result;
  }
  root.HuxerMediaCodecCapabilities = {
    open(kind) {
      if (!Number.isInteger(kind) || kind < 0 || kind > 3 || nextId > 0xffffffff) return 0;
      const id = nextId++, state = {active: true, result: null, error: ''};
      queries.set(id, state);
      query(state, kind).catch(error => { if (state.active) state.error = String(error && error.message || error); });
      return id;
    },
    poll(id) {
      const state = queries.get(id);
      if (!state) return {error: 'Codec capability query is closed'};
      if (state.error) return {error: state.error};
      return state.result;
    },
    close(id) {
      const state = queries.get(id);
      if (state) { state.active = false; state.result = null; queries.delete(id); }
      return '';
    }
  };
})(globalThis);

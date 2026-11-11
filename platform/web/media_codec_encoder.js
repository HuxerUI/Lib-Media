// Encoder sessions belong to the creating event loop. Late callbacks and promises retain only retired JS state.
(function(root) {
  'use strict';
  const sessions = new Map();
  let nextId = 1;
  function fail(state, error) {
    if (state.active) state.error = String(error && error.message || error);
  }
  function output(state, chunk, metadata) {
    if (!state.active || state.error) return;
    try {
      if (!state.allowance || !Number.isSafeInteger(chunk.timestamp) || !chunk.byteLength || chunk.byteLength > 65536)
        throw new Error('AAC output exceeds reserved packet capacity or timestamp range');
      --state.allowance;
      const config = metadata && metadata.decoderConfig;
      if (config) {
        if (config.codec !== 'mp4a.40.2' || config.sampleRate !== state.rate || config.numberOfChannels !== state.channels || !config.description)
          throw new Error('WebCodecs changed the AAC-LC stream format');
        const source = config.description;
        const description = ArrayBuffer.isView(source)
          ? new Uint8Array(source.buffer, source.byteOffset, source.byteLength) : new Uint8Array(source);
        if (description.length !== 2 && description.length !== 5) throw new Error('Unsupported AAC initialization data');
        if (state.description && (state.description.length !== description.length || state.description.some((v, i) => v !== description[i])))
          throw new Error('AAC initialization data changed during encoding');
        if (!state.description) {
          state.description = description.slice();
          state.output.push({kind: 'format', description: state.description});
        }
      }
      if (!state.description) throw new Error('AAC output precedes initialization data');
      const data = new Uint8Array(chunk.byteLength);
      chunk.copyTo(data);
      state.output.push({kind: 'packet', data, pts: chunk.timestamp});
    } catch (error) { fail(state, error); }
  }
  root.HuxerMediaAudioEncoder = {
    open(options) {
      const id = nextId++;
      const state = {active: true, ready: false, flushed: false, error: '', output: [], allowance: 0,
        started: false, rate: options.rate, channels: options.channels};
      sessions.set(id, state);
      if (!root.AudioEncoder || !root.AudioData) { fail(state, 'WebCodecs AudioEncoder/AudioData is unavailable'); return id; }
      const config = {codec: 'mp4a.40.2', sampleRate: options.rate, numberOfChannels: options.channels,
        bitrate: options.bitrate, aac: {format: 'aac'}};
      try {
        root.AudioEncoder.isConfigSupported(config).then(support => {
          if (!state.active) return;
          if (!support.supported || support.config.codec !== config.codec || support.config.sampleRate !== config.sampleRate ||
              support.config.numberOfChannels !== config.numberOfChannels || support.config.bitrate !== config.bitrate)
            throw new Error('WebCodecs does not support the AAC encoder configuration');
          state.encoder = new root.AudioEncoder({output: (chunk, metadata) => output(state, chunk, metadata), error: error => fail(state, error)});
          state.encoder.configure(config);
          state.ready = true;
        }).catch(error => fail(state, error));
      } catch (error) { fail(state, error); }
      return id;
    },
    ready(id) { return !!sessions.get(id)?.ready; },
    error(id) { return sessions.get(id)?.error || ''; },
    submit(id, data, pts, samples) {
      const state = sessions.get(id);
      if (state.encoder.encodeQueueSize >= 8) return false;
      let frame;
      try {
        if (!Number.isSafeInteger(pts)) throw new Error('PCM timestamp exceeds WebCodecs safe integer range');
        frame = new root.AudioData({format: 's16', sampleRate: state.rate, numberOfFrames: samples,
          numberOfChannels: state.channels, timestamp: pts, data});
        state.allowance += 1 + (state.started ? 0 : 8);
        state.started = true;
        state.encoder.encode(frame);
      } catch (error) { fail(state, error); }
      finally { if (frame) frame.close(); }
      return true;
    },
    poll(id) { return sessions.get(id).output.shift() || null; },
    finish(id) {
      const state = sessions.get(id);
      try { state.encoder.flush().then(() => { if (state.active) state.flushed = true; }).catch(error => fail(state, error)); }
      catch (error) { fail(state, error); }
    },
    ended(id) { const state = sessions.get(id); return state.flushed && !state.output.length; },
    close(id) {
      const state = sessions.get(id);
      if (!state) return '';
      state.active = false;
      state.output.length = 0;
      state.description = null;
      let error = '';
      try { if (state.encoder && state.encoder.state !== 'closed') state.encoder.close(); }
      catch (failure) { error = String(failure.message || failure); }
      state.encoder = null;
      sessions.delete(id);
      return error;
    }
  };
})(globalThis);

// Video callbacks retain only JS session state. Output admission is reserved before encode().
(function(root) {
  'use strict';
  const sessions = new Map();
  let nextId = 1;
  function fail(state, error) {
    if (state.active && !state.error) state.error = String(error && error.message || error);
  }
  function codec(options) {
    if (options.hevc) {
      const picture = options.width * options.height;
      const rate = picture * options.rateNumerator / options.rateDenominator;
      // HEVC Main-tier bounds from Annex A for luma samples, sample rate, and bit rate.
      const levels = [
        [30, 36864, 552960, 128000], [60, 122880, 3686400, 1500000], [63, 245760, 7372800, 3000000],
        [90, 552960, 16588800, 6000000], [93, 983040, 33177600, 10000000],
        [120, 2228224, 66846720, 12000000], [123, 2228224, 133693440, 20000000],
        [150, 8912896, 267386880, 25000000], [153, 8912896, 534773760, 40000000],
        [156, 8912896, 1069547520, 60000000], [180, 35651584, 1069547520, 60000000],
        [183, 35651584, 2139095040, 120000000], [186, 35651584, 4278190080, 240000000]
      ];
      const edge = Math.max(options.width, options.height);
      const level = levels.find(value => picture <= value[1] && edge * edge <= 8 * value[1] &&
        rate <= value[2] && options.bitrate <= value[3]);
      if (!level) throw new Error('HEVC geometry, frame rate, or bit rate exceeds Main-tier limits');
      return 'hvc1.' + (options.layout === 'P010' ? '2.4' : '1.6') + '.L' + level[0] + '.B0';
    }
    const blocks = Math.ceil(options.width / 16) * Math.ceil(options.height / 16);
    const rate = blocks * options.rateNumerator / options.rateDenominator;
    const bitrate = options.bitrate / (options.profile === 100 ? 1.25 : 1);
    const levels = [
      [10, 1485, 99, 64000], [11, 3000, 396, 192000], [12, 6000, 396, 384000],
      [13, 11880, 396, 768000], [20, 11880, 396, 2000000], [21, 19800, 792, 4000000],
      [22, 20250, 1620, 4000000], [30, 40500, 1620, 10000000], [31, 108000, 3600, 14000000],
      [32, 216000, 5120, 20000000], [40, 245760, 8192, 20000000], [41, 245760, 8192, 50000000],
      [42, 522240, 8704, 50000000], [50, 589824, 22080, 135000000], [51, 983040, 36864, 240000000],
      [52, 2073600, 36864, 240000000], [60, 4177920, 139264, 240000000],
      [61, 8355840, 139264, 480000000], [62, 16711680, 139264, 800000000]
    ];
    const level = levels.find(value => rate <= value[1] && blocks <= value[2] && bitrate <= value[3]);
    if (!level) throw new Error('H.264 geometry/rate/bitrate exceeds the supported level table');
    return 'avc1.' + options.profile.toString(16).padStart(2, '0') +
      (options.profile === 66 ? 'e0' : '00') + level[0].toString(16).padStart(2, '0');
  }
  function planar10(data, width, height) {
    const luma = width * height;
    const chroma = luma / 4;
    const packed = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const planes = new Uint8Array(data.byteLength);
    const output = new DataView(planes.buffer);
    for (let i = 0; i < luma; ++i)
      output.setUint16(i * 2, packed.getUint16(i * 2, true) >>> 6, true);
    for (let i = 0; i < chroma; ++i) {
      output.setUint16((luma + i) * 2, packed.getUint16(luma * 2 + i * 4, true) >>> 6, true);
      output.setUint16((luma + chroma + i) * 2, packed.getUint16(luma * 2 + i * 4 + 2, true) >>> 6, true);
    }
    return planes;
  }
  function output(state, chunk, metadata) {
    if (!state.active || state.error) return;
    try {
      if (!state.pending || !Number.isSafeInteger(chunk.timestamp) || !chunk.byteLength || chunk.byteLength > state.options.maximum)
        throw new Error('Video output exceeds reserved capacity or timestamp range');
      --state.pending;
      const config = metadata && metadata.decoderConfig;
      if (config) {
        const codec = state.options.hevc ? /^(hvc1|hev1)\./i : /^avc[13]\./i;
        if (!config.description || !codec.test(config.codec))
          throw new Error('WebCodecs did not provide initialization data for the requested video codec');
        const source = config.description;
        const description = ArrayBuffer.isView(source)
          ? new Uint8Array(source.buffer, source.byteOffset, source.byteLength) : new Uint8Array(source);
        if (!description.length || description.length > 65536) throw new Error('Video initialization data exceeds 64 KiB');
        if (state.description && (state.description.length !== description.length ||
            state.description.some((value, i) => value !== description[i])))
          throw new Error('WebCodecs video configuration changed');
        if (!state.description) {
          state.description = description.slice();
          state.output.push({kind: 'format', description: state.description});
        }
      }
      if (!state.description) throw new Error('Video output preceded initialization data');
      const data = new Uint8Array(chunk.byteLength);
      chunk.copyTo(data);
      state.output.push({kind: 'packet', data, pts: chunk.timestamp});
    } catch (error) { fail(state, error); }
  }
  function flush(state, final) {
    if (final) state.finishing = true;
    if (state.flushing || state.flushed || (!state.pending && !final)) return;
    state.flushing = true;
    try {
      state.encoder.flush().then(() => {
        if (!state.active) return;
        state.flushing = false;
        if (state.pending) throw new Error('WebCodecs flush lost accepted video pictures');
        if (state.finishing) state.flushed = true;
      }).catch(error => fail(state, error));
    } catch (error) { fail(state, error); }
  }
  root.HuxerMediaVideoEncoder = {
    open(options) {
      const id = nextId++;
      const state = {options, active: true, ready: false, error: '', output: [], pending: 0, submitted: 0,
        first: true, keyRequested: false, flushing: false, finishing: false, flushed: false};
      sessions.set(id, state);
      try {
        if (!root.VideoEncoder || !root.VideoFrame) throw new Error('WebCodecs VideoEncoder/VideoFrame is unavailable');
        if (options.maxB !== null && (options.hevc || options.profile !== 66 || options.maxB !== 0))
          throw new Error('WebCodecs cannot enforce the requested numeric B-picture bound');
        const config = {codec: codec(options), width: options.width, height: options.height,
          framerate: options.rateNumerator / options.rateDenominator, bitrate: options.bitrate,
          latencyMode: 'quality', ...(options.hevc ? {hevc: {format: 'hevc'}} : {avc: {format: 'avc'}})};
        root.VideoEncoder.isConfigSupported(config).then(support => {
          if (!state.active) return;
          if (!support.supported || support.config.codec.toLowerCase() !== config.codec.toLowerCase() ||
              support.config.width !== config.width || support.config.height !== config.height ||
              support.config.latencyMode !== 'quality' ||
              (options.hevc ? (support.config.hevc?.format ?? 'hevc') !== 'hevc' : support.config.avc?.format !== 'avc'))
            throw new Error('WebCodecs does not support the requested video configuration');
          if (options.layout === 'P010') {
            const probe = new root.VideoFrame(new Uint8Array(12), {format: 'I420P10', codedWidth: 2, codedHeight: 2, timestamp: 0});
            probe.close();
          }
          state.encoder = new root.VideoEncoder({output: (chunk, metadata) => output(state, chunk, metadata),
            error: error => fail(state, error)});
          state.encoder.configure(config); state.ready = true;
        }).catch(error => fail(state, error));
      } catch (error) { fail(state, error); }
      return id;
    },
    ready(id) { return !!sessions.get(id)?.ready; },
    error(id) { return sessions.get(id)?.error || ''; },
    submit(id, data, pts, duration) {
      const state = sessions.get(id), options = state.options;
      if (state.flushing || state.pending >= options.window || state.encoder.encodeQueueSize >= options.window) return false;
      let frame;
      try {
        if (!Number.isSafeInteger(pts) || (duration !== null && (!Number.isSafeInteger(duration) || duration <= 0)))
          throw new Error('Video timing exceeds the WebCodecs safe integer range');
        const init = {timestamp: pts};
        if (duration !== null) init.duration = duration;
        if (options.layout === 'Opaque') {
          frame = new root.VideoFrame(data, init);
        } else {
          const tenBit = options.layout === 'P010';
          frame = new root.VideoFrame(tenBit ? planar10(data, options.width, options.height) : data, {...init,
            format: tenBit ? 'I420P10' : options.layout, codedWidth: options.width, codedHeight: options.height,
            colorSpace: options.color});
        }
        const key = state.first || state.keyRequested || state.submitted % options.keyInterval === 0;
        ++state.pending; ++state.submitted;
        state.encoder.encode(frame, {keyFrame: key});
        state.first = state.keyRequested = false;
      } catch (error) { fail(state, error); }
      finally { if (frame) frame.close(); }
      return true;
    },
    poll(id) { return sessions.get(id).output.shift() || null; },
    requestKeyFrame(id) { sessions.get(id).keyRequested = true; },
    progress(id) { flush(sessions.get(id), false); },
    finish(id) { flush(sessions.get(id), true); },
    ended(id) { const state = sessions.get(id); return state.flushed && !state.output.length; },
    close(id) {
      const state = sessions.get(id);
      if (!state) return '';
      state.active = false; state.output.length = 0; state.description = null;
      let error = '';
      try { if (state.encoder && state.encoder.state !== 'closed') state.encoder.close(); }
      catch (failure) { error = String(failure.message || failure); }
      state.encoder = null; sessions.delete(id);
      return error;
    }
  };
})(globalThis);

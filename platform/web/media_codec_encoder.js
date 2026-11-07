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

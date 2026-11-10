// Each decoder session belongs to this JS event loop. Platform callbacks never call into WASM.
// Retired promises retain only JS state; they cannot publish output or access application captures.
(function(root) {
  'use strict';
  const sessions = new Map();
  let nextId = 1;
  function failure(state, error) {
    if (state.active) state.error = String(error && error.message || error);
  }
  async function output(state, frame) {
    if (!state.active) { frame.close(); return; }
    ++state.copying;
    let retained = false;
    try {
      const record = {pts: frame.timestamp, duration: frame.duration ?? null, planes: []};
      if (state.audio) {
        if (frame.sampleRate !== state.rate || frame.numberOfChannels !== state.channels ||
            frame.numberOfFrames > (state.maxSamples || 1024))
          throw new Error('WebCodecs changed the configured PCM rate/layout or frame bound');
        record.samples = frame.numberOfFrames;
        for (let i = 0; i < (state.planar ? state.channels : 1); ++i) {
          const options = {planeIndex: i, format: state.sampleFormat};
          const bytes = new Uint8Array(frame.allocationSize(options));
          frame.copyTo(bytes, options);
          record.planes.push(bytes);
        }
      } else {
        // The browser frame may include allocation padding beyond the stream's coded bounds.
        const width = Math.min(frame.codedWidth, state.width), height = Math.min(frame.codedHeight, state.height);
        if (frame.visibleRect.x + frame.visibleRect.width > width || frame.visibleRect.y + frame.visibleRect.height > height)
          throw new Error('WebCodecs visible frame exceeds configured dimensions');
        record.width = width; record.height = height;
        record.visible = {x: frame.visibleRect.x, y: frame.visibleRect.y, width: frame.visibleRect.width, height: frame.visibleRect.height};
        record.format = frame.format;
        record.color = frame.colorSpace.toJSON();
        record.aspectWidth = frame.displayWidth * frame.visibleRect.height;
        record.aspectHeight = frame.displayHeight * frame.visibleRect.width;
        if (state.platform) {
          record.frame = frame;
          state.output.push(record);
          retained = true;
          return;
        }
        const planar10 = frame.format === 'I420P10';
        if (state.tenBit ? !planar10 : frame.format !== 'I420' && frame.format !== 'NV12')
          throw new Error('WebCodecs did not expose the requested CPU YUV bit depth (' + frame.format + ')');
        const options = {rect: {x: 0, y: 0, width, height}};
        const bytes = new Uint8Array(frame.allocationSize(options));
        const layouts = await frame.copyTo(bytes, options);
        const count = frame.format === 'NV12' ? 2 : 3;
        if (layouts.length !== count) throw new Error('Invalid WebCodecs plane layout');
        if (planar10) {
          const y = new Uint8Array(width * height * 2);
          const cw = Math.ceil(width / 2), ch = Math.ceil(height / 2);
          const uv = new Uint8Array(cw * ch * 4);
          const source = new DataView(bytes.buffer);
          const luma = new DataView(y.buffer), chroma = new DataView(uv.buffer);
          const read = (plane, row, x) => source.getUint16(layouts[plane].offset + row * layouts[plane].stride + x * 2, true);
          for (let row = 0; row < height; ++row) for (let x = 0; x < width; ++x) {
            luma.setUint16((row * width + x) * 2, read(0, row, x) << 6, true);
          }
          for (let row = 0; row < ch; ++row) for (let x = 0; x < cw; ++x) for (let plane = 1; plane < 3; ++plane) {
            chroma.setUint16((row * cw + x) * 4 + (plane - 1) * 2, read(plane, row, x) << 6, true);
          }
          record.format = 'P010';
          record.planes = [y, uv];
          record.strides = [width * 2, cw * 4];
        } else {
          record.strides = [];
          for (let i = 0; i < count; ++i) {
            const rows = i ? Math.ceil(height / 2) : height;
            const rowBytes = i ? Math.ceil(width / 2) * (count === 2 ? 2 : 1) : width;
            const size = layouts[i].stride * (rows - 1) + rowBytes;
            record.planes.push(bytes.slice(layouts[i].offset, layouts[i].offset + size));
            record.strides.push(layouts[i].stride);
          }
        }
      }
      if (state.active) state.output.push(record);
    } catch (error) { failure(state, error); }
    finally { if (!retained) frame.close(); --state.copying; }
  }
  root.HuxerMediaCodec = {
    open(config) {
      const id = nextId++;
      const state = {...config, active: true, ready: false, error: '', copying: 0, output: [], flushed: false, decoder: null};
      sessions.set(id, state);
      const Type = config.audio ? root.AudioDecoder : root.VideoDecoder;
      if (!Type) { failure(state, 'WebCodecs decoder is unavailable'); return id; }
      const description = config.description;
      const codecConfig = config.audio
        ? {codec: config.codec || 'mp4a.40.2', sampleRate: config.rate, numberOfChannels: config.channels,
           ...(description.length ? {description} : {})}
        : {codec: config.codec || 'avc1.' + Array.from(description.slice(1, 4), x => x.toString(16).padStart(2, '0')).join(''),
           codedWidth: config.width, codedHeight: config.height, description};
      Type.isConfigSupported(codecConfig).then(support => {
        if (!state.active) return;
        if (!support.supported) throw new Error('WebCodecs does not support this configuration');
        state.decoder = new Type({output: frame => output(state, frame), error: error => failure(state, error)});
        state.decoder.configure(codecConfig);
        state.ready = true;
      }).catch(error => failure(state, error));
      return id;
    },
    ready(id) { const s = sessions.get(id); return s && s.ready; },
    error(id) { return sessions.get(id)?.error || ''; },
    submit(id, bytes, pts, duration, key) {
      const state = sessions.get(id);
      try {
        if (!Number.isSafeInteger(pts) || (duration !== null && !Number.isSafeInteger(duration)))
          throw new Error('Timestamp exceeds WebCodecs safe integer range');
        const Type = state.audio ? root.EncodedAudioChunk : root.EncodedVideoChunk;
        const data = {type: state.audio || key ? 'key' : 'delta', timestamp: pts, data: bytes};
        if (duration !== null) data.duration = duration;
        state.decoder.decode(new Type(data));
      } catch (error) { failure(state, error); }
    },
    poll(id) { return sessions.get(id).output.shift() || null; },
    finish(id) {
      const state = sessions.get(id);
      try { state.decoder.flush().then(() => { if (state.active) state.flushed = true; }).catch(error => failure(state, error)); }
      catch (error) { failure(state, error); }
    },
    ended(id) { const s = sessions.get(id); return s.flushed && !s.copying && !s.output.length; },
    close(id) {
      const state = sessions.get(id);
      if (!state) return '';
      state.active = false;
      for (const record of state.output) record.frame?.close();
      state.output.length = 0;
      let error = '';
      try { if (state.decoder && state.decoder.state !== 'closed') state.decoder.close(); }
      catch (e) { error = String(e.message || e); }
      state.decoder = null;
      sessions.delete(id);
      return error;
    }
  };
})(globalThis);

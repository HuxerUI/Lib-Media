// The worklet reports rendered media ranges. Only an output timestamp can confirm their presentation.
(function(root) {
  'use strict';
  const worklet = `
    class MediaPcmOutput extends AudioWorkletProcessor {
      constructor({processorOptions: {channels, capacity}}) {
        super();
        this.channels = channels;
        this.samples = new Int16Array(capacity * channels);
        this.read = 0;
        this.write = 0;
        this.queued = 0;
        this.port.onmessage = ({data}) => {
          if (data.length > this.samples.length - this.queued) {
            this.port.postMessage('Web Audio PCM queue overflow');
            return;
          }
          const first = Math.min(data.length, this.samples.length - this.write);
          this.samples.set(data.subarray(0, first), this.write);
          this.samples.set(data.subarray(first));
          this.write = (this.write + data.length) % this.samples.length;
          this.queued += data.length;
        };
      }
      process(_inputs, [output]) {
        if (!output.length) return true;
        const count = Math.min(this.queued / this.channels, output[0].length);
        for (let frame = 0; frame < count; ++frame) {
          for (let channel = 0; channel < this.channels; ++channel)
            output[channel][frame] = this.samples[this.read + channel] / 32768;
          this.read += this.channels;
          if (this.read === this.samples.length) this.read = 0;
        }
        this.queued -= count * this.channels;
        if (count) this.port.postMessage([currentFrame, count]);
        return true;
      }
    }
    registerProcessor('huxer-media-pcm', MediaPcmOutput);
  `;
  const timeout = 5000;
  const message = error => String(error && error.message || error);
  function fail(state, error) {
    if (state.active && !state.error) state.error = message(error);
  }
  function revokeModule(state) {
    if (state.moduleUrl) root.URL.revokeObjectURL(state.moduleUrl);
    state.moduleUrl = null;
  }
  function receive(state, data) {
    if (!state.active) return;
    if (typeof data === 'string') { fail(state, data); return; }
    const [start, count] = data;
    const end = start + count;
    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || count <= 0 || start < state.lastRenderEnd) {
      fail(state, 'Invalid Web Audio render range');
      return;
    }
    state.lastRenderEnd = end;
    const last = state.ranges.at(-1);
    if (last && last.end === start) last.end = end;
    else state.ranges.push({start, end});
  }
  root.HuxerMediaPlayback = {
    open(rate, channels, capacity) {
      const state = {
        rate, channels, active: true, ready: false, closed: false,
        error: '', closeError: '', deadline: performance.now() + timeout,
        context: null, node: null, moduleUrl: null, transition: null, running: false,
        ranges: [], lastRenderEnd: 0, presented: 0, unconfirmed: 0, confirmationDeadline: 0
      };
      try {
        if (!root.AudioContext || !root.AudioWorkletNode)
          throw new Error('AudioWorklet requires a supported browser and a secure context');
        const context = state.context = new root.AudioContext({sampleRate: rate});
        if (!context.audioWorklet || !context.getOutputTimestamp || context.sampleRate !== rate)
          throw new Error('Web Audio cannot provide the requested PCM rate and output timestamp');
        state.moduleUrl = root.URL.createObjectURL(new root.Blob([worklet], {type: 'text/javascript'}));
        Promise.all([context.suspend(), context.audioWorklet.addModule(state.moduleUrl)]).then(() => {
          if (!state.active) return;
          const node = state.node = new root.AudioWorkletNode(context, 'huxer-media-pcm', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [channels],
            processorOptions: {channels, capacity}
          });
          node.port.onmessage = ({data}) => receive(state, data);
          node.onprocessorerror = () => fail(state, 'AudioWorklet processor failed');
          context.onstatechange = () => {
            if (context.state === 'closed' || context.state === 'interrupted' ||
                (state.running && !state.transition && context.state !== 'running'))
              fail(state, 'Web Audio output was interrupted');
          };
          node.connect(context.destination);
          state.ready = true;
          state.deadline = 0;
        }).catch(error => fail(state, error)).finally(() => revokeModule(state));
      } catch (error) { fail(state, error); }
      return state;
    },
    error(state) {
      if (state.deadline && performance.now() >= state.deadline) {
        if (!state.ready) fail(state, 'AudioWorklet setup did not complete');
        else if (state.transition?.running)
          fail(state, 'Web Audio did not start. Start from a user gesture.');
        else fail(state, 'Web Audio did not acknowledge pause');
      }
      return state.error;
    },
    write(state, samples) {
      if (state.error) return 0;
      try {
        // Copy before returning to WASM. The transferred buffer never aliases its heap.
        const copy = samples.slice();
        const frames = copy.length / state.channels;
        state.node.port.postMessage(copy, [copy.buffer]);
        state.unconfirmed += frames;
        return frames;
      } catch (error) { fail(state, error); return 0; }
    },
    transport(state, running) {
      if (state.error || state.transition?.running === running) return false;
      const context = state.context;
      if (!state.transition && context.state === (running ? 'running' : 'suspended')) {
        state.running = running;
        return true;
      }
      // A suspend request must reach the context even while a blocked resume still reports suspended.
      const command = state.transition = {running};
      const current = () => state.active && state.transition === command;
      state.deadline = performance.now() + timeout;
      state.confirmationDeadline = 0;
      try {
        (running ? context.resume() : context.suspend()).then(() => {
          if (!current()) return;
          state.running = running;
          state.transition = null;
          state.deadline = 0;
        }, error => { if (current()) fail(state, error); });
      } catch (error) { fail(state, error); }
      return false;
    },
    observe(state) {
      if (!state.ready || state.error) return state.presented;
      const previous = state.presented;
      if (state.ranges.length) {
        try {
          const stamp = state.context.getOutputTimestamp();
          const frame = Math.floor(stamp.contextTime * state.rate);
          if (Number.isSafeInteger(frame) && Number.isFinite(stamp.performanceTime) && stamp.performanceTime > 0) {
            while (state.ranges.length && state.ranges[0].start < frame) {
              const range = state.ranges[0];
              const confirmed = Math.min(range.end, frame);
              state.presented += confirmed - range.start;
              range.start = confirmed;
              if (confirmed === range.end) state.ranges.shift();
            }
          }
        } catch (error) { fail(state, error); }
      }
      const confirmed = state.presented - previous;
      state.unconfirmed -= confirmed;
      if (!state.running || state.transition || !state.unconfirmed) state.confirmationDeadline = 0;
      else {
        const now = performance.now();
        if (confirmed || !state.confirmationDeadline) state.confirmationDeadline = now + timeout;
        else if (now >= state.confirmationDeadline) fail(state, 'Web Audio output clock stalled');
      }
      return state.presented;
    },
    close(state) {
      if (!state.active) return state.closed;
      state.active = false;
      state.ranges.length = 0;
      revokeModule(state);
      const context = state.context;
      if (context) context.onstatechange = null;
      if (state.node) {
        state.node.port.onmessage = null;
        state.node.onprocessorerror = null;
        state.node.port.close();
        state.node.disconnect();
        state.node = null;
      }
      const complete = () => { state.context = null; state.closed = true; };
      const failed = error => { state.closeError = message(error); complete(); };
      if (!context || context.state === 'closed') { complete(); return true; }
      try { context.close().then(complete, failed); }
      catch (error) { failed(error); }
      return state.closed;
    }
  };
})(globalThis);

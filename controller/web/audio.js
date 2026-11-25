const sampleRate = 48000;
const packetSamples = 960;
const startDelay = .075;
const maxQueue = .2;
const AudioContext = window.AudioContext || window.webkitAudioContext;

export async function grantMicrophoneAccess() {
  const stream = await navigator.mediaDevices.getUserMedia({audio: true});
  for (const track of stream.getTracks()) {
    track.stop();
  }
}

export class BrowserAudio {
  constructor() {
    this.sources = new Set();
    this.volume = 0;
    this.nextStart = 0;
  }

  async start() {
    if (!AudioContext) {
      throw new Error('Dieser Browser unterstützt keine Audioausgabe.');
    }
    if (!this.context) {
      this.context = new AudioContext({latencyHint: 'interactive'});
      this.gain = this.context.createGain();
      this.gain.gain.value = this.volume;
      this.gain.connect(this.context.destination);
      this.context.onstatechange = () => {
        if (this.context.state !== 'running') {
          this.reset();
        }
      };
    }
    // Call resume synchronously from the click before awaiting any work.
    let timeout;
    try {
      await Promise.race([
        this.context.resume(),
        new Promise((_, reject) => {
          timeout = setTimeout(() => reject(new Error('Der Browser hat den Ton blockiert. Ton freigeben und erneut einschalten.')), 5000);
        }),
      ]);
    } finally {
      clearTimeout(timeout);
    }
  }

  get canSelectOutput() {
    return typeof AudioContext?.prototype.setSinkId === 'function';
  }

  async requestOutputAccess() {
    const media = navigator.mediaDevices;
    if (media.selectAudioOutput) {
      const device = await media.selectAudioOutput();
      return device.deviceId === 'default' ? '' : device.deviceId;
    }
    // Chromium exposes the other outputs after microphone permission.
    await grantMicrophoneAccess();
  }

  async setOutput(deviceId) {
    if (!this.context || (this.context.sinkId ?? '') === deviceId) {
      return;
    }
    if (!this.canSelectOutput) {
      throw new Error('Dieser Browser unterstützt nur die Systemausgabe.');
    }
    await this.context.setSinkId(deviceId);
  }

  setVolume(percent) {
    this.volume = percent / 100;
    if (this.gain) {
      this.gain.gain.setTargetAtTime(this.volume, this.context.currentTime, .01);
    }
  }

  stop() {
    this.reset();
    this.context?.suspend().catch(() => {});
  }

  reset() {
    for (const source of this.sources) {
      source.stop();
    }
    this.sources.clear();
    this.nextStart = 0;
  }

  receive(view, offset, count) {
    if (this.context?.state !== 'running' || count !== packetSamples ||
        view.getUint32(8, true) !== sampleRate) {
      return;
    }
    const now = this.context.currentTime;
    if (!this.nextStart || this.nextStart < now) {
      this.nextStart = now + startDelay;
    } else if (this.nextStart > now + maxQueue) {
      return;
    }
    const buffer = this.context.createBuffer(1, count, sampleRate);
    const samples = buffer.getChannelData(0);
    for (let i = 0; i < count; ++i) {
      samples[i] = view.getInt16(offset + 2 * i, true) / 32768;
    }
    const source = this.context.createBufferSource();
    source.buffer = buffer;
    source.connect(this.gain);
    source.onended = () => this.sources.delete(source);
    this.sources.add(source);
    source.start(this.nextStart);
    this.nextStart += buffer.duration;
  }
}

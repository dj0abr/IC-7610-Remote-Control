const sampleRate = 16000;
const AudioContext = window.AudioContext || window.webkitAudioContext;

export class Microphone {
  constructor(onFrame, onLost) {
    this.onFrame = onFrame;
    this.onLost = onLost;
  }

  get ready() {
    return this.context?.state === 'running' && this.track?.readyState === 'live';
  }

  async start(deviceId) {
    if (!AudioContext || !navigator.mediaDevices?.getUserMedia) {
      throw new Error('Mikrofonzugriff benötigt einen unterstützten Browser auf localhost oder HTTPS.');
    }
    const context = new AudioContext({sampleRate, latencyHint: 'interactive'});
    this.context = context;
    const resumed = context.resume();
    resumed.catch(() => {});
    let stream;
    try {
      if (!context.audioWorklet || context.sampleRate !== sampleRate) {
        throw new Error('Der Browser unterstützt den 16-kHz-Mikrofonpfad nicht.');
      }
      const audio = {channelCount: 1, echoCancellation: false, noiseSuppression: false, autoGainControl: false};
      if (deviceId) {
        audio.deviceId = {exact: deviceId};
      }
      stream = await navigator.mediaDevices.getUserMedia({audio});
      await Promise.all([resumed, context.audioWorklet.addModule('/tx-worklet.js')]);
      if (this.context !== context) {
        throw new Error('Mikrofonstart abgebrochen.');
      }
      this.track = stream.getAudioTracks()[0];
      if (!this.ready) {
        throw new Error('Das Mikrofon konnte nicht gestartet werden.');
      }
      this.source = context.createMediaStreamSource(stream);
      this.node = new AudioWorkletNode(context, 'tx-microphone', {channelCount: 1, outputChannelCount: [1]});
      this.source.connect(this.node);
      this.node.connect(context.destination);
      this.node.port.onmessage = ({data}) => this.onFrame(data.id, data.packet);
      this.node.onprocessorerror = this.track.onended = () => this.onLost();
      context.onstatechange = () => {
        if (!this.ready) {
          this.onLost();
        }
      };
    } catch (error) {
      stream?.getTracks().forEach(track => track.stop());
      if (this.context === context) {
        this.stop();
      }
      throw error;
    }
  }

  capture(id) {
    this.node?.port.postMessage(id);
  }

  stop() {
    const context = this.context;
    this.context = null;
    if (context) {
      context.onstatechange = null;
      context.close().catch(() => {});
    }
    if (this.track) {
      this.track.onended = null;
      this.track.stop();
    }
    this.source?.disconnect();
    this.node?.disconnect();
    this.track = this.source = this.node = null;
  }
}

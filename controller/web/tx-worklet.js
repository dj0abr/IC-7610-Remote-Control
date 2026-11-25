const samplesPerFrame = 320;
const headerBytes = 8;

class TxMicrophone extends AudioWorkletProcessor {
  constructor() {
    super();
    this.id = 0;
    this.samples = 0;
    this.packet = new DataView(new ArrayBuffer(headerBytes + samplesPerFrame * 2));
    this.port.onmessage = ({data}) => {
      this.id = data;
      this.samples = 0;
    };
  }

  process(inputs) {
    const channel = inputs[0]?.[0];
    if (!this.id || !channel) {
      return true;
    }
    for (const sample of channel) {
      const pcm = Math.round(Math.max(-1, Math.min(32767 / 32768, sample)) * 32768);
      this.packet.setInt16(headerBytes + this.samples++ * 2, pcm, true);
      if (this.samples === samplesPerFrame) {
        this.packet.setUint32(0, 0x49435458); // ICTX
        this.packet.setUint32(4, this.id, true);
        const packet = this.packet.buffer;
        this.port.postMessage({id: this.id, packet}, [packet]);
        this.packet = new DataView(new ArrayBuffer(headerBytes + samplesPerFrame * 2));
        this.samples = 0;
      }
    }
    // Only the message port carries microphone audio. The output stays silent.
    return true;
  }
}

registerProcessor('tx-microphone', TxMicrophone);

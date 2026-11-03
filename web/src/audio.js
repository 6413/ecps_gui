export const FRAME_AUDIO = 0x80

const SAMPLE_RATE = 48000
const CHANNELS = 2
const BITRATE = 128000
const FRAME_US = 20000
const AUDIO_HEADER = 2
const TARGET_LEAD_S = 0.08
const MIN_LEAD_S = 0.02
const MAX_LEAD_S = 0.3
const MAX_ENCODE_QUEUE = 10
const OPUS = { codec: 'opus', sampleRate: SAMPLE_RATE, numberOfChannels: CHANNELS }

export const canPlayAudio = () => typeof AudioDecoder != 'undefined' && typeof AudioContext != 'undefined'
export const canShareAudio = () => typeof AudioEncoder != 'undefined' && typeof MediaStreamTrackProcessor != 'undefined'

export class AudioPlayer {
  constructor(volume, muted) {
    this.ctx = new AudioContext({ sampleRate: SAMPLE_RATE, latencyHint: 'interactive' })
    this.gain = this.ctx.createGain()
    this.gain.connect(this.ctx.destination)
    this.setVolume(volume, muted)
    this.openDecoder()
  }

  openDecoder() {
    this.decoder = new AudioDecoder({
      output: data => this.play(data),
      error: () => this.openDecoder()
    })
    this.decoder.configure(OPUS)
  }

  push(packet) {
    const bytes = new Uint8Array(packet)
    if (this.muted || bytes.length <= 1 + AUDIO_HEADER || this.decoder.state != 'configured') {
      return
    }
    this.timestamp += FRAME_US
    this.decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: this.timestamp, data: bytes.subarray(1 + AUDIO_HEADER) }))
  }

  play(data) {
    const buffer = this.ctx.createBuffer(CHANNELS, data.numberOfFrames, data.sampleRate)
    for (let c = 0; c < CHANNELS; ++c) {
      data.copyTo(buffer.getChannelData(c), { planeIndex: Math.min(c, data.numberOfChannels - 1), format: 'f32-planar' })
    }
    data.close()
    const now = this.ctx.currentTime
    if (this.playhead < now + MIN_LEAD_S || this.playhead > now + MAX_LEAD_S) {
      this.playhead = now + TARGET_LEAD_S
    }
    const source = this.ctx.createBufferSource()
    source.buffer = buffer
    source.connect(this.gain)
    source.start(this.playhead)
    this.playhead += buffer.duration
  }

  setVolume(volume, muted) {
    this.muted = muted
    this.gain.gain.value = muted ? 0 : volume
    if (!muted) {
      this.ctx.resume().catch(() => {})
    }
  }

  close() {
    if (this.decoder.state != 'closed') {
      this.decoder.close()
    }
    this.ctx.close().catch(() => {})
  }

  timestamp = 0
  playhead = 0
  muted = false
}

export class AudioSharer {
  constructor(track, send, onError) {
    this.track = track
    this.send = send
    this.onError = onError
  }

  async start() {
    const config = { ...OPUS, bitrate: BITRATE, opus: { frameDuration: FRAME_US } }
    if (!(await AudioEncoder.isConfigSupported(config).catch(() => null))?.supported) {
      this.onError("this browser can't encode opus audio")
      return false
    }
    this.ctx = new AudioContext({ sampleRate: SAMPLE_RATE })
    const destination = this.ctx.createMediaStreamDestination()
    destination.channelCount = CHANNELS
    this.ctx.createMediaStreamSource(new MediaStream([this.track])).connect(destination)
    this.encoder = new AudioEncoder({
      output: chunk => {
        const packet = new Uint8Array(chunk.byteLength + 1)
        packet[0] = FRAME_AUDIO
        chunk.copyTo(packet.subarray(1))
        this.send(packet)
      },
      error: e => this.onError(e.message)
    })
    this.encoder.configure(config)
    this.reader = new MediaStreamTrackProcessor({ track: destination.stream.getAudioTracks()[0] }).readable.getReader()
    this.pump()
    return true
  }

  async pump() {
    while (!this.stopped) {
      const { value, done } = await this.reader.read()
      if (done) {
        break
      }
      if (this.encoder.state == 'configured' && this.encoder.encodeQueueSize < MAX_ENCODE_QUEUE) {
        this.encoder.encode(value)
      }
      value.close()
    }
  }

  stop() {
    this.stopped = true
    this.reader?.cancel().catch(() => {})
    if (this.encoder && this.encoder.state != 'closed') {
      this.encoder.close()
    }
    this.ctx?.close().catch(() => {})
  }

  stopped = false
}
import { avcCodec, FRAME_KEY, parsePacket, supportedConfig } from './h264.js'

const MAX_DECODE_QUEUE = 8
const MAX_ENCODE_QUEUE = 2
const SHARE_MAX_WIDTH = 1920
const SHARE_FPS = 60
const SHARE_BITRATE = 10000000
const SHARE_KEYFRAME_INTERVAL_MS = 5000
const SHARE_CODECS = ['avc1.640033', 'avc1.4d0033', 'avc1.42e033', 'avc1.42e01f']

export const canView = () => typeof VideoDecoder != 'undefined'
export const canShare = () => typeof VideoEncoder != 'undefined' && !!navigator.mediaDevices?.getDisplayMedia

function findSps(data) {
  for (let i = 0; i + 7 < data.length; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 7) {
      return data.subarray(i + 3)
    }
  }
  return null
}

export class Viewer {
  constructor(canvas, requestKeyframe, onError) {
    this.canvas = canvas
    this.ctx = canvas.getContext('2d', { alpha: false, desynchronized: true })
    this.requestKeyframe = requestKeyframe
    this.onError = onError
  }

  // in order, frames arriving while a keyframe sets up the decoder must wait for it
  push(packet) {
    this.work = this.work.then(() => this.decode(packet)).catch(() => {})
  }

  async decode(packet) {
    const { keyframe, data } = parsePacket(packet)
    if (keyframe) {
      this.waitingForKeyframe = false
      await this.configure(data)
    }
    if (this.waitingForKeyframe || !this.decoder || this.decoder.state != 'configured') {
      return
    }
    if (this.decoder.decodeQueueSize > MAX_DECODE_QUEUE) {
      this.waitingForKeyframe = true
      this.requestKeyframe()
      return
    }
    this.decoder.decode(new EncodedVideoChunk({
      type: keyframe ? 'key' : 'delta',
      timestamp: this.timestamp++,
      data
    }))
  }

  async configure(data) {
    const sps = findSps(data)
    if (!sps) {
      return
    }
    const codec = avcCodec(sps)
    if (codec == this.codec && this.decoder?.state == 'configured') {
      return
    }
    const config = await supportedConfig(VideoDecoder, { codec, optimizeForLatency: true })
    if (!config) {
      this.onError(`this browser can't decode ${codec}`)
      return
    }
    this.close()
    this.decoder = new VideoDecoder({
      output: frame => this.draw(frame),
      error: () => {
        this.codec = null
        this.waitingForKeyframe = true
        this.requestKeyframe()
      }
    })
    this.decoder.configure(config)
    this.codec = codec
  }

  // draws once per screen refresh, a frame that is replaced before then costs nothing
  draw(frame) {
    this.pending?.close()
    this.pending = frame
    if (!this.drawScheduled) {
      this.drawScheduled = true
      requestAnimationFrame(() => this.drawPending())
    }
  }

  drawPending() {
    this.drawScheduled = false
    const frame = this.pending
    this.pending = null
    if (!frame) {
      return
    }
    if (this.canvas.width != frame.displayWidth || this.canvas.height != frame.displayHeight) {
      this.canvas.width = frame.displayWidth
      this.canvas.height = frame.displayHeight
    }
    this.ctx.drawImage(frame, 0, 0)
    frame.close()
    this.frames++
  }

  close() {
    this.pending?.close()
    this.pending = null
    if (this.decoder && this.decoder.state != 'closed') {
      this.decoder.close()
    }
    this.decoder = null
  }

  work = Promise.resolve()
  decoder = null
  pending = null
  drawScheduled = false
  codec = null
  timestamp = 0
  frames = 0
  waitingForKeyframe = true
}

async function pickEncoderConfig(width, height) {
  for (const codec of SHARE_CODECS) {
    const config = await supportedConfig(VideoEncoder, {
      codec,
      width,
      height,
      bitrate: SHARE_BITRATE,
      framerate: SHARE_FPS,
      latencyMode: 'realtime',
      avc: { format: 'annexb' }
    })
    if (config) {
      return config
    }
  }
  return null
}

export class Sharer {
  constructor(stream, sendFrame, onEnded, onError) {
    this.stream = stream
    this.sendFrame = sendFrame
    this.onEnded = onEnded
    this.onError = onError
  }

  async start() {
    const track = this.stream.getVideoTracks()[0]
    track.addEventListener('ended', () => this.onEnded())

    const settings = track.getSettings()
    const scale = Math.min(1, SHARE_MAX_WIDTH / settings.width)
    const width = Math.round(settings.width * scale) & ~1
    const height = Math.round(settings.height * scale) & ~1

    const config = await pickEncoderConfig(width, height)
    if (!config) {
      this.onError("this browser can't encode H.264")
      return false
    }
    this.encoder = new VideoEncoder({
      output: chunk => this.onChunk(chunk),
      error: e => this.onError(e.message)
    })
    this.encoder.configure(config)

    this.canvas = new OffscreenCanvas(width, height)
    this.ctx = this.canvas.getContext('2d', { alpha: false })
    this.width = width
    this.height = height

    if (typeof MediaStreamTrackProcessor != 'undefined') {
      this.readFrames(track)
    }
    else {
      this.pollVideo()
    }
    return true
  }

  // not throttled when the page is in the background, unlike the video element path
  async readFrames(track) {
    const reader = new MediaStreamTrackProcessor({ track }).readable.getReader()
    while (!this.stopped) {
      const { value: frame, done } = await reader.read()
      if (done) {
        break
      }
      this.encode(frame)
      frame.close()
    }
    reader.cancel().catch(() => {})
  }

  pollVideo() {
    const video = document.createElement('video')
    video.muted = true
    video.playsInline = true
    video.srcObject = this.stream
    video.play().catch(() => {})
    this.timer = setInterval(() => {
      if (video.readyState >= 2) {
        this.encode(video)
      }
    }, 1000 / SHARE_FPS)
  }

  encode(source) {
    const now = performance.now()
    if (this.stopped || now - this.lastFrameAt < 1000 / SHARE_FPS - 2 || this.encoder.encodeQueueSize > MAX_ENCODE_QUEUE) {
      return
    }
    this.lastFrameAt = now

    // a frame that already has the right size goes straight to the encoder
    const direct = typeof VideoFrame != 'undefined' && source instanceof VideoFrame &&
      source.displayWidth == this.width && source.displayHeight == this.height
    if (!direct) {
      this.ctx.drawImage(source, 0, 0, this.width, this.height)
    }
    const frame = direct ? source : new VideoFrame(this.canvas, { timestamp: Math.round(now * 1000) })
    const keyFrame = this.keyframeWanted || now - this.lastKeyframeAt > SHARE_KEYFRAME_INTERVAL_MS
    if (keyFrame) {
      this.keyframeWanted = false
      this.lastKeyframeAt = now
    }
    this.encoder.encode(frame, { keyFrame })
    if (!direct) {
      frame.close()
    }
  }

  onChunk(chunk) {
    const packet = new Uint8Array(chunk.byteLength + 1)
    packet[0] = chunk.type == 'key' ? FRAME_KEY : 0
    chunk.copyTo(packet.subarray(1))
    this.sendFrame(packet)
  }

  requestKeyframe() {
    this.keyframeWanted = true
  }

  stop() {
    this.stopped = true
    clearInterval(this.timer)
    this.stream.getTracks().forEach(track => track.stop())
    if (this.encoder && this.encoder.state != 'closed') {
      this.encoder.close()
    }
  }

  stopped = false
  keyframeWanted = true
  lastFrameAt = 0
  lastKeyframeAt = 0
}

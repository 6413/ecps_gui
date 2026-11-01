// Playback for browsers without WebCodecs (Firefox on Android): the h264 stream
// is rewrapped as fragmented mp4 and fed to a <video> through Media Source Extensions.
import { avcCodec, parsePacket } from './h264.js'

const TIMESCALE = 90000
const DEFAULT_DURATION_MS = 33
const MAX_DURATION_MS = 500
const TARGET_LATENCY_S = 0.15
const CATCH_UP_RATE = 1.25
const MAX_LATENCY_S = 1
const AHEAD_TOLERANCE_S = 0.05
const LIVE_EDGE_S = 0.05
const KEEP_BEHIND_S = 2
const TRIM_AFTER_S = 4
const MAX_QUEUE = 16
const NAL_SPS = 7
const NAL_PPS = 8
const NAL_AUD = 9
const NAL_IDR = 5

export const canUseMse = () =>
  typeof MediaSource != 'undefined' && MediaSource.isTypeSupported('video/mp4; codecs="avc1.42e01f"')

function u32(v) {
  return [v >>> 24 & 0xff, v >>> 16 & 0xff, v >>> 8 & 0xff, v & 0xff]
}

function u16(v) {
  return [v >>> 8 & 0xff, v & 0xff]
}

function ascii(text) {
  return [...text].map(c => c.charCodeAt(0))
}

function concat(parts) {
  const size = parts.reduce((total, part) => total + part.length, 0)
  const out = new Uint8Array(size)
  let at = 0
  for (const part of parts) {
    out.set(part, at)
    at += part.length
  }
  return out
}

function box(type, ...parts) {
  const bytes = parts.map(part => part instanceof Uint8Array ? part : Uint8Array.from(part))
  const size = bytes.reduce((total, part) => total + part.length, 8)
  return concat([Uint8Array.from([...u32(size), ...ascii(type)]), ...bytes])
}

export function splitNals(data) {
  const nals = []
  let start = -1
  for (let i = 0; i + 3 <= data.length; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      if (start >= 0) {
        let end = i
        while (end > start && data[end - 1] == 0) {
          --end
        }
        nals.push(data.subarray(start, end))
      }
      start = i + 3
      i += 2
    }
  }
  if (start >= 0) {
    nals.push(data.subarray(start))
  }
  return nals
}

export function spsSize(sps) {
  const rbsp = []
  for (let i = 1; i < sps.length; ++i) {
    if (i >= 3 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) {
      continue
    }
    rbsp.push(sps[i])
  }
  let bit = 24
  const readBit = () => {
    const v = rbsp[bit >> 3] >> (7 - (bit & 7)) & 1
    ++bit
    return v
  }
  const readBits = n => {
    let v = 0
    for (let i = 0; i < n; ++i) {
      v = v * 2 + readBit()
    }
    return v
  }
  const ue = () => {
    let zeros = 0
    while (bit < rbsp.length * 8 && !readBit()) {
      ++zeros
    }
    return 2 ** zeros - 1 + readBits(zeros)
  }
  const se = () => {
    const v = ue()
    return v & 1 ? (v + 1) / 2 : -v / 2
  }

  const profile = rbsp[0]
  ue() // seq_parameter_set_id
  let chroma = 1
  if ([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].includes(profile)) {
    chroma = ue()
    if (chroma == 3) {
      readBit()
    }
    ue()
    ue()
    readBit()
    if (readBit()) {
      for (let i = 0; i < (chroma == 3 ? 12 : 8); ++i) {
        if (readBit()) {
          let last = 8
          let next = 8
          for (let j = 0; j < (i < 6 ? 16 : 64); ++j) {
            if (next != 0) {
              next = (last + se() + 256) % 256
            }
            last = next == 0 ? last : next
          }
        }
      }
    }
  }
  ue() // log2_max_frame_num
  const pocType = ue()
  if (pocType == 0) {
    ue()
  }
  else if (pocType == 1) {
    readBit()
    se()
    se()
    const cycle = ue()
    for (let i = 0; i < cycle; ++i) {
      se()
    }
  }
  ue() // max_num_ref_frames
  readBit()
  const widthMbs = ue() + 1
  const heightUnits = ue() + 1
  const frameMbsOnly = readBit()
  if (!frameMbsOnly) {
    readBit()
  }
  readBit()
  let cropX = 0
  let cropY = 0
  if (readBit()) {
    const left = ue()
    const right = ue()
    const top = ue()
    const bottom = ue()
    const subX = chroma == 1 || chroma == 2 ? 2 : 1
    const subY = chroma == 1 ? 2 : 1
    cropX = (left + right) * subX
    cropY = (top + bottom) * subY * (2 - frameMbsOnly)
  }
  return {
    width: widthMbs * 16 - cropX,
    height: heightUnits * 16 * (2 - frameMbsOnly) - cropY
  }
}

export function initSegment(sps, pps) {
  const { width, height } = spsSize(sps)
  const zeros = n => new Array(n).fill(0)
  const matrix = [...u32(0x10000), ...zeros(12), ...u32(0x10000), ...zeros(12), ...u32(0x40000000)]

  const avcC = box('avcC',
    [1, sps[1], sps[2], sps[3], 0xff, 0xe1, ...u16(sps.length)], sps,
    [1, ...u16(pps.length)], pps)
  const avc1 = box('avc1',
    zeros(6), u16(1), zeros(16), u16(width), u16(height),
    u32(0x480000), u32(0x480000), zeros(4), u16(1), zeros(32), u16(24), u16(0xffff), avcC)

  const stbl = box('stbl',
    box('stsd', zeros(4), u32(1), avc1),
    box('stts', zeros(8)),
    box('stsc', zeros(8)),
    box('stsz', zeros(12)),
    box('stco', zeros(8)))
  const dinf = box('dinf', box('dref', zeros(4), u32(1), box('url ', [0, 0, 0, 1])))
  const minf = box('minf', box('vmhd', [0, 0, 0, 1], zeros(8)), dinf, stbl)
  const mdia = box('mdia',
    box('mdhd', zeros(12), u32(TIMESCALE), zeros(4), u16(0x55c4), zeros(2)),
    box('hdlr', zeros(8), ascii('vide'), zeros(12), ascii('ecps\0')),
    minf)
  const trak = box('trak',
    box('tkhd', [0, 0, 0, 3], zeros(8), u32(1), zeros(4), zeros(4), zeros(8), zeros(8), matrix, u32(width * 65536), u32(height * 65536)),
    mdia)
  const mvex = box('mvex', box('trex', zeros(4), u32(1), u32(1), zeros(12)))
  const moov = box('moov',
    box('mvhd', zeros(12), u32(TIMESCALE), zeros(4), u32(0x10000), u16(0x100), zeros(10), matrix, zeros(24), u32(2)),
    trak,
    mvex)

  return concat([box('ftyp', ascii('isom'), zeros(4), ascii('isom'), ascii('avc1')), moov])
}

export function mediaSegment(nals, sequence, decodeTime, duration, keyframe) {
  const sampleSize = nals.reduce((total, nal) => total + 4 + nal.length, 0)
  const mdatParts = nals.flatMap(nal => [Uint8Array.from(u32(nal.length)), nal])

  const highTime = Math.floor(decodeTime / 2 ** 32)
  const trun = dataOffset => box('trun', [0, 0, 7, 1], u32(1), u32(dataOffset),
    u32(duration), u32(sampleSize), u32(keyframe ? 0x02000000 : 0x01010000))
  const moof = dataOffset => box('moof',
    box('mfhd', [0, 0, 0, 0], u32(sequence)),
    box('traf',
      box('tfhd', [0, 2, 0, 0], u32(1)),
      box('tfdt', [1, 0, 0, 0], u32(highTime), u32(decodeTime >>> 0)),
      trun(dataOffset)))

  const moofSize = moof(0).length
  return concat([moof(moofSize + 8), box('mdat', ...mdatParts)])
}

export class MseViewer {
  constructor(video, requestKeyframe, onError) {
    this.video = video
    this.requestKeyframe = requestKeyframe
    this.onError = onError
    this.onVideoError = () => this.recover()
    video.addEventListener('error', this.onVideoError)
  }

  push(packet) {
    const { keyframe, data } = parsePacket(packet)
    const nals = splitNals(data)

    if (keyframe) {
      const sps = nals.find(nal => (nal[0] & 0x1f) == NAL_SPS)
      const pps = nals.find(nal => (nal[0] & 0x1f) == NAL_PPS)
      if (sps && pps && !this.matchesStream(sps)) {
        this.openStream(sps, pps)
      }
      if (this.source) {
        this.waitingForKeyframe = false
      }
    }
    if (this.waitingForKeyframe || !this.source) {
      return
    }
    const samples = nals.filter(nal => ![NAL_SPS, NAL_PPS, NAL_AUD].includes(nal[0] & 0x1f))
    if (samples.length == 0) {
      return
    }
    if (this.queue.length >= MAX_QUEUE) {
      this.queue.length = this.initPending ? 1 : 0
      this.waitingForKeyframe = true
      this.requestKeyframe()
      return
    }
    // a frame lasts as long as it took to arrive, which keeps the timeline at wall clock speed
    const now = performance.now()
    const gap = this.lastFrameAt ? now - this.lastFrameAt : DEFAULT_DURATION_MS
    this.lastFrameAt = now
    const duration = Math.round(Math.min(Math.max(gap, 1), MAX_DURATION_MS) * TIMESCALE / 1000)

    const isSync = samples.some(nal => (nal[0] & 0x1f) == NAL_IDR)
    if (isSync) {
      this.keyframeTimes.push(this.decodeTime / TIMESCALE)
    }
    this.queue.push(mediaSegment(samples, ++this.sequence, this.decodeTime, duration, isSync))
    this.decodeTime += duration
    this.pump()
  }

  matchesStream(sps) {
    return this.sps && sps.length == this.sps.length && sps.every((v, i) => v == this.sps[i])
  }

  openStream(sps, pps) {
    const codec = avcCodec(sps)
    const mime = `video/mp4; codecs="${codec}"`
    if (!MediaSource.isTypeSupported(mime)) {
      this.onError(`this browser can't play ${codec}`)
      return
    }
    this.closeStream()
    this.sps = sps.slice()
    this.queue = [initSegment(sps, pps)]
    this.initPending = true

    const source = new MediaSource()
    this.source = source
    this.objectUrl = URL.createObjectURL(source)
    this.video.src = this.objectUrl
    source.addEventListener('sourceopen', () => {
      if (source != this.source) {
        return
      }
      this.buffer = source.addSourceBuffer(mime)
      this.buffer.mode = 'segments'
      this.buffer.addEventListener('updateend', () => this.onUpdateEnd())
      this.pump()
    }, { once: true })
    this.video.play().catch(() => {})
  }

  pump() {
    if (!this.buffer || this.buffer.updating || this.queue.length == 0 || this.source.readyState != 'open') {
      return
    }
    const segment = this.queue[0]
    try {
      this.buffer.appendBuffer(segment)
      this.queue.shift()
      this.initPending = false
    }
    catch {
      this.recover()
    }
  }

  onUpdateEnd() {
    const { video, buffer } = this
    if (!buffer || this.source?.readyState != 'open') {
      return
    }
    const ranges = buffer.buffered
    if (ranges.length) {
      const start = ranges.start(ranges.length - 1)
      const end = ranges.end(ranges.length - 1)
      const time = video.currentTime
      // a seek decodes from the previous keyframe, so small delays are played away instead
      if (end - time > MAX_LATENCY_S || time < start || time > end + AHEAD_TOLERANCE_S) {
        video.currentTime = Math.max(end - LIVE_EDGE_S, start)
      }
      else {
        video.playbackRate = end - time > TARGET_LATENCY_S ? CATCH_UP_RATE : 1
      }
      if (time - ranges.start(0) > TRIM_AFTER_S) {
        // removal runs on to the next keyframe, so only a keyframe is a safe place to stop
        const keep = this.keyframeTimes.filter(t => t <= time - KEEP_BEHIND_S).pop()
        if (keep > ranges.start(0)) {
          this.keyframeTimes = this.keyframeTimes.filter(t => t >= keep)
          buffer.remove(0, keep)
          return
        }
      }
    }
    if (video.paused) {
      video.play().catch(() => {})
    }
    this.pump()
  }

  recover() {
    this.closeStream()
    this.waitingForKeyframe = true
    this.requestKeyframe()
  }

  closeStream() {
    this.source = null
    this.buffer = null
    this.sps = null
    this.queue = []
    this.initPending = false
    this.sequence = 0
    this.decodeTime = 0
    this.lastFrameAt = 0
    this.keyframeTimes = []
    if (this.objectUrl) {
      URL.revokeObjectURL(this.objectUrl)
      this.objectUrl = null
    }
  }

  close() {
    this.video.removeEventListener('error', this.onVideoError)
    this.closeStream()
    this.video.removeAttribute('src')
    this.video.load()
  }

  source = null
  buffer = null
  sps = null
  objectUrl = null
  queue = []
  initPending = false
  sequence = 0
  decodeTime = 0
  lastFrameAt = 0
  keyframeTimes = []
  waitingForKeyframe = true
}

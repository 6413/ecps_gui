// For browsers that can't encode H.264 themselves (Firefox, Firefox on Android: no WebCodecs encoder): the page
// records with MediaRecorder (WebM, VP8 and Opus) and sends the recording here, ffmpeg turns it into what the ecps
// clients take. Video becomes H.264 frames, audio 20 ms Opus packets at 48 kHz stereo.
// ffmpeg must be on the PATH, or named by FFMPEG.
import { spawn } from 'node:child_process'

const FFMPEG = process.env.FFMPEG || 'ffmpeg'

// the recorder's WebM is read as it arrives: a small probe, no analysis (-fflags nobuffer breaks VP8 input)
// info, not error: ffmpeg says once what it reads (codec, size), the gateway logs it
const INPUT = ['-hide_banner', '-nostats', '-loglevel', 'info', '-flags', 'low_delay',
  '-probesize', '32768', '-analyzeduration', '0', '-f', 'matroska', '-i', 'pipe:0']

export function canTranscode() {
  return new Promise(resolve => {
    try {
      const probe = spawn(FFMPEG, ['-hide_banner', '-version'], { stdio: 'ignore' })
      probe.on('error', () => resolve(false))
      probe.on('exit', code => resolve(code == 0))
    }
    catch {
      resolve(false)
    }
  })
}

class Transcoder {
  constructor(args, onOutput, onError) {
    this.process = spawn(FFMPEG, [...INPUT, ...args], { stdio: ['pipe', 'pipe', 'pipe'] })
    this.process.stdout.on('data', data => {
      this.bytesOut += data.length
      onOutput(data)
    })
    let errors = ''
    this.process.stderr.on('data', d => {
      errors = (errors + d).slice(-2000)
      // "Stream #0:0: Video: vp8, yuv420p(...), 1280x720, ..." of the input, the first time it shows
      if (!this.input) {
        const video = /Stream #0:\d.*?Video: (\w+)[^\n]*?(\d{2,5})x(\d{2,5})/.exec(errors)
        if (video) {
          this.input = `${video[1]} ${video[2]}x${video[3]}`
        }
      }
    })
    this.process.on('error', e => onError(`ffmpeg could not start: ${e.message}`))
    this.process.on('exit', code => {
      if (!this.closed && code != 0) {
        onError(`ffmpeg stopped: ${errors.trim() || code}`)
      }
    })
    this.process.stdin.on('error', () => {})
  }

  write(data) {
    if (!this.closed && this.process.stdin.writable) {
      this.bytesIn += data.length
      this.process.stdin.write(data)
    }
  }

  bytesIn = 0
  bytesOut = 0
  input = ''

  close() {
    this.closed = true
    this.process.stdin.end()
    setTimeout(() => this.process.kill('SIGKILL'), 1000).unref()
  }
}

// H.264 Annex B in, one frame (access unit) out at a time. x264 puts an access unit delimiter in front of every
// frame (aud=1), a frame is complete when the next one's delimiter comes
class AccessUnits {
  constructor(onFrame) {
    this.onFrame = onFrame
    this.buffer = Buffer.alloc(0)
  }

  push(data) {
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, data]) : data
    let start = -1
    for (let i = 0; i + 4 < this.buffer.length; ++i) {
      if (this.buffer[i] != 0 || this.buffer[i + 1] != 0) {
        continue
      }
      const long = this.buffer[i + 2] == 0 && this.buffer[i + 3] == 1
      if (!long && this.buffer[i + 2] != 1) {
        continue
      }
      const nalType = this.buffer[i + (long ? 4 : 3)] & 0x1f
      if (nalType == 9) {
        if (start >= 0) {
          this.emit(this.buffer.subarray(start, i))
        }
        start = i
      }
      i += long ? 3 : 2
    }
    if (start > 0) {
      this.buffer = Buffer.from(this.buffer.subarray(start))
    }
  }

  emit(frame) {
    let keyframe = false
    for (let i = 0; i + 4 < frame.length; ++i) {
      if (frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1 && (frame[i + 3] & 0x1f) == 5) {
        keyframe = true
        break
      }
    }
    this.onFrame(Buffer.from(frame), keyframe)
  }
}

// Ogg in, the Opus packets out, without the two header packets
class OggPackets {
  constructor(onPacket) {
    this.onPacket = onPacket
    this.buffer = Buffer.alloc(0)
    this.partial = []
    this.headers = 2
  }

  push(data) {
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, data]) : data
    while (this.buffer.length >= 27) {
      if (this.buffer.toString('latin1', 0, 4) != 'OggS') {
        const next = this.buffer.indexOf('OggS', 1, 'latin1')
        this.buffer = next < 0 ? Buffer.alloc(0) : this.buffer.subarray(next)
        continue
      }
      const segments = this.buffer[26]
      if (this.buffer.length < 27 + segments) {
        return
      }
      const lacing = this.buffer.subarray(27, 27 + segments)
      const bodySize = lacing.reduce((sum, n) => sum + n, 0)
      if (this.buffer.length < 27 + segments + bodySize) {
        return
      }
      let at = 27 + segments
      for (const size of lacing) {
        this.partial.push(this.buffer.subarray(at, at + size))
        at += size
        // a lacing value under 255 ends a packet
        if (size < 255) {
          const packet = Buffer.concat(this.partial)
          this.partial = []
          if (this.headers > 0) {
            --this.headers
          }
          else if (packet.length) {
            this.onPacket(packet)
          }
        }
      }
      this.buffer = Buffer.from(this.buffer.subarray(at))
    }
  }
}

// one recorded share: the video recording and, apart from it, the audio one
export class RecordingShare {
  constructor(onFrame, onAudio, onError) {
    const units = new AccessUnits(onFrame)
    this.video = new Transcoder([
      '-an', '-vf', "scale='min(1920,iw)':-2", '-pix_fmt', 'yuv420p',
      '-c:v', 'libx264', '-preset', 'veryfast', '-tune', 'zerolatency', '-g', '60', '-bf', '0',
      // a recording has no fixed frame rate, ffmpeg takes its millisecond timestamps for 1000 fps and x264 gave each
      // frame a thousandth of the bitrate (0.1 Mbit/s out of 8 in). x264 is told 30 fps instead, and encodes for a
      // quality (crf) capped at 8 Mbit/s, so a camera giving fewer frames still gets the quality
      '-crf', '20', '-maxrate', '8M', '-bufsize', '8M', '-x264-params', 'aud=1:repeat-headers=1:fps=30:force-cfr=1',
      '-fps_mode', 'passthrough', '-flush_packets', '1', '-f', 'h264', 'pipe:1'
    ], data => units.push(data), onError)
    this.onAudio = onAudio
    this.onError = onError
    this.frames = 0
    this.skipped = 0
    this.lastReport = { at: Date.now(), in: 0, out: 0, frames: 0 }
  }

  // a line for the gateway's window every 10 s: what the browser records and what goes out of it
  report() {
    const now = Date.now()
    const seconds = (now - this.lastReport.at) / 1000
    if (seconds < 10) {
      return null
    }
    const mbit = bytes => (bytes * 8 / 1e6 / seconds).toFixed(1)
    const line = `browser recording: in ${this.video.input || 'unknown'} ${mbit(this.video.bytesIn - this.lastReport.in)} Mbit/s, ` +
      `out H.264 ${mbit(this.video.bytesOut - this.lastReport.out)} Mbit/s ` +
      `${((this.frames - this.lastReport.frames) / seconds).toFixed(0)} fps, ${this.skipped} frames skipped (send queue full)`
    this.lastReport = { at: now, in: this.video.bytesIn, out: this.video.bytesOut, frames: this.frames }
    this.skipped = 0
    return line
  }

  writeVideo(data) {
    this.video.write(data)
  }

  writeAudio(data) {
    if (!this.audio) {
      const packets = new OggPackets(this.onAudio)
      this.audio = new Transcoder([
        '-vn', '-c:a', 'libopus', '-b:a', '96k', '-ar', '48000', '-ac', '2', '-frame_duration', '20',
        '-application', 'audio', '-page_duration', '20000', '-flush_packets', '1', '-f', 'ogg', 'pipe:1'
      ], data => packets.push(data), this.onError)
    }
    this.audio.write(data)
  }

  close() {
    this.video.close()
    this.audio?.close()
  }
}

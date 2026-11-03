// ECPS protocol client: a web user looks like any other client to the server.
// Layouts mirror prot.h (everything is packed, little endian) and
// fan/network/network_crypto.ixx.
import crypto from 'node:crypto'
import dgram from 'node:dgram'
import fs from 'node:fs'
import net from 'node:net'
import { EventEmitter } from 'node:events'

const C2S = {
  KeepAlive: 0,
  KeyExchange: 1,
  Request_Login: 2,
  CreateChannel: 3,
  JoinChannel: 4,
  RequestChannelList: 6,
  RequestChannelSessionList: 7,
  ViewToShare: 14
}
const S2C = {
  KeepAlive: 0,
  KeyExchange: 1,
  Response_Login: 2,
  CreateChannel_OK: 3,
  CreateChannel_Error: 4,
  JoinChannel_OK: 5,
  JoinChannel_Error: 6,
  KickedFromChannel: 7,
  ChannelList: 8,
  ChannelSessionList: 9,
  ViewToShare: 16
}
// body size of every S2C command, in protocol order
const S2C_SIZE = [0, 80, 8, 7, 1, 7, 1, 3, 2, 4, 3, 10, 10, 10, 12, 5, 4, 4]

const UDP_C2S = { KeepAlive: 0, HostStreamData: 1, HostAudioData: 3 }
const UDP_S2C = { KeepAlive: 0, ViewStreamData: 1, ViewAudioData: 3 }

const TCP_HEADER = 6
const UDP_HEADER = 18
const TAG = 16
const CHUNK = 0x400
const STREAM_BODY = 3
const STREAM_HEAD = 5
const MAX_RECORD = 16384
const CHANNEL_INFO = 76
const SESSION_INFO = 53
const RESET_IDR = 2
const KEYFRAME_FLAG = 1
const CRYPTO_CONTEXT = 'ecps udp v1'
const MAX_OPUS = 1275

export const FRAME_KEY = 1
export const FRAME_AUDIO = 0x80
export const AUDIO_HEADER = 2

function nonceFor(counter) {
  const nonce = Buffer.alloc(12)
  nonce.writeBigUInt64LE(BigInt(counter), 4)
  return nonce
}

export function seal(key, counter, aad, plain) {
  const c = crypto.createCipheriv('chacha20-poly1305', key, nonceFor(counter), { authTagLength: TAG })
  c.setAAD(aad, { plaintextLength: plain.length })
  const body = c.update(plain)
  c.final()
  return Buffer.concat([body, c.getAuthTag()])
}

export function open(key, counter, aad, sealed) {
  if (sealed.length < TAG) {
    return null
  }
  const body = sealed.subarray(0, sealed.length - TAG)
  try {
    const d = crypto.createDecipheriv('chacha20-poly1305', key, nonceFor(counter), { authTagLength: TAG })
    d.setAAD(aad, { plaintextLength: body.length })
    d.setAuthTag(sealed.subarray(sealed.length - TAG))
    const plain = d.update(body)
    d.final()
    return plain
  }
  catch {
    return null
  }
}

function x25519PublicKey(raw) {
  return crypto.createPublicKey({ key: { kty: 'OKP', crv: 'X25519', x: raw.toString('base64url') }, format: 'jwk' })
}

export function deriveKeys(privateKey, clientPublic, serverEphemeral, serverStatic) {
  const dh = Buffer.concat([
    crypto.diffieHellman({ privateKey, publicKey: x25519PublicKey(serverEphemeral) }),
    crypto.diffieHellman({ privateKey, publicKey: x25519PublicKey(serverStatic) })
  ])
  const salt = Buffer.concat([clientPublic, serverEphemeral, serverStatic])
  const keys = Buffer.from(crypto.hkdfSync('sha256', dh, salt, CRYPTO_CONTEXT, 144))
  return {
    c2s: keys.subarray(0, 32),
    s2c: keys.subarray(32, 64),
    confirm: keys.subarray(64, 80),
    tcpC2s: keys.subarray(80, 112),
    tcpS2c: keys.subarray(112, 144)
  }
}

export function parseStreamHeader(b) {
  const sequence = b[0] | (b[1] & 0xf0) << 4
  const current = (b[1] & 0x0f) | b[2] << 4
  if (current != 0) {
    return { sequence, current, size: STREAM_BODY }
  }
  return {
    sequence,
    current,
    possible: b[3] | (b[4] & 0xf0) << 4,
    flag: b[4] & 0x0f,
    size: STREAM_HEAD
  }
}

export function writeStreamHeader(sequence, current, possible, flag) {
  const b = Buffer.alloc(current == 0 ? STREAM_HEAD : STREAM_BODY)
  b[0] = sequence & 0xff
  b[1] = (sequence >> 4 & 0xf0) | (current & 0x0f)
  b[2] = current >> 4 & 0xff
  if (current == 0) {
    b[3] = possible & 0xff
    b[4] = (possible >> 4 & 0xf0) | (flag & 0x0f)
  }
  return b
}

export function writeAudioPacket(sequence, opus) {
  return Buffer.concat([u16(sequence & 0xffff), opus])
}

export function parseAudioPacket(b) {
  if (b.length <= AUDIO_HEADER) {
    return null
  }
  return { sequence: b.readUInt16LE(0), opus: b.subarray(AUDIO_HEADER) }
}

function isKeyframe(data) {
  const end = Math.min(data.length - 4, 64)
  for (let i = 0; i < end; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
      const type = data[i + 4] & 0x1f
      if (type == 5 || type == 7) {
        return true
      }
    }
  }
  return false
}

function u16(value) {
  const b = Buffer.alloc(2)
  b.writeUInt16LE(value)
  return b
}

function cString(b) {
  const end = b.indexOf(0)
  return b.toString('latin1', 0, end < 0 ? b.length : end)
}

function sequenceAfter(a, b) {
  const diff = (a - b) & 0xfff
  return diff != 0 && diff < 0x800
}

class ReplayWindow {
  accept(n) {
    if (n + 1024 <= this.top) {
      return false
    }
    if (this.seen.has(n)) {
      return false
    }
    this.seen.add(n)
    if (n >= this.top) {
      this.top = n + 1
    }
    if (this.seen.size > 2048) {
      for (const old of this.seen) {
        if (old + 1024 <= this.top) {
          this.seen.delete(old)
        }
      }
    }
    return true
  }
  top = 0
  seen = new Set()
}

export class EcpsClient extends EventEmitter {
  constructor(host, port, knownHostsPath) {
    super()
    this.host = host
    this.port = port
    this.knownHostsPath = knownHostsPath
  }

  connect() {
    const { privateKey, publicKey } = crypto.generateKeyPairSync('x25519')
    this.privateKey = privateKey
    this.publicKey = Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url')

    this.tcp = net.createConnection({ host: this.host, port: this.port, noDelay: true })
    this.tcp.on('connect', () => {
      this.writeCommand(C2S.KeyExchange, this.publicKey)
    })
    this.tcp.on('data', data => this.onTcpData(data))
    this.tcp.on('error', e => this.fail(e.message))
    this.tcp.on('close', () => this.fail('connection lost'))

    this.udp = dgram.createSocket('udp4')
    this.udp.on('error', () => {})
    this.udp.on('message', msg => this.onUdp(msg))
    this.udp.connect(this.port, this.host, () => {
      try {
        this.udp.setSendBufferSize(8 << 20)
        this.udp.setRecvBufferSize(8 << 20)
      }
      catch {}
      this.udpReady = true
      this.sendUdpKeepAlive()
    })
  }

  close() {
    this.closed = true
    clearInterval(this.keepAliveTimer)
    this.tcp?.destroy()
    if (this.udp) {
      try {
        this.udp.close()
      }
      catch {}
    }
    this.removeAllListeners()
  }

  fail(reason) {
    if (this.closed) {
      return
    }
    this.emit('closed', reason)
    this.close()
  }

  writeCommand(command, body = Buffer.alloc(0)) {
    if (this.closed) {
      return
    }
    const packet = Buffer.alloc(TCP_HEADER + body.length)
    packet.writeUInt32LE(this.commandId++, 0)
    packet.writeUInt16LE(command, 4)
    body.copy(packet, TCP_HEADER)
    if (!this.tcpKeys) {
      this.tcp.write(packet)
      return
    }
    for (let at = 0; at < packet.length; at += MAX_RECORD) {
      const plain = packet.subarray(at, at + MAX_RECORD)
      const length = Buffer.alloc(2)
      length.writeUInt16LE(plain.length)
      this.tcp.write(Buffer.concat([length, seal(this.tcpKeys.send, this.tcpSendCounter++, length, plain)]))
    }
  }

  onTcpData(data) {
    this.rx = this.rx.length ? Buffer.concat([this.rx, data]) : data
    if (!this.tcpKeys) {
      // the key exchange reply is the only plaintext message
      if (this.rx.length < TCP_HEADER + S2C_SIZE[S2C.KeyExchange]) {
        return
      }
      const command = this.rx.readUInt16LE(4)
      if (command != S2C.KeyExchange) {
        return this.fail('unexpected reply to key exchange')
      }
      const body = this.rx.subarray(TCP_HEADER, TCP_HEADER + S2C_SIZE[S2C.KeyExchange])
      this.rx = this.rx.subarray(TCP_HEADER + S2C_SIZE[S2C.KeyExchange])
      if (!this.onKeyExchange(body)) {
        return
      }
    }
    while (this.rx.length >= 2) {
      const n = this.rx.readUInt16LE(0)
      if (n == 0 || n > MAX_RECORD) {
        return this.fail('invalid record')
      }
      if (this.rx.length < 2 + n + TAG) {
        break
      }
      const plain = open(this.tcpKeys.recv, this.tcpRecvCounter++, this.rx.subarray(0, 2), this.rx.subarray(2, 2 + n + TAG))
      if (!plain) {
        return this.fail('record authentication failed')
      }
      this.rx = this.rx.subarray(2 + n + TAG)
      this.plain = this.plain.length ? Buffer.concat([this.plain, plain]) : plain
    }
    this.parseMessages()
  }

  onKeyExchange(body) {
    const serverEphemeral = Buffer.from(body.subarray(0, 32))
    const serverStatic = Buffer.from(body.subarray(32, 64))
    const keys = deriveKeys(this.privateKey, this.publicKey, serverEphemeral, serverStatic)
    if (!crypto.timingSafeEqual(keys.confirm, body.subarray(64, 80))) {
      this.fail('server key exchange failed')
      return false
    }
    if (!this.verifyServerKey(serverStatic)) {
      return false
    }
    this.tcpKeys = { send: keys.tcpC2s, recv: keys.tcpS2c }
    this.udpKeys = { send: keys.c2s, recv: keys.s2c }
    this.writeCommand(C2S.Request_Login, Buffer.alloc(1))
    return true
  }

  // same trust model as the desktop client's known_hosts
  verifyServerKey(key) {
    const host = `${this.host}:${this.port}`
    const hex = key.toString('hex')
    let lines = []
    try {
      lines = fs.readFileSync(this.knownHostsPath, 'utf8').split('\n')
    }
    catch {}
    for (const line of lines) {
      const [h, k] = line.trim().split(/\s+/)
      if (h == host) {
        if (k != hex) {
          this.fail(`server key for ${host} changed, delete its line in ${this.knownHostsPath}`)
          return false
        }
        return true
      }
    }
    fs.appendFileSync(this.knownHostsPath, `${host} ${hex}\n`)
    console.log('pinned server key for', host, hex)
    return true
  }

  parseMessages() {
    while (this.plain.length >= TCP_HEADER) {
      const command = this.plain.readUInt16LE(4)
      if (command >= S2C_SIZE.length) {
        return this.fail('invalid command')
      }
      const size = S2C_SIZE[command]
      if (this.plain.length < TCP_HEADER + size) {
        return
      }
      const body = this.plain.subarray(TCP_HEADER, TCP_HEADER + size)
      let extra = 0
      if (command == S2C.ChannelList) {
        extra = body.readUInt16LE(0) * CHANNEL_INFO
      }
      else if (command == S2C.ChannelSessionList) {
        extra = body.readUInt16LE(2) * SESSION_INFO
      }
      if (this.plain.length < TCP_HEADER + size + extra) {
        return
      }
      const rest = this.plain.subarray(TCP_HEADER + size, TCP_HEADER + size + extra)
      this.plain = this.plain.subarray(TCP_HEADER + size + extra)
      this.onMessage(command, body, rest)
    }
  }

  onMessage(command, body, rest) {
    switch (command) {
      case S2C.Response_Login: {
        this.sessionId = body.readUInt32LE(4)
        this.loggedIn = true
        this.sendUdpKeepAlive()
        this.keepAliveTimer = setInterval(() => {
          this.writeCommand(C2S.KeepAlive)
          this.sendUdpKeepAlive()
        }, 1000)
        this.requestChannels()
        this.emit('ready')
        break
      }
      case S2C.CreateChannel_OK:
      case S2C.JoinChannel_OK: {
        const joined = command == S2C.JoinChannel_OK
        this.channel = { id: body.readUInt16LE(1), sessionId: body.readUInt32LE(3) }
        this.emit(joined ? 'joined' : 'created', this.channel.id)
        this.requestUsers(this.channel.id)
        if (joined) {
          this.requestKeyframe()
        }
        break
      }
      case S2C.JoinChannel_Error:
      case S2C.CreateChannel_Error: {
        this.emit('channelError')
        break
      }
      case S2C.KickedFromChannel: {
        this.emit('kicked', body.readUInt16LE(0))
        break
      }
      case S2C.ChannelList: {
        const channels = []
        for (let i = 0; i + CHANNEL_INFO <= rest.length; i += CHANNEL_INFO) {
          const c = rest.subarray(i, i + CHANNEL_INFO)
          channels.push({
            id: c.readUInt16LE(0),
            users: c.readUInt32LE(3),
            name: cString(c.subarray(7, 71)),
            hostSessionId: c.readUInt32LE(72)
          })
        }
        this.emit('channels', channels)
        break
      }
      case S2C.ChannelSessionList: {
        const users = []
        for (let i = 0; i + SESSION_INFO <= rest.length; i += SESSION_INFO) {
          const s = rest.subarray(i, i + SESSION_INFO)
          users.push({ sessionId: s.readUInt32LE(0), name: cString(s.subarray(12, 44)), isHost: s[44] != 0 })
        }
        this.emit('users', { channel: body.readUInt16LE(0), users })
        break
      }
      case S2C.ViewToShare: {
        if (body.readUInt16LE(2) & RESET_IDR) {
          this.emit('keyframeWanted')
        }
        break
      }
    }
  }

  requestChannels() {
    this.writeCommand(C2S.RequestChannelList, Buffer.alloc(1))
  }

  requestUsers(channel) {
    this.writeCommand(C2S.RequestChannelSessionList, u16(channel))
  }

  createChannel() {
    this.writeCommand(C2S.CreateChannel, Buffer.alloc(1))
  }

  join(channel) {
    this.writeCommand(C2S.JoinChannel, u16(channel))
  }

  requestKeyframe() {
    const now = Date.now()
    if (!this.channel || now - this.lastKeyframeRequest < 300) {
      return
    }
    this.lastKeyframeRequest = now
    this.writeCommand(C2S.ViewToShare, Buffer.concat([u16(this.channel.id), u16(RESET_IDR)]))
  }

  writeUdp(command, body) {
    if (!this.udpReady || !this.udpKeys || !this.loggedIn || this.closed) {
      return
    }
    const nonce = this.udpSendNonce++
    const header = Buffer.alloc(UDP_HEADER)
    header.writeUInt32LE(this.sessionId, 0)
    header.writeBigUInt64LE(BigInt(nonce), 8)
    header.writeUInt16LE(command, 16)
    this.udp.send(Buffer.concat([header, seal(this.udpKeys.send, nonce, header, body)]))
  }

  sendUdpKeepAlive() {
    this.writeUdp(UDP_C2S.KeepAlive, Buffer.alloc(0))
  }

  onUdp(msg) {
    if (msg.length < UDP_HEADER + TAG || !this.udpKeys || msg.readUInt32LE(0) != this.sessionId) {
      return
    }
    const header = msg.subarray(0, UDP_HEADER)
    const nonce = msg.readBigUInt64LE(8)
    const plain = open(this.udpKeys.recv, nonce, header, msg.subarray(UDP_HEADER))
    if (!plain || !this.udpReplay.accept(Number(nonce))) {
      return
    }
    const command = msg.readUInt16LE(16)
    if (plain.length < 2 || plain.readUInt16LE(0) != this.channel?.id) {
      return
    }
    if (command == UDP_S2C.ViewStreamData && plain.length >= 2 + STREAM_BODY) {
      this.onStreamPacket(plain.subarray(2))
    }
    else if (command == UDP_S2C.ViewAudioData && parseAudioPacket(plain.subarray(2))) {
      this.emit('audio', plain.subarray(2))
    }
  }

  onStreamPacket(packet) {
    const head = parseStreamHeader(packet)
    if (packet.length < head.size) {
      return
    }
    if (this.lastDelivered >= 0 && !sequenceAfter(head.sequence, this.lastDelivered)) {
      return
    }
    let frame = this.frames.get(head.sequence)
    if (!frame) {
      frame = { chunks: new Map(), possible: -1, flag: 0, size: 0 }
      this.frames.set(head.sequence, frame)
    }
    if (frame.chunks.has(head.current)) {
      return
    }
    if (head.current == 0) {
      frame.possible = head.possible
      frame.flag = head.flag
    }
    const data = packet.subarray(head.size)
    frame.chunks.set(head.current, data)
    frame.size += data.length
    if (frame.chunks.size == frame.possible) {
      this.completeFrame(head.sequence, frame)
    }
    else if (this.frames.size > 8) {
      // frames this far behind lost a packet for good
      this.dropOldestFrame()
    }
  }

  dropOldestFrame() {
    let oldest = -1
    for (const sequence of this.frames.keys()) {
      if (oldest < 0 || sequenceAfter(oldest, sequence)) {
        oldest = sequence
      }
    }
    this.frames.delete(oldest)
    this.lastDelivered = oldest
    this.waitingForKeyframe = true
    this.requestKeyframe()
  }

  completeFrame(sequence, frame) {
    for (const other of this.frames.keys()) {
      if (sequenceAfter(sequence, other)) {
        this.frames.delete(other)
        this.waitingForKeyframe = true
      }
    }
    this.frames.delete(sequence)
    this.lastDelivered = sequence

    const data = Buffer.alloc(frame.size)
    let at = 0
    for (let i = 0; i < frame.possible; ++i) {
      const chunk = frame.chunks.get(i)
      chunk.copy(data, at)
      at += chunk.length
    }
    const keyframe = (frame.flag & KEYFRAME_FLAG) != 0 || isKeyframe(data)
    if (this.waitingForKeyframe && !keyframe) {
      this.requestKeyframe()
      return
    }
    this.waitingForKeyframe = false
    this.emit('frame', data, keyframe)
  }

  // host side: a frame from the browser's encoder
  sendFrame(data, keyframe) {
    if (!this.channel) {
      return false
    }
    if (this.sendQueue.length > 4 && !keyframe) {
      return false
    }
    if (keyframe) {
      this.sendQueue.length = 0
    }
    this.sendQueue.push({ data, keyframe })
    if (!this.sending) {
      this.sending = true
      this.pumpFrames()
    }
    return true
  }

  sendAudio(opus) {
    if (!this.channel || !opus.length || opus.length > MAX_OPUS) {
      return
    }
    const target = Buffer.alloc(6)
    target.writeUInt16LE(this.channel.id, 0)
    target.writeUInt32LE(this.channel.sessionId, 2)
    this.writeUdp(UDP_C2S.HostAudioData, Buffer.concat([target, writeAudioPacket(this.audioSequence++, opus)]))
    this.audioSequence &= 0xffff
  }

  async pumpFrames() {
    const target = Buffer.alloc(6)
    target.writeUInt16LE(this.channel.id, 0)
    target.writeUInt32LE(this.channel.sessionId, 2)
    while (this.sendQueue.length && !this.closed) {
      const { data, keyframe } = this.sendQueue.shift()
      const possible = Math.ceil(data.length / CHUNK)
      if (possible > 0xfff) {
        continue
      }
      for (let i = 0; i < possible; ++i) {
        const chunk = data.subarray(i * CHUNK, (i + 1) * CHUNK)
        const header = writeStreamHeader(this.frameIndex, i, possible, keyframe ? KEYFRAME_FLAG : 0)
        this.writeUdp(UDP_C2S.HostStreamData, Buffer.concat([target, header, chunk]))
        if (i % 32 == 31) {
          await new Promise(resolve => setImmediate(resolve))
        }
      }
      this.frameIndex = (this.frameIndex + 1) & 0xfff
    }
    this.sending = false
  }

  rx = Buffer.alloc(0)
  plain = Buffer.alloc(0)
  commandId = 0
  tcpSendCounter = 0
  tcpRecvCounter = 0
  udpSendNonce = 0
  udpReplay = new ReplayWindow()
  frames = new Map()
  lastDelivered = -1
  waitingForKeyframe = true
  lastKeyframeRequest = 0
  sendQueue = []
  sending = false
  frameIndex = 0
  audioSequence = 0
}

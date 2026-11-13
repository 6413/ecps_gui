// Secure file transfer for web users, the same wire format and crypto as the desktop client
// (client/file_transfer.h), so a browser and a desktop user can send each other files.
//
//  - the sender offers a file with a fresh x25519 key, the receiver answers with its own,
//    both derive one chacha20-poly1305 key (hkdf over the dh result, both public keys, the
//    transfer id, the size and the name). the ecps server only relays sealed chunks.
//  - every chunk is sealed with its index as the nonce, so chunks can't be reordered,
//    replayed or cut off without the receiver noticing.
//  - sizes and chunk indexes are 64 bit on the wire and BigInt here, there is no size limit.
//  - browsers have no chacha20-poly1305, so this runs in the gateway, like the video decrypt.
//    The file travels between gateway and browser over the page's own connection (use
//    https for anything but localhost).
//  - a short verification code comes out of the same key derivation. If both users see the
//    same code nobody in the middle, the ecps server included, swapped the keys.
import crypto from 'node:crypto'
import { C2S, S2C, newX25519, open, seal, x25519PublicKey } from './ecps.js'

export const FRAME_FILE = 0x40
// handles chosen by the browser stay below this, the gateway numbers incoming files above it
export const INCOMING_HANDLE = 0x80000000

export const CHUNK = 4096
export const NAME_FIELD = 128
const TAG = 16
const WINDOW = 64
const ACK_EVERY = 16n
const MAX_PENDING_OFFERS = 8
const FILE_CONTEXT = 'ecps file v1'
const MAX_SAFE = BigInt(Number.MAX_SAFE_INTEGER)
const REPORT_MS = 250

// body layouts, packed little endian (prot.h, Channel_File_*)
const OFFER_SIZE = 178
const RESPONSE_SIZE = 43
const CHUNK_SIZE = 4132
const ACK_SIZE = 18
const CANCEL_SIZE = 10

const RESERVED = new Set(['CON', 'PRN', 'AUX', 'NUL',
  ...Array.from({ length: 9 }, (_, i) => `COM${i + 1}`),
  ...Array.from({ length: 9 }, (_, i) => `LPT${i + 1}`)])

// the name comes from the other side, it must never be able to leave the folder it is saved in
export function sanitizeName(raw) {
  let name = String(raw)
  name = name.slice(Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1)
  name = name.replace(/[\x00-\x1f<>:"|?*]/g, '_').replace(/[. ]+$/, '')
  if (!name) {
    name = 'file'
  }
  if (RESERVED.has(name.split('.')[0].toUpperCase())) {
    name = `_${name}`
  }
  return name
}

// the exact bytes both sides feed into the key derivation, cut on a character boundary
export function nameField(raw) {
  let bytes = Buffer.from(sanitizeName(raw))
  if (bytes.length > NAME_FIELD - 1) {
    let cut = NAME_FIELD - 1
    while (cut > 0 && (bytes[cut] & 0xc0) == 0x80) {
      --cut
    }
    bytes = bytes.subarray(0, cut)
  }
  const field = Buffer.alloc(NAME_FIELD)
  bytes.copy(field)
  return field
}

function fieldToName(field) {
  const end = field.indexOf(0)
  return sanitizeName(field.toString('utf8', 0, end < 0 ? field.length : end))
}

export function deriveFileKey(privateKey, ownPublic, peerPublic, outgoing, id, size, field) {
  const dh = crypto.diffieHellman({ privateKey, publicKey: x25519PublicKey(peerPublic) })
  const head = Buffer.alloc(12)
  head.writeUInt32LE(id, 0)
  head.writeBigUInt64LE(BigInt(size), 4)
  const salt = Buffer.concat([outgoing ? ownPublic : peerPublic, outgoing ? peerPublic : ownPublic, head, field])
  const okm = Buffer.from(crypto.hkdfSync('sha256', dh, salt, FILE_CONTEXT, 36))
  const n = okm.readUInt32LE(32) % 1000000
  const code = `${String(Math.floor(n / 1000)).padStart(3, '0')} ${String(n % 1000).padStart(3, '0')}`
  return { key: okm.subarray(0, 32), code }
}

export function fileAad(id, index, size) {
  const aad = Buffer.alloc(14)
  aad.writeUInt32LE(id, 0)
  aad.writeBigUInt64LE(BigInt(index), 4)
  aad.writeUInt16LE(size, 12)
  return aad
}

export function sealChunk(key, id, index, plain) {
  const sealed = seal(key, index, fileAad(id, index, plain.length), plain)
  return { cipher: sealed.subarray(0, plain.length), tag: sealed.subarray(plain.length) }
}

export function openChunk(key, id, index, cipher, tag) {
  return open(key, index, fileAad(id, index, cipher.length), Buffer.concat([cipher, tag]))
}

function header(size, channel, peer, id) {
  const body = Buffer.alloc(size)
  body.writeUInt16LE(channel, 0)
  body.writeUInt32LE(peer, 2)
  body.writeUInt32LE(id, 6)
  return body
}

export function offerBody(channel, peer, id, size, ephemeral, field) {
  const body = header(OFFER_SIZE, channel, peer, id)
  body.writeBigUInt64LE(BigInt(size), 10)
  ephemeral.copy(body, 18)
  field.copy(body, 50)
  return body
}

export function responseBody(channel, peer, id, ephemeral) {
  const body = header(RESPONSE_SIZE, channel, peer, id)
  if (ephemeral) {
    body[10] = 1
    ephemeral.copy(body, 11)
  }
  return body
}

export function chunkBody(channel, peer, id, index, cipher, tag) {
  const body = header(CHUNK_SIZE, channel, peer, id)
  body.writeBigUInt64LE(BigInt(index), 10)
  body.writeUInt16LE(cipher.length, 18)
  cipher.copy(body, 20)
  tag.copy(body, 20 + CHUNK)
  return body
}

function ackBody(channel, peer, id, received) {
  const body = header(ACK_SIZE, channel, peer, id)
  body.writeBigUInt64LE(received, 10)
  return body
}

function finished(t) {
  return t.state == 'done' || t.state == 'declined' || t.state == 'cancelled' || t.state == 'failed'
}

export class FileTransfers {
  // notify(message): a control message for the browser, deliver(handle, bytes): received data
  constructor(ecps, notify, deliver) {
    this.ecps = ecps
    this.notify = notify
    this.deliver = deliver
  }

  find(outgoing, peer, id) {
    for (const t of this.transfers.values()) {
      if (t.outgoing == outgoing && t.peer == peer && t.id == id) {
        return t
      }
    }
    return null
  }

  report(t, force) {
    const now = Date.now()
    if (!force && now - t.lastReport < REPORT_MS) {
      return
    }
    t.lastReport = now
    this.notify({
      t: 'fileState',
      handle: t.handle,
      state: t.state,
      status: t.status,
      transferred: Number(t.transferred),
      code: t.code
    })
  }

  // ends a transfer, telling the other side unless it was the one that ended it
  end(t, state, status, tellPeer) {
    if (finished(t)) {
      return
    }
    if (tellPeer) {
      this.ecps.writeCommand(C2S.FileCancel, header(CANCEL_SIZE, t.channel, t.peer, t.id))
    }
    t.key = null
    t.privateKey = null
    t.state = state
    t.status = status
    if (state == 'done') {
      t.transferred = t.size
    }
    this.report(t, true)
  }

  fail(handle, status) {
    this.notify({ t: 'fileState', handle, state: 'failed', status, transferred: 0, code: '' })
  }

  // ---- messages from the browser ----

  offer(handle, peer, rawName, rawSize) {
    const channel = this.ecps.channel
    const size = Number(rawSize)
    if (!Number.isInteger(handle) || handle <= 0 || handle >= INCOMING_HANDLE || this.transfers.has(handle)) {
      return
    }
    if (!channel || !Number.isInteger(peer) || peer < 0 || peer > 0xffffffff) {
      return this.fail(handle, 'join a channel with the other user first')
    }
    if (!Number.isSafeInteger(size) || size <= 0) {
      return this.fail(handle, 'the file is empty')
    }
    const field = nameField(rawName)
    const { privateKey, publicRaw } = newX25519()
    const t = {
      handle,
      outgoing: true,
      channel: channel.id,
      peer,
      id: crypto.randomBytes(4).readUInt32LE(),
      size: BigInt(size),
      total: (BigInt(size) + BigInt(CHUNK - 1)) / BigInt(CHUNK),
      next: 0n,
      acked: 0n,
      transferred: 0n,
      state: 'waiting',
      status: 'waiting for the receiver',
      code: '',
      field,
      privateKey,
      publicRaw,
      key: null,
      lastReport: 0
    }
    this.transfers.set(handle, t)
    this.ecps.writeCommand(C2S.FileOffer, offerBody(t.channel, peer, t.id, t.size, publicRaw, field))
    this.report(t, true)
  }

  // one chunk of the file, exactly CHUNK bytes except for the last one
  onData(handle, data) {
    const t = this.transfers.get(handle)
    if (!t || !t.outgoing || t.state != 'transferring') {
      return
    }
    const remaining = t.size - t.transferred
    const expected = remaining < BigInt(CHUNK) ? Number(remaining) : CHUNK
    if (data.length != expected) {
      return this.end(t, 'failed', 'the browser sent a chunk of the wrong size', true)
    }
    const { cipher, tag } = sealChunk(t.key, t.id, t.next, data)
    this.ecps.writeCommand(C2S.FileChunk, chunkBody(t.channel, t.peer, t.id, t.next, cipher, tag))
    t.next++
    t.transferred += BigInt(data.length)
    this.report(t, false)
  }

  accept(handle) {
    const t = this.transfers.get(handle)
    if (!t || t.outgoing || t.state != 'offered') {
      return
    }
    try {
      const { privateKey, publicRaw } = newX25519()
      const derived = deriveFileKey(privateKey, publicRaw, t.peerPublic, false, t.id, t.size, t.field)
      t.key = derived.key
      t.code = derived.code
      this.ecps.writeCommand(C2S.FileResponse, responseBody(t.channel, t.peer, t.id, publicRaw))
    }
    catch {
      return this.end(t, 'failed', 'key exchange failed', true)
    }
    t.state = 'transferring'
    t.status = 'receiving'
    this.report(t, true)
  }

  decline(handle) {
    const t = this.transfers.get(handle)
    if (!t || t.outgoing || t.state != 'offered') {
      return
    }
    this.ecps.writeCommand(C2S.FileResponse, responseBody(t.channel, t.peer, t.id, null))
    this.end(t, 'declined', 'you declined', false)
  }

  cancel(handle) {
    const t = this.transfers.get(handle)
    if (t) {
      this.end(t, 'cancelled', 'cancelled', true)
    }
  }

  // the browser wrote this many chunks of an incoming file, only then does the sender get
  // more window, so a slow disk slows the sender instead of filling memory
  consumed(handle, rawCount) {
    const t = this.transfers.get(handle)
    if (!t || t.outgoing || t.state != 'transferring' || !Number.isSafeInteger(rawCount) || rawCount < 0) {
      return
    }
    let count = BigInt(rawCount)
    if (count > t.next) {
      count = t.next
    }
    if (count <= t.consumed) {
      return
    }
    t.consumed = count
    const last = t.consumed == t.total
    if (last || t.consumed - t.acked >= ACK_EVERY) {
      t.acked = t.consumed
      this.ecps.writeCommand(C2S.FileAck, ackBody(t.channel, t.peer, t.id, t.consumed))
    }
    if (last) {
      this.end(t, 'done', 'saved', false)
    }
  }

  // the browser went away
  abortAll() {
    for (const t of this.transfers.values()) {
      this.end(t, 'cancelled', 'cancelled', true)
    }
  }

  // ---- messages from the ecps server ----

  onFile(command, body) {
    switch (command) {
      case S2C.FileOffer: return this.onOffer(body)
      case S2C.FileResponse: return this.onResponse(body)
      case S2C.FileChunk: return this.onChunk(body)
      case S2C.FileAck: return this.onAck(body)
      case S2C.FileCancel: return this.onCancel(body)
    }
  }

  onOffer(body) {
    const channel = body.readUInt16LE(0)
    const peer = body.readUInt32LE(2)
    const id = body.readUInt32LE(6)
    const size = body.readBigUInt64LE(10)
    const field = Buffer.from(body.subarray(50, 50 + NAME_FIELD))
    field[NAME_FIELD - 1] = 0

    let pending = 0
    for (const t of this.transfers.values()) {
      pending += !t.outgoing && t.state == 'offered'
    }
    if (size == 0n || size > MAX_SAFE || pending >= MAX_PENDING_OFFERS || this.find(false, peer, id)) {
      this.ecps.writeCommand(C2S.FileResponse, responseBody(channel, peer, id, null))
      return
    }
    const t = {
      handle: this.nextIncoming++,
      outgoing: false,
      channel,
      peer,
      id,
      size,
      total: (size + BigInt(CHUNK - 1)) / BigInt(CHUNK),
      next: 0n,
      consumed: 0n,
      acked: 0n,
      transferred: 0n,
      state: 'offered',
      status: 'waiting for you',
      code: '',
      field,
      peerPublic: Buffer.from(body.subarray(18, 50)),
      key: null,
      lastReport: 0
    }
    this.transfers.set(t.handle, t)
    this.notify({ t: 'fileOffer', handle: t.handle, from: peer, name: fieldToName(field), size: Number(size) })
  }

  onResponse(body) {
    const t = this.find(true, body.readUInt32LE(2), body.readUInt32LE(6))
    if (!t || t.state != 'waiting') {
      return
    }
    if (!body[10]) {
      return this.end(t, 'declined', 'declined by the receiver', false)
    }
    try {
      const derived = deriveFileKey(t.privateKey, t.publicRaw, Buffer.from(body.subarray(11, 43)), true, t.id, t.size, t.field)
      t.key = derived.key
      t.code = derived.code
    }
    catch {
      return this.end(t, 'failed', 'key exchange failed', true)
    }
    t.privateKey = null
    t.state = 'transferring'
    t.status = 'sending'
    this.report(t, true)
    this.notify({ t: 'fileCredit', handle: t.handle, chunks: WINDOW })
  }

  onAck(body) {
    const t = this.find(true, body.readUInt32LE(2), body.readUInt32LE(6))
    if (!t || t.state != 'transferring') {
      return
    }
    let acked = body.readBigUInt64LE(10)
    if (acked > t.next) {
      acked = t.next
    }
    if (acked > t.acked) {
      this.notify({ t: 'fileCredit', handle: t.handle, chunks: Number(acked - t.acked) })
      t.acked = acked
    }
    if (t.acked >= t.total) {
      this.end(t, 'done', 'sent', false)
    }
  }

  onChunk(body) {
    const t = this.find(false, body.readUInt32LE(2), body.readUInt32LE(6))
    if (!t || t.state != 'transferring') {
      return
    }
    const index = body.readBigUInt64LE(10)
    const size = body.readUInt16LE(18)
    const remaining = t.size - t.transferred
    const expected = remaining < BigInt(CHUNK) ? Number(remaining) : CHUNK
    if (index != t.next || size != expected) {
      return this.end(t, 'failed', 'protocol error', true)
    }
    const plain = openChunk(t.key, t.id, index, body.subarray(20, 20 + size), body.subarray(20 + CHUNK, 20 + CHUNK + TAG))
    if (!plain) {
      return this.end(t, 'failed', 'data was corrupted or tampered with', true)
    }
    t.next++
    t.transferred += BigInt(size)
    this.deliver(t.handle, plain)
    this.report(t, false)
  }

  onCancel(body) {
    const peer = body.readUInt32LE(2)
    const id = body.readUInt32LE(6)
    for (const outgoing of [true, false]) {
      const t = this.find(outgoing, peer, id)
      if (t) {
        this.end(t, 'cancelled', 'cancelled by the other side', false)
      }
    }
  }

  transfers = new Map()
  nextIncoming = INCOMING_HANDLE
}

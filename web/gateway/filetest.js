// Sends files between two gateway sessions through a running ecps server:
//   ECPS_HOST=127.0.0.1 ECPS_PORT=43254 npm run filetest
// It plays the browser on both sides, so it covers the server relay, the framing, the
// crypto, the flow control and the declines/cancels, but not the page itself.
import assert from 'node:assert/strict'
import crypto from 'node:crypto'
import os from 'node:os'
import path from 'node:path'
import { EcpsClient } from './ecps.js'
import { CHUNK, FileTransfers, FRAME_FILE } from './transfer.js'

const host = process.env.ECPS_HOST ?? '127.0.0.1'
const port = Number(process.env.ECPS_PORT ?? 43254)
const size = Number(process.env.FILE_SIZE ?? 20 * 1024 * 1024 + 123)
const knownHosts = path.join(os.tmpdir(), `ecps-filetest-${process.pid}`)

function once(emitter, event) {
  return new Promise(resolve => emitter.once(event, resolve))
}

// a session with the browser's side of the file protocol attached
function session(name) {
  const ecps = new EcpsClient(host, port, knownHosts)
  const s = { name, ecps, offers: [], states: new Map(), credits: new Map(), received: new Map(), waiters: [] }
  s.transfers = new FileTransfers(
    ecps,
    message => {
      if (message.t == 'fileOffer') {
        s.offers.push(message)
      }
      else if (message.t == 'fileState') {
        s.states.set(message.handle, message)
      }
      else if (message.t == 'fileCredit') {
        s.credits.set(message.handle, (s.credits.get(message.handle) ?? 0) + message.chunks)
        s.pump?.()
      }
      s.waiters = s.waiters.filter(w => !w())
    },
    (handle, data) => {
      const r = s.received.get(handle) ?? { hash: crypto.createHash('sha256'), chunks: 0, bytes: 0 }
      s.received.set(handle, r)
      r.hash.update(data)
      r.chunks++
      r.bytes += data.length
      // the browser reports what it wrote every 16 chunks and when it is done
      if (r.chunks % 16 == 0 || r.bytes == r.expected) {
        s.transfers.consumed(handle, r.chunks)
      }
    }
  )
  ecps.on('file', (command, body) => s.transfers.onFile(command, body))
  ecps.on('closed', reason => console.log(name, 'closed:', reason))
  return s
}

function waitFor(s, condition, what, ms = 60000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timed out waiting for ${what}`)), ms)
    const check = () => {
      if (condition()) {
        clearTimeout(timer)
        resolve()
        return true
      }
      return false
    }
    if (!check()) {
      s.waiters.push(check)
    }
  })
}

// feeds a buffer to the sender as the credits allow
function upload(s, handle, data) {
  let offset = 0
  s.pump = () => {
    while ((s.credits.get(handle) ?? 0) > 0 && offset < data.length) {
      s.credits.set(handle, s.credits.get(handle) - 1)
      const chunk = data.subarray(offset, Math.min(offset + CHUNK, data.length))
      offset += chunk.length
      s.transfers.onData(handle, chunk)
    }
  }
  s.pump()
}

const a = session('a')
const b = session('b')
a.ecps.connect()
await once(a.ecps, 'ready')
a.ecps.createChannel()
await once(a.ecps, 'created')
b.ecps.connect()
await once(b.ecps, 'ready')
b.ecps.join(a.ecps.channel.id)
await once(b.ecps, 'joined')
console.log('sessions', a.ecps.sessionId, b.ecps.sessionId, 'in channel', a.ecps.channel.id)

// a file with a size that is not a multiple of the chunk
{
  const data = crypto.randomBytes(size)
  const hash = crypto.createHash('sha256').update(data).digest('hex')
  const started = Date.now()
  a.transfers.offer(1, b.ecps.sessionId, '../evil/..\\name: "x".bin', size)
  await waitFor(b, () => b.offers.length == 1, 'the offer')
  const offer = b.offers[0]
  assert.equal(offer.name, 'name_ _x_.bin')
  assert.equal(offer.size, size)
  b.received.set(offer.handle, { hash: crypto.createHash('sha256'), chunks: 0, bytes: 0, expected: size })
  b.transfers.accept(offer.handle)
  await waitFor(a, () => a.states.get(1)?.state == 'transferring', 'the accept')
  upload(a, 1, data)
  await waitFor(a, () => a.states.get(1)?.state == 'done', 'the sender to finish')
  await waitFor(b, () => b.states.get(offer.handle)?.state == 'done', 'the receiver to finish')
  const r = b.received.get(offer.handle)
  assert.equal(r.bytes, size)
  assert.equal(r.hash.digest('hex'), hash)
  assert.equal(a.states.get(1).code, b.states.get(offer.handle).code)
  assert.match(a.states.get(1).code, /^\d{3} \d{3}$/)
  const seconds = (Date.now() - started) / 1000
  console.log(`transferred ${(size / 1048576).toFixed(1)} MiB in ${seconds.toFixed(2)} s (${(size / 1048576 / seconds).toFixed(1)} MiB/s), code ${a.states.get(1).code}`)
}

// the other direction, and a tiny file
{
  const data = crypto.randomBytes(5)
  b.transfers.offer(2, a.ecps.sessionId, 'tiny.txt', data.length)
  await waitFor(a, () => a.offers.length == 1, 'the offer')
  const offer = a.offers[0]
  a.received.set(offer.handle, { hash: crypto.createHash('sha256'), chunks: 0, bytes: 0, expected: data.length })
  a.transfers.accept(offer.handle)
  await waitFor(b, () => b.states.get(2)?.state == 'transferring', 'the accept')
  upload(b, 2, data)
  await waitFor(b, () => b.states.get(2)?.state == 'done', 'the sender to finish')
  await waitFor(a, () => a.states.get(offer.handle)?.state == 'done', 'the receiver to finish')
  assert.equal(a.received.get(offer.handle).hash.digest('hex'), crypto.createHash('sha256').update(data).digest('hex'))
}

// declined
{
  a.transfers.offer(3, b.ecps.sessionId, 'no.bin', 1000)
  await waitFor(b, () => b.offers.length == 2, 'the offer')
  b.transfers.decline(b.offers[1].handle)
  await waitFor(a, () => a.states.get(3)?.state == 'declined', 'the decline')
}

// cancelled by the sender while transferring
{
  a.transfers.offer(4, b.ecps.sessionId, 'cancel.bin', 10 * CHUNK)
  await waitFor(b, () => b.offers.length == 3, 'the offer')
  b.transfers.accept(b.offers[2].handle)
  await waitFor(a, () => a.states.get(4)?.state == 'transferring', 'the accept')
  a.transfers.cancel(4)
  await waitFor(b, () => b.states.get(b.offers[2].handle)?.state == 'cancelled', 'the cancel')
}

// a chunk with the wrong size ends the transfer
{
  a.transfers.offer(5, b.ecps.sessionId, 'bad.bin', 3 * CHUNK)
  await waitFor(b, () => b.offers.length == 4, 'the offer')
  b.transfers.accept(b.offers[3].handle)
  await waitFor(a, () => a.states.get(5)?.state == 'transferring', 'the accept')
  a.transfers.onData(5, Buffer.alloc(10))
  assert.equal(a.states.get(5).state, 'failed')
  await waitFor(b, () => b.states.get(b.offers[3].handle)?.state == 'cancelled', 'the cancel')
}

assert.ok(FRAME_FILE)
console.log('filetest ok')
a.ecps.close()
b.ecps.close()
process.exit(0)

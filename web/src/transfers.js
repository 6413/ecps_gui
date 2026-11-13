// Browser side of file transfer. The gateway does the encryption (see gateway/transfer.js),
// this file reads the files to send in small slices and writes the received ones to disk.
//
// - sending reads the file lazily, 256 KB at a time, so any file size works without memory
// - receiving streams to a file the user picks (Chrome, Edge), there is no size limit
//   there. Other browsers have no way to write a file while it arrives, so they keep it in
//   memory until it is complete, which is why they are limited to MEMORY_LIMIT.
export const FRAME_FILE = 0x40

const CHUNK = 4096
const READ_AHEAD = 64 * CHUNK
const CONSUMED_EVERY = 16
const MEMORY_LIMIT = 2 * 1024 * 1024 * 1024

export const canStreamToDisk = typeof window !== 'undefined' && typeof window.showSaveFilePicker == 'function'
export const memoryLimit = MEMORY_LIMIT

export function formatBytes(bytes) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    ++unit
  }
  return `${unit ? value.toFixed(2) : value} ${units[unit]}`
}

export function isFinished(item) {
  return item.state == 'done' || item.state == 'declined' || item.state == 'cancelled' || item.state == 'failed'
}

export class Transfers {
  // sendControl(object), sendBinary(Uint8Array), onChange(list of items), onError(text)
  constructor({ sendControl, sendBinary, onChange, onError }) {
    this.sendControl = sendControl
    this.sendBinary = sendBinary
    this.onChange = onChange
    this.onError = onError
  }

  emit() {
    this.onChange(this.items.map(item => ({ ...item })))
  }

  item(handle) {
    return this.items.find(item => item.handle == handle)
  }

  // ---- sending ----

  sendFile(file, peer) {
    const handle = this.nextHandle++
    const item = {
      handle,
      outgoing: true,
      peer,
      name: file.name,
      size: file.size,
      transferred: 0,
      state: 'waiting',
      status: 'waiting for the receiver',
      code: ''
    }
    this.items.push(item)
    if (!file.size) {
      item.state = 'failed'
      item.status = 'the file is empty'
    }
    else {
      this.outgoing.set(handle, { file, offset: 0, credits: 0, busy: false, buffer: null, bufferAt: 0 })
      this.sendControl({ t: 'fileSend', handle, peer, name: file.name, size: file.size })
    }
    this.emit()
  }

  // the gateway says how many more chunks it will take
  async pump(handle) {
    const r = this.outgoing.get(handle)
    if (!r || r.busy) {
      return
    }
    r.busy = true
    try {
      while (r.credits > 0 && r.offset < r.file.size && this.outgoing.get(handle) === r) {
        if (!r.buffer || r.bufferAt >= r.buffer.length) {
          const end = Math.min(r.offset + READ_AHEAD, r.file.size)
          const buffer = new Uint8Array(await r.file.slice(r.offset, end).arrayBuffer())
          if (this.outgoing.get(handle) !== r) {
            return
          }
          if (buffer.length != end - r.offset) {
            throw new Error('the file changed while it was being sent')
          }
          r.buffer = buffer
          r.bufferAt = 0
        }
        const n = Math.min(CHUNK, r.buffer.length - r.bufferAt)
        const frame = new Uint8Array(5 + n)
        frame[0] = FRAME_FILE
        new DataView(frame.buffer).setUint32(1, handle, true)
        frame.set(r.buffer.subarray(r.bufferAt, r.bufferAt + n), 5)
        this.sendBinary(frame)
        r.bufferAt += n
        r.offset += n
        r.credits--
      }
    }
    catch (e) {
      this.outgoing.delete(handle)
      this.sendControl({ t: 'fileCancel', handle })
      this.setState(handle, 'failed', e?.message ?? 'could not read the file')
    }
    finally {
      r.busy = false
    }
    if (r.credits > 0 && r.offset < r.file.size && this.outgoing.get(handle) === r) {
      this.pump(handle)
    }
  }

  // ---- receiving ----

  // call this straight from the click: the save dialog needs the user's gesture
  async accept(handle) {
    const item = this.item(handle)
    if (!item || item.outgoing || item.state != 'offered') {
      return
    }
    const total = Math.ceil(item.size / CHUNK)
    let sink
    try {
      if (canStreamToDisk) {
        const file = await window.showSaveFilePicker({ suggestedName: item.name })
        sink = { writable: await file.createWritable(), parts: null }
      }
      else if (item.size > MEMORY_LIMIT) {
        this.onError(`This browser can't save a file this big. Use Chrome or Edge to receive ${item.name}.`)
        return
      }
      else {
        sink = { writable: null, parts: [] }
      }
    }
    catch {
      // the save dialog was closed, the offer stays open
      return
    }
    Object.assign(sink, { handle, name: item.name, total, chunks: 0, chain: Promise.resolve(), failed: false })
    this.sinks.set(handle, sink)
    this.sendControl({ t: 'fileAccept', handle })
  }

  decline(handle) {
    this.sendControl({ t: 'fileDecline', handle })
  }

  cancel(handle) {
    this.sendControl({ t: 'fileCancel', handle })
  }

  onBinary(buffer) {
    const handle = new DataView(buffer).getUint32(1, true)
    const sink = this.sinks.get(handle)
    if (!sink || sink.failed) {
      return
    }
    const data = new Uint8Array(buffer, 5)
    const count = ++sink.chunks
    const last = count == sink.total
    // writes happen in order, and the gateway only hears about a chunk once it is written
    sink.chain = sink.chain.then(async () => {
      if (sink.failed) {
        return
      }
      if (sink.writable) {
        await sink.writable.write(data)
      }
      else {
        sink.parts.push(data)
      }
      if (last) {
        await this.finish(sink)
      }
      if (last || count % CONSUMED_EVERY == 0) {
        this.sendControl({ t: 'fileConsumed', handle, count })
      }
    }).catch(() => this.sinkFailed(sink, 'could not write the file'))
  }

  async finish(sink) {
    if (sink.writable) {
      await sink.writable.close()
    }
    else {
      const url = URL.createObjectURL(new Blob(sink.parts))
      const link = document.createElement('a')
      link.href = url
      link.download = sink.name
      document.body.append(link)
      link.click()
      link.remove()
      setTimeout(() => URL.revokeObjectURL(url), 60000)
      sink.parts = []
    }
    this.sinks.delete(sink.handle)
  }

  sinkFailed(sink, text) {
    if (sink.failed) {
      return
    }
    sink.failed = true
    sink.parts = null
    sink.writable?.abort().catch(() => {})
    this.sinks.delete(sink.handle)
    this.sendControl({ t: 'fileCancel', handle: sink.handle })
    this.setState(sink.handle, 'failed', text)
  }

  // ---- messages from the gateway ----

  setState(handle, state, status) {
    const item = this.item(handle)
    if (item) {
      item.state = state
      item.status = status
      this.emit()
    }
  }

  // returns true when the message was about files
  handle(message) {
    switch (message.t) {
      case 'fileOffer': {
        this.items.push({
          handle: message.handle,
          outgoing: false,
          peer: message.from,
          name: message.name,
          size: message.size,
          transferred: 0,
          state: 'offered',
          status: 'waiting for you',
          code: ''
        })
        this.emit()
        return true
      }
      case 'fileState': {
        const item = this.item(message.handle)
        if (item) {
          item.state = message.state
          item.status = message.status
          item.transferred = message.transferred
          item.code = message.code
          if (isFinished(item)) {
            this.outgoing.delete(item.handle)
            const sink = this.sinks.get(item.handle)
            // done is closed by the data path, anything else means the file is incomplete
            if (sink && item.state != 'done') {
              sink.failed = true
              sink.parts = null
              sink.writable?.abort().catch(() => {})
              this.sinks.delete(item.handle)
            }
          }
          this.emit()
        }
        return true
      }
      case 'fileCredit': {
        const r = this.outgoing.get(message.handle)
        if (r) {
          r.credits += message.chunks
          this.pump(message.handle)
        }
        return true
      }
    }
    return false
  }

  clearFinished() {
    this.items = this.items.filter(item => !isFinished(item))
    this.emit()
  }

  // the session is gone, so are its transfers
  reset() {
    for (const sink of this.sinks.values()) {
      sink.failed = true
      sink.parts = null
      sink.writable?.abort().catch(() => {})
    }
    this.sinks.clear()
    this.outgoing.clear()
    for (const item of this.items) {
      if (!isFinished(item)) {
        item.state = 'failed'
        item.status = 'the connection was closed'
      }
    }
    this.emit()
  }

  items = []
  outgoing = new Map()
  sinks = new Map()
  nextHandle = 1
}

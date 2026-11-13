// Serves the web page and bridges each browser to its own ECPS session.
import crypto from 'node:crypto'
import fs from 'node:fs'
import http from 'node:http'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { WebSocketServer } from 'ws'
import { EcpsClient, FRAME_AUDIO, FRAME_KEY } from './ecps.js'
import { FRAME_FILE, FileTransfers } from './transfer.js'

const here = path.dirname(fileURLToPath(import.meta.url))
const distDir = path.join(here, '..', 'dist')
const knownHostsPath = path.join(here, 'known_hosts')

const ecpsHost = process.env.ECPS_HOST ?? '127.0.0.1'
const ecpsPort = Number(process.env.ECPS_PORT ?? 43254)
const webHost = process.env.WEB_HOST ?? '127.0.0.1'
const webPort = Number(process.env.WEB_PORT ?? 8080)
const password = process.env.WEB_PASSWORD ?? ''

const MAX_BUFFERED = 1 << 20
const MAX_FRAME = 4 << 20

const mimeTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon'
}

function serveStatic(req, res) {
  const url = new URL(req.url, 'http://localhost')
  const name = url.pathname == '/' ? 'index.html' : decodeURIComponent(url.pathname).slice(1)
  const file = path.resolve(distDir, name)
  if (file != distDir && !file.startsWith(distDir + path.sep)) {
    res.writeHead(403).end()
    return
  }
  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404).end('not found')
      return
    }
    res.writeHead(200, { 'content-type': mimeTypes[path.extname(file)] ?? 'application/octet-stream' })
    res.end(data)
  })
}

function hash(text) {
  return crypto.createHash('sha256').update(text).digest()
}

function passwordMatches(text) {
  return crypto.timingSafeEqual(hash(text ?? ''), hash(password))
}

function bridge(ws) {
  let ecps = null
  let transfers = null
  let authed = false
  let streamBlocked = false

  const send = message => {
    if (ws.readyState == ws.OPEN) {
      ws.send(JSON.stringify(message))
    }
  }

  const start = () => {
    ecps = new EcpsClient(ecpsHost, ecpsPort, knownHostsPath)
    transfers = new FileTransfers(ecps, send, (handle, data) => {
      if (ws.readyState == ws.OPEN) {
        const head = Buffer.alloc(5)
        head[0] = FRAME_FILE
        head.writeUInt32LE(handle, 1)
        ws.send(Buffer.concat([head, data]), { binary: true })
      }
    })
    ecps.on('file', (command, body) => transfers.onFile(command, body))
    ecps.on('ready', () => send({ t: 'ready', sessionId: ecps.sessionId }))
    ecps.on('channels', channels => send({ t: 'channels', channels }))
    ecps.on('users', list => send({ t: 'users', ...list }))
    for (const t of ['joined', 'created', 'kicked']) {
      ecps.on(t, channel => send({ t, channel }))
    }
    ecps.on('channelError', () => send({ t: 'error', message: 'could not enter that channel' }))
    ecps.on('keyframeWanted', () => send({ t: 'keyframe' }))
    ecps.on('closed', reason => {
      send({ t: 'closed', reason })
      ws.close()
    })
    ecps.on('audio', packet => {
      if (ws.readyState == ws.OPEN && ws.bufferedAmount <= MAX_BUFFERED) {
        ws.send(Buffer.concat([Buffer.from([FRAME_AUDIO]), packet]), { binary: true })
      }
    })
    ecps.on('frame', (data, keyframe) => {
      if (ws.readyState != ws.OPEN) {
        return
      }
      // a slow phone gets fewer frames, not more delay
      if (ws.bufferedAmount > MAX_BUFFERED || (streamBlocked && !keyframe)) {
        streamBlocked = true
        ecps.requestKeyframe()
        return
      }
      streamBlocked = false
      ws.send(Buffer.concat([Buffer.from([keyframe ? FRAME_KEY : 0]), data]), { binary: true })
    })
    ecps.connect()
  }

  const onControl = message => {
    switch (message.t) {
      case 'hello': {
        if (authed) {
          break
        }
        if (password && !passwordMatches(String(message.password ?? ''))) {
          send({ t: 'denied' })
          ws.close()
          break
        }
        authed = true
        start()
        break
      }
      case 'join': {
        if (Number.isInteger(message.channel)) {
          ecps.join(message.channel & 0xffff)
        }
        break
      }
      case 'users': {
        if (Number.isInteger(message.channel)) {
          ecps.requestUsers(message.channel & 0xffff)
        }
        break
      }
      case 'share': {
        ecps.createChannel()
        break
      }
      case 'keyframe': {
        ecps.requestKeyframe()
        break
      }
      case 'fileSend': {
        transfers?.offer(message.handle, message.peer, message.name, message.size)
        break
      }
      case 'fileAccept': {
        transfers?.accept(message.handle)
        break
      }
      case 'fileDecline': {
        transfers?.decline(message.handle)
        break
      }
      case 'fileCancel': {
        transfers?.cancel(message.handle)
        break
      }
      case 'fileConsumed': {
        transfers?.consumed(message.handle, message.count)
        break
      }
    }
  }

  // tell the other side of any running transfer before the session goes away
  const closeSession = () => {
    transfers?.abortAll()
    setTimeout(() => ecps?.close(), 100)
  }

  ws.on('message', (data, isBinary) => {
    if (isBinary) {
      if (authed && data.length > 1 && data[0] & FRAME_AUDIO) {
        ecps.sendAudio(data.subarray(1))
      }
      else if (authed && data.length > 5 && data[0] & FRAME_FILE) {
        transfers.onData(data.readUInt32LE(1), data.subarray(5))
      }
      else if (authed && data.length > 1 && data.length <= MAX_FRAME) {
        if (!ecps.sendFrame(data.subarray(1), (data[0] & FRAME_KEY) != 0)) {
          send({ t: 'keyframe' })
        }
      }
      return
    }
    try {
      onControl(JSON.parse(data.toString()))
    }
    catch {}
  })
  ws.on('close', closeSession)
  ws.on('error', closeSession)
}

const server = http.createServer(serveStatic)
const wss = new WebSocketServer({ server, path: '/ws', maxPayload: MAX_FRAME })
wss.on('connection', bridge)
server.listen(webPort, webHost, () => {
  console.log(`web page on http://${webHost}:${webPort}, ecps server ${ecpsHost}:${ecpsPort}`)
  if (!password) {
    console.log('no WEB_PASSWORD set, anyone with the link can watch and share')
  }
})

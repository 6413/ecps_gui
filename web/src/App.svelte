<script>
  import { onDestroy } from 'svelte'
  import { Viewer, Sharer, canView, canShare } from './media.js'
  import { MseViewer, canUseMse } from './mse.js'

  const shareSupported = canShare()
  // ?mse forces the fallback player on browsers that have WebCodecs
  const useMse = canUseMse() && (!canView() || new URLSearchParams(location.search).has('mse'))
  const viewSupported = canView() || useMse

  let status = $state('connecting')
  let password = $state('')
  let needsPassword = $state(false)
  let channels = $state([])
  let usersByChannel = $state({})
  let sessionId = $state(null)
  let current = $state(null)
  let error = $state('')
  let canvas = $state()
  let video = $state()
  let stage = $state()
  let view = $state()
  let zoom = $state(1)
  let panX = $state(0)
  let panY = $state(0)

  let ws = null
  let viewer = null
  let sharer = null
  let pendingStream = null
  let pendingJoin = null
  let reconnectTimer = null
  let retryMs = 500
  let destroyed = false
  let pinchDistance = 0
  const pointers = new Map()

  const MAX_ZOOM = 8
  const ZOOM_STEP = 1.5
  const WHEEL_SPEED = 0.002

  const users = $derived(current ? usersByChannel[current.channel] ?? [] : [])
  const visibleChannels = $derived(channels.filter(c => c.users > 0))

  function send(message) {
    if (ws?.readyState == WebSocket.OPEN) {
      ws.send(JSON.stringify(message))
    }
  }

  function connect() {
    clearTimeout(reconnectTimer)
    ws?.close()
    status = 'connecting'
    const scheme = location.protocol == 'https:' ? 'wss:' : 'ws:'
    const socket = new WebSocket(`${scheme}//${location.host}/ws`)
    socket.binaryType = 'arraybuffer'
    ws = socket
    socket.onopen = () => send({ t: 'hello', password })
    socket.onmessage = e => {
      if (typeof e.data == 'string') {
        onControl(JSON.parse(e.data))
      }
      else {
        viewer?.push(e.data)
      }
    }
    socket.onclose = () => {
      if (ws != socket || destroyed) {
        return
      }
      stopMedia()
      current = null
      if (needsPassword) {
        return
      }
      status = 'offline'
      reconnectTimer = setTimeout(connect, retryMs)
      retryMs = Math.min(retryMs * 2, 8000)
    }
  }

  function onControl(message) {
    switch (message.t) {
      case 'ready': {
        status = 'online'
        needsPassword = false
        retryMs = 500
        sessionId = message.sessionId
        if (pendingStream) {
          send({ t: 'share' })
        }
        else if (pendingJoin != null) {
          send({ t: 'join', channel: pendingJoin })
          pendingJoin = null
        }
        break
      }
      case 'denied': {
        needsPassword = true
        status = 'offline'
        break
      }
      case 'channels': {
        channels = message.channels
        break
      }
      case 'users': {
        usersByChannel[message.channel] = message.users
        break
      }
      case 'joined': {
        current = { channel: message.channel, role: 'viewer' }
        startViewer()
        break
      }
      case 'created': {
        current = { channel: message.channel, role: 'host' }
        startSharing()
        break
      }
      case 'kicked': {
        stopMedia()
        current = null
        error = 'the host stopped sharing'
        break
      }
      case 'keyframe': {
        sharer?.requestKeyframe()
        break
      }
      case 'error': {
        error = message.message
        break
      }
      case 'closed': {
        error = message.reason
        break
      }
    }
  }

  function startViewer() {
    stopMedia()
    const requestKeyframe = () => send({ t: 'keyframe' })
    const onError = text => { error = text }
    viewer = useMse ? new MseViewer(video, requestKeyframe, onError) : new Viewer(canvas, requestKeyframe, onError)
  }

  async function startSharing() {
    const stream = pendingStream
    pendingStream = null
    sharer = new Sharer(
      stream,
      packet => {
        if (ws?.readyState == WebSocket.OPEN) {
          ws.send(packet)
        }
      },
      leave,
      text => { error = text }
    )
    if (!await sharer.start()) {
      leave()
    }
  }

  function stopMedia() {
    viewer?.close()
    viewer = null
    resetZoom()
    sharer?.stop()
    sharer = null
  }

  // the server can't leave a channel yet, a new session does the same thing
  function freshSession() {
    stopMedia()
    current = null
    connect()
  }

  function join(channel) {
    error = ''
    if (current) {
      pendingJoin = channel
      freshSession()
    }
    else {
      send({ t: 'join', channel })
    }
  }

  function leave() {
    error = ''
    freshSession()
  }

  async function share() {
    error = ''
    try {
      pendingStream = await navigator.mediaDevices.getDisplayMedia({
        video: { frameRate: 60 },
        audio: false
      })
    }
    catch {
      return
    }
    if (current) {
      freshSession()
    }
    else {
      send({ t: 'share' })
    }
  }

  function fullscreen() {
    if (document.fullscreenElement) {
      document.exitFullscreen()
    }
    else {
      stage.requestFullscreen?.()
    }
  }

  function clampPan() {
    const { width, height } = view.getBoundingClientRect()
    const limitX = width * (zoom - 1) / 2
    const limitY = height * (zoom - 1) / 2
    panX = Math.min(Math.max(panX, -limitX), limitX)
    panY = Math.min(Math.max(panY, -limitY), limitY)
  }

  // x and y are relative to the center of the view, that point stays under the cursor
  function zoomAt(next, x = 0, y = 0) {
    next = Math.min(Math.max(next, 1), MAX_ZOOM)
    const ratio = next / zoom
    panX = x - (x - panX) * ratio
    panY = y - (y - panY) * ratio
    zoom = next
    clampPan()
  }

  function resetZoom() {
    zoom = 1
    panX = 0
    panY = 0
  }

  function fromCenter(x, y) {
    const r = view.getBoundingClientRect()
    return [x - r.left - r.width / 2, y - r.top - r.height / 2]
  }

  function onWheel(e) {
    e.preventDefault()
    zoomAt(zoom * Math.exp(-e.deltaY * WHEEL_SPEED), ...fromCenter(e.clientX, e.clientY))
  }

  function onPointerDown(e) {
    view.setPointerCapture(e.pointerId)
    pointers.set(e.pointerId, [e.clientX, e.clientY])
    pinchDistance = 0
  }

  function onPointerMove(e) {
    const last = pointers.get(e.pointerId)
    if (!last) {
      return
    }
    pointers.set(e.pointerId, [e.clientX, e.clientY])
    if (pointers.size == 1) {
      panX += e.clientX - last[0]
      panY += e.clientY - last[1]
      clampPan()
    }
    else if (pointers.size == 2) {
      const [a, b] = [...pointers.values()]
      const distance = Math.hypot(a[0] - b[0], a[1] - b[1])
      if (pinchDistance) {
        zoomAt(zoom * distance / pinchDistance, ...fromCenter((a[0] + b[0]) / 2, (a[1] + b[1]) / 2))
      }
      pinchDistance = distance
    }
  }

  function onPointerUp(e) {
    pointers.delete(e.pointerId)
    pinchDistance = 0
  }

  function onDoubleClick(e) {
    if (zoom > 1) {
      resetZoom()
    }
    else {
      zoomAt(2, ...fromCenter(e.clientX, e.clientY))
    }
  }

  // not an onwheel attribute: preventDefault needs a non-passive listener
  $effect(() => {
    if (!view) {
      return
    }
    view.addEventListener('wheel', onWheel, { passive: false })
    return () => view.removeEventListener('wheel', onWheel)
  })

  function submitPassword(e) {
    e.preventDefault()
    needsPassword = false
    connect()
  }

  connect()

  onDestroy(() => {
    destroyed = true
    clearTimeout(reconnectTimer)
    stopMedia()
    ws?.close()
  })
</script>

<div class="app">
  <aside class="side">
    <header>
      <h1>ECPS</h1>
      <span class="status {status}">{status}</span>
    </header>

    {#if needsPassword}
      <form onsubmit={submitPassword}>
        <label for="password">Password</label>
        <input id="password" type="password" autocomplete="current-password" bind:value={password}>
        <button class="primary" type="submit">Enter</button>
      </form>
    {:else}
      <section>
        <h2>Channels</h2>
        {#if visibleChannels.length == 0}
          <p class="muted">No channels yet.</p>
        {/if}
        <ul>
          {#each visibleChannels as channel (channel.id)}
            <li class:active={current?.channel == channel.id}>
              <button class="row" disabled={status != 'online' || !viewSupported} onclick={() => join(channel.id)}>
                <span>{channel.name}{channel.hostSessionId == sessionId ? ' (you)' : ''}</span>
                <span class="muted">{Math.max(channel.users - 1, 0)} watching</span>
              </button>
            </li>
          {/each}
        </ul>
      </section>

      {#if current}
        <section>
          <h2>Users</h2>
          <ul>
            {#each users as user (user.sessionId)}
              <li class="user">
                {user.name}{user.sessionId == sessionId ? ' (you)' : ''}
                {#if user.isHost}<span class="muted">host</span>{/if}
              </li>
            {/each}
          </ul>
        </section>
      {/if}

      <div class="actions">
        {#if shareSupported}
          <button class="primary" disabled={status != 'online' || current?.role == 'host'} onclick={share}>
            Share screen
          </button>
        {/if}
        {#if current}
          <button onclick={leave}>{current.role == 'host' ? 'Stop sharing' : 'Leave'}</button>
        {/if}
      </div>
    {/if}

    {#if !viewSupported}
      <p class="error">This browser can't play H.264 streams here. Open the https:// link, or try Chrome.</p>
    {/if}
    {#if error}
      <p class="error">{error}</p>
    {/if}
  </aside>

  <main class="stage" bind:this={stage}>
    <div
      class="view"
      class:hidden={current?.role != 'viewer'}
      class:zoomed={zoom > 1}
      bind:this={view}
      role="application"
      aria-label="Stream"
      style:transform="translate({panX}px, {panY}px) scale({zoom})"
      onpointerdown={onPointerDown}
      onpointermove={onPointerMove}
      onpointerup={onPointerUp}
      onpointercancel={onPointerUp}
      ondblclick={onDoubleClick}
    >
      <canvas bind:this={canvas} class:hidden={useMse}></canvas>
      <video bind:this={video} class:hidden={!useMse} muted playsinline></video>
    </div>
    {#if current?.role == 'viewer'}
      <div class="controls">
        <button aria-label="Zoom out" disabled={zoom <= 1} onclick={() => zoomAt(zoom / ZOOM_STEP)}>-</button>
        <button aria-label="Reset zoom" disabled={zoom == 1} onclick={resetZoom}>{Math.round(zoom * 100)}%</button>
        <button aria-label="Zoom in" disabled={zoom >= MAX_ZOOM} onclick={() => zoomAt(zoom * ZOOM_STEP)}>+</button>
        <button onclick={fullscreen}>Fullscreen</button>
      </div>
    {:else if current?.role == 'host'}
      <p class="muted">You are sharing your screen.</p>
    {:else}
      <p class="muted">Pick a channel to watch.</p>
    {/if}
  </main>
</div>

<style>
  :global(:root) {
    --bg: #101114;
    --panel: #181a1f;
    --line: #2a2d34;
    --text: #e8e9ec;
    --muted: #9195a0;
    --accent: #4f8cff;
    --danger: #ff6b6b;
  }
  :global(html, body, #app) {
    height: 100%;
    margin: 0;
  }
  :global(body) {
    background: var(--bg);
    color: var(--text);
    font: 15px/1.45 system-ui, sans-serif;
  }
  .app {
    display: grid;
    grid-template-columns: 280px 1fr;
    height: 100dvh;
  }
  .side {
    display: flex;
    flex-direction: column;
    gap: 16px;
    padding: 16px;
    background: var(--panel);
    border-right: 1px solid var(--line);
    overflow-y: auto;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  h1 {
    margin: 0;
    font-size: 18px;
  }
  h2 {
    margin: 0 0 8px;
    color: var(--muted);
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }
  ul {
    display: flex;
    flex-direction: column;
    gap: 4px;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  button, input {
    font: inherit;
    color: inherit;
  }
  button {
    min-height: 40px;
    padding: 8px 12px;
    background: transparent;
    border: 1px solid var(--line);
    border-radius: 6px;
    cursor: pointer;
  }
  button:disabled {
    opacity: 0.5;
    cursor: default;
  }
  input {
    width: 100%;
    box-sizing: border-box;
    min-height: 40px;
    padding: 8px;
    background: var(--bg);
    border: 1px solid var(--line);
    border-radius: 6px;
  }
  form {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .primary {
    background: var(--accent);
    border-color: var(--accent);
    color: #fff;
  }
  .row {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    width: 100%;
    text-align: left;
  }
  li.active .row {
    border-color: var(--accent);
  }
  .user {
    display: flex;
    justify-content: space-between;
    padding: 4px 0;
  }
  .actions {
    display: flex;
    flex-direction: column;
    gap: 8px;
    margin-top: auto;
  }
  .muted {
    color: var(--muted);
  }
  .error {
    margin: 0;
    color: var(--danger);
  }
  .status {
    color: var(--muted);
    font-size: 12px;
  }
  .status.online {
    color: #4cd08a;
  }
  .stage {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    min-width: 0;
    min-height: 0;
    background: #000;
  }
  canvas, video {
    max-width: 100%;
    max-height: 100%;
  }
  .hidden {
    display: none;
  }
  .stage {
    overflow: hidden;
  }
  .view {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    touch-action: none;
    user-select: none;
  }
  .view.zoomed {
    cursor: grab;
  }
  .view.hidden {
    display: none;
  }
  .controls {
    position: absolute;
    right: 12px;
    bottom: 12px;
    display: flex;
    gap: 4px;
  }
  .controls button {
    min-width: 40px;
    background: rgba(0, 0, 0, 0.6);
  }
  @media (max-width: 720px) {
    .app {
      grid-template-columns: 1fr;
      grid-template-rows: 56dvh 1fr;
    }
    .stage {
      grid-row: 1;
    }
    .side {
      grid-row: 2;
      border-right: 0;
      border-top: 1px solid var(--line);
    }
  }
</style>

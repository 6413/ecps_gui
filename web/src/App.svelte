<script>
  import { onDestroy } from 'svelte'
  import { Viewer, Sharer, RecordingSharer, canView, canShare, canShareCamera, canRecordScreen, canRecordCamera, canRecordAudio } from './media.js'
  import { MseViewer, canUseMse } from './mse.js'
  import { AudioPlayer, AudioSharer, FRAME_AUDIO, canPlayAudio, canShareAudio } from './audio.js'
  import { Transfers, FRAME_FILE, canStreamToDisk, formatBytes, isFinished, memoryLimit } from './transfers.js'

  // the gateway says on connect whether it can turn a recording into H.264 (ffmpeg), for browsers without an encoder
  let recorderAvailable = $state(false)
  // this browser encodes H.264 itself, else it records and the gateway converts
  const encodesItself = typeof VideoEncoder != 'undefined'
  const shareSupported = $derived(canShare() || (!encodesItself && recorderAvailable && canRecordScreen()))
  const cameraSupported = $derived(canShareCamera() || (!encodesItself && recorderAvailable && canRecordCamera()))
  // ?mse forces the fallback player on browsers that have WebCodecs
  const useMse = canUseMse() && (!canView() || new URLSearchParams(location.search).has('mse'))
  const viewSupported = canView() || useMse
  const audioPlaySupported = canPlayAudio()
  // audio goes with the encoder path (WebCodecs audio) or in the recording
  const audioShareSupported = $derived(encodesItself ? canShareAudio() : recorderAvailable && canRecordAudio())

  function loadSetting(key, fallback) {
    try {
      const value = localStorage.getItem(key)
      return value == null ? fallback : value == 'true'
    }
    catch {
      return fallback
    }
  }

  function saveSetting(key, value) {
    try {
      localStorage.setItem(key, String(value))
    }
    catch {}
  }

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
  let shareAudio = $state(loadSetting('ecps.shareAudio', true))
  let audioMuted = $state(loadSetting('ecps.audioMuted', false))
  let transfers = $state([])
  let dragging = $state(false)
  let dropPending = $state(null)
  let fileInput = $state()
  let sendTarget = null

  let ws = null
  let viewer = null
  let sharer = null
  let audio = null
  let audioSharer = null
  let pendingStream = null
  // { channel, password } to join once the next session is ready
  let pendingJoin = null
  // the channel asking for its password: { channel, text, message }
  let lockPrompt = $state(null)
  let newChannelPassword = $state('')
  // sharing and watching are apart, like in the app: in a channel you can share and watch someone else at once
  let sharing = $state(false)
  // who shares in the channel (session ids, from the server), and whose screen this one shows
  let streamers = $state([])
  let watched = $state(null)
  let passwordNote = $state('')
  let reconnectTimer = null
  let retryMs = 500
  let destroyed = false
  let pinchDistance = 0
  const pointers = new Map()

  const MAX_ZOOM = 8
  const ZOOM_STEP = 1.5
  const WHEEL_SPEED = 0.002

  const users = $derived(current ? usersByChannel[current.channel] ?? [] : [])
  // empty ones too: a channel stays on the server after everyone left, like in the app
  const visibleChannels = $derived(channels)
  const currentLocked = $derived(channels.find(c => c.id == current?.channel)?.locked ?? false)
  // the host by the server's list: the one who made it, or its maker come back to a channel that stayed
  const isHost = $derived(current != null && channels.find(c => c.id == current.channel)?.hostSessionId == sessionId)

  function send(message) {
    if (ws?.readyState == WebSocket.OPEN) {
      ws.send(JSON.stringify(message))
    }
  }

  const files = new Transfers({
    sendControl: send,
    // the gateway hands out credits, so at most a few hundred KB are ever queued here
    sendBinary: data => {
      if (ws?.readyState == WebSocket.OPEN) {
        ws.send(data)
      }
    },
    onChange: list => { transfers = list },
    onError: text => { error = text }
  })

  function connect() {
    clearTimeout(reconnectTimer)
    ws?.close()
    files.reset()
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
      else if (new Uint8Array(e.data, 0, 1)[0] & FRAME_AUDIO) {
        audio?.push(e.data)
      }
      else if (new Uint8Array(e.data, 0, 1)[0] & FRAME_FILE) {
        files.onBinary(e.data)
      }
      else {
        viewer?.push(e.data)
      }
    }
    socket.onclose = () => {
      if (ws != socket || destroyed) {
        return
      }
      files.reset()
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
    if (files.handle(message)) {
      return
    }
    switch (message.t) {
      case 'ready': {
        status = 'online'
        needsPassword = false
        retryMs = 500
        sessionId = message.sessionId
        recorderAvailable = !!message.recorder
        if (pendingStream) {
          send({ t: 'share' })
        }
        else if (pendingJoin != null) {
          send({ t: 'join', channel: pendingJoin.channel, password: pendingJoin.password })
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
        streamers = []
        watched = null
        // the host's screen first (all an older server sends). A newer one says right after who shares, and the
        // screen is picked again from that
        {
          const host = channels.find(c => c.id == message.channel)?.hostSessionId
          if (host != null && host != 0xffffffff && host != sessionId) {
            watchSource(host)
          }
        }
        break
      }
      case 'created': {
        current = { channel: message.channel, role: 'host' }
        streamers = []
        watched = null
        startSharing()
        break
      }
      case 'streamers': {
        if (message.channel == current?.channel) {
          streamers = message.streamers
          pickWatched()
        }
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
      case 'channelError': {
        if (message.reason == 'passwordRequired' || message.reason == 'wrongPassword') {
          lockPrompt = {
            channel: message.channel,
            text: '',
            message: message.reason == 'wrongPassword' ? 'Wrong password' : 'This channel has a password'
          }
        }
        else {
          error = 'could not enter that channel'
        }
        break
      }
      case 'closed': {
        error = message.reason
        break
      }
    }
  }

  // the screen shown: the one picked while it still shares, else the first one sharing that isn't this session
  function pickWatched() {
    const others = streamers.filter(id => id != sessionId)
    if (watched != null && others.includes(watched)) {
      return
    }
    watchSource(others[0] ?? null)
  }

  function watchSource(source) {
    if (source == watched && (source == null || viewer)) {
      return
    }
    watched = source
    if (source == null) {
      stopViewer()
      return
    }
    send({ t: 'watch', source })
    startViewer()
  }

  function stopViewer() {
    viewer?.close()
    viewer = null
    audio?.close()
    audio = null
    resetZoom()
  }

  function startViewer() {
    stopViewer()
    const requestKeyframe = () => send({ t: 'keyframe' })
    const onError = text => { error = text }
    viewer = useMse ? new MseViewer(video, requestKeyframe, onError) : new Viewer(canvas, requestKeyframe, onError)
    if (audioPlaySupported) {
      audio = new AudioPlayer(1, audioMuted)
    }
  }

  async function startSharing() {
    const stream = pendingStream
    pendingStream = null
    if (!stream) {
      return
    }
    const sendBinary = packet => {
      if (ws?.readyState == WebSocket.OPEN) {
        ws.send(packet)
      }
    }
    const onError = text => { error = text }
    sharer = encodesItself
      ? new Sharer(stream, sendBinary, stopSharing, onError)
      : new RecordingSharer(stream, sendBinary, send, shareAudio && audioShareSupported, stopSharing, onError)
    if (!await sharer.start()) {
      stopSharing()
      return
    }
    sharing = true
    send({ t: 'shareHere' })
    // the recording carries its audio itself
    const track = stream.getAudioTracks()[0]
    if (encodesItself && track && audioShareSupported) {
      audioSharer = new AudioSharer(
        track,
        packet => {
          if (ws?.readyState == WebSocket.OPEN) {
            ws.send(packet)
          }
        },
        text => { error = text }
      )
      if (!await audioSharer.start()) {
        audioSharer = null
      }
    }
  }

  // stops sending, the channel and the screen watched stay
  function stopSharing() {
    sharer?.stop()
    sharer = null
    audioSharer?.stop()
    audioSharer = null
    if (sharing) {
      send({ t: 'stopShare' })
    }
    sharing = false
  }

  function stopMedia() {
    stopViewer()
    stopSharing()
    streamers = []
    watched = null
  }

  // the server can't leave a channel yet, a new session does the same thing
  function freshSession() {
    stopMedia()
    current = null
    connect()
  }

  // a locked channel asks for its password first, its host gets in without
  function join(channel, password = '') {
    error = ''
    const info = channels.find(c => c.id == channel)
    if (info?.locked && !password && info.hostSessionId != sessionId) {
      lockPrompt = { channel, text: '', message: '' }
      return
    }
    lockPrompt = null
    if (current) {
      pendingJoin = { channel, password }
      freshSession()
    }
    else {
      send({ t: 'join', channel, password })
    }
  }

  function submitChannelPassword(event) {
    event.preventDefault()
    if (lockPrompt?.text) {
      join(lockPrompt.channel, lockPrompt.text)
    }
  }

  // the host's channel: set a password, or remove it with an empty one
  function setChannelPassword(event, remove = false) {
    event?.preventDefault()
    if (!isHost) {
      return
    }
    send({ t: 'setPassword', channel: current.channel, password: remove ? '' : newChannelPassword })
    passwordNote = remove ? 'Password removed' : newChannelPassword ? 'Password set' : ''
    newChannelPassword = ''
  }

  // everyone in it but the host. A channel stays after its host leaves, then all in it are watching
  function watching(channel) {
    const hostHere = channel.hostSessionId != 0xffffffff
    return Math.max(channel.users - (hostHere ? 1 : 0), 0)
  }

  function leave() {
    error = ''
    freshSession()
  }

  async function share(camera) {
    error = ''
    try {
      pendingStream = camera
        ? await navigator.mediaDevices.getUserMedia({
          video: { width: { ideal: 1280 }, height: { ideal: 720 }, frameRate: { ideal: 30 } },
          audio: shareAudio && audioShareSupported
        })
        : await navigator.mediaDevices.getDisplayMedia({
          video: { frameRate: 60 },
          audio: shareAudio && audioShareSupported,
          systemAudio: 'include'
        })
    }
    catch {
      return
    }
    // in a channel: share into it and keep watching. Else a channel of its own is made for it
    if (current) {
      startSharing()
    }
    else {
      send({ t: 'share' })
    }
  }

  function toggleShareAudio() {
    saveSetting('ecps.shareAudio', shareAudio)
  }

  function toggleMute() {
    audioMuted = !audioMuted
    saveSetting('ecps.audioMuted', audioMuted)
    audio?.setVolume(1, audioMuted)
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

  // ---- file transfer ----

  const others = $derived(users.filter(user => user.sessionId != sessionId))

  function userName(peer) {
    for (const list of Object.values(usersByChannel)) {
      const user = list.find(u => u.sessionId == peer)
      if (user) {
        return user.name
      }
    }
    return `User ${peer}`
  }

  function percent(item) {
    return item.size ? `${Math.min(100, Math.floor(item.transferred * 100 / item.size))}%` : ''
  }

  function pickFile(peer) {
    sendTarget = peer
    fileInput.value = ''
    fileInput.click()
  }

  function onFilesPicked() {
    for (const file of fileInput.files) {
      files.sendFile(file, sendTarget)
    }
  }

  function sendTo(list, peer) {
    for (const file of list) {
      files.sendFile(file, peer)
    }
  }

  function hasFiles(e) {
    return e.dataTransfer?.types?.includes('Files')
  }

  function onDragOver(e) {
    if (hasFiles(e)) {
      e.preventDefault()
      dragging = true
    }
  }

  function onDragLeave(e) {
    if (!e.relatedTarget) {
      dragging = false
    }
  }

  function onDrop(e) {
    if (!hasFiles(e)) {
      return
    }
    e.preventDefault()
    dragging = false
    // folders can't be sent, only the files that were dropped
    const dropped = []
    for (const item of e.dataTransfer.items) {
      const entry = item.webkitGetAsEntry?.()
      if (item.kind == 'file' && (!entry || entry.isFile)) {
        const file = item.getAsFile()
        if (file) {
          dropped.push(file)
        }
      }
    }
    if (!dropped.length) {
      return
    }
    error = ''
    if (!current) {
      error = 'Join or start a channel first, then drop the files to send them'
    }
    else if (others.length == 0) {
      error = 'There is nobody else in this channel to send to'
    }
    else if (others.length == 1) {
      sendTo(dropped, others[0].sessionId)
    }
    else {
      dropPending = dropped
    }
  }

  function dropSend(peer) {
    sendTo(dropPending, peer)
    dropPending = null
  }

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

<input type="file" multiple class="hidden" bind:this={fileInput} onchange={onFilesPicked}>

<div class="app" class:dragging ondragover={onDragOver} ondragleave={onDragLeave} ondrop={onDrop} role="presentation">
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
                <span>
                  {#if channel.locked}<span class="lock" title="Has a password" aria-label="Has a password">&#128274;</span>{/if}
                  {channel.name}{channel.hostSessionId == sessionId ? ' (you)' : ''}
                </span>
                <span class="muted">{watching(channel)} watching</span>
              </button>
              {#if lockPrompt?.channel == channel.id}
                <form class="lock-form" onsubmit={submitChannelPassword}>
                  {#if lockPrompt.message}<span class="muted">{lockPrompt.message}</span>{/if}
                  <!-- svelte-ignore a11y_autofocus -->
                  <input type="password" placeholder="Channel password" autocomplete="off" autofocus
                    aria-label="Channel password" bind:value={lockPrompt.text}>
                  <div class="file-actions">
                    <button class="primary small" type="submit" disabled={!lockPrompt.text}>Join</button>
                    <button class="small" type="button" onclick={() => { lockPrompt = null }}>Cancel</button>
                  </div>
                </form>
              {/if}
            </li>
          {/each}
        </ul>
      </section>

      {#if isHost}
        <section>
          <h2>Channel password</h2>
          <form onsubmit={e => setChannelPassword(e)}>
            <input type="password" autocomplete="new-password" aria-label="New channel password"
              placeholder={currentLocked ? 'New password' : 'No password, anyone can join'} bind:value={newChannelPassword}>
            <div class="file-actions">
              <button class="primary small" type="submit" disabled={!newChannelPassword}>Set</button>
              {#if currentLocked}
                <button class="small" type="button" onclick={e => setChannelPassword(e, true)}>Remove</button>
              {/if}
            </div>
          </form>
          {#if passwordNote}<p class="muted hint">{passwordNote}</p>{/if}
        </section>
      {/if}

      {#if current && streamers.some(id => id != sessionId)}
        <section>
          <h2>Watching</h2>
          <ul>
            {#each streamers.filter(id => id != sessionId) as id (id)}
              <li class:active={watched == id}>
                <button class="row" disabled={!viewSupported} onclick={() => watchSource(id)}>
                  <span>{userName(id)}</span>
                  <span class="muted">{watched == id ? 'showing' : 'sharing'}</span>
                </button>
              </li>
            {/each}
          </ul>
        </section>
      {/if}

      {#if current}
        <section>
          <h2>Users</h2>
          <ul>
            {#each users as user (user.sessionId)}
              <li class="user">
                <span>
                  {user.name}{user.sessionId == sessionId ? ' (you)' : ''}
                  {#if user.isHost}<span class="muted">host</span>{/if}
                </span>
                {#if user.sessionId != sessionId}
                  <button class="small" onclick={() => pickFile(user.sessionId)}>Send file</button>
                {/if}
              </li>
            {/each}
          </ul>
        </section>
      {/if}

      {#if dropPending}
        <section>
          <h2>Send {dropPending.length == 1 ? dropPending[0].name : `${dropPending.length} files`} to</h2>
          <ul>
            {#each others as user (user.sessionId)}
              <li><button class="row" onclick={() => dropSend(user.sessionId)}>{user.name}</button></li>
            {/each}
          </ul>
          <button onclick={() => { dropPending = null }}>Cancel</button>
        </section>
      {/if}

      {#if transfers.length}
        <section>
          <h2>Files</h2>
          <ul>
            {#each transfers as item (item.handle)}
              <li class="file">
                <div class="file-head">
                  <span class="name">{item.name}</span>
                  <span class="muted">{formatBytes(item.size)}</span>
                </div>
                <div class="muted">{item.outgoing ? 'to' : 'from'} {userName(item.peer)}</div>
                {#if item.state == 'transferring'}
                  <progress max={item.size} value={item.transferred}></progress>
                  <div class="muted">{percent(item)} {formatBytes(item.transferred)}</div>
                {:else}
                  <div class="muted">{item.status}</div>
                {/if}
                {#if item.code}
                  <div class="muted">Code {item.code}</div>
                {/if}
                {#if !item.outgoing && item.state == 'offered'}
                  <div class="file-actions">
                    <button class="primary small" onclick={() => files.accept(item.handle)}>Accept</button>
                    <button class="small" onclick={() => files.decline(item.handle)}>Decline</button>
                  </div>
                {:else if !isFinished(item)}
                  <div class="file-actions">
                    <button class="small" onclick={() => files.cancel(item.handle)}>Cancel</button>
                  </div>
                {/if}
              </li>
            {/each}
          </ul>
          <p class="muted hint">
            End-to-end encrypted. If the code matches on both sides, nobody in between could read or change the file.
            {#if !canStreamToDisk}
              This browser keeps received files in memory (up to {formatBytes(memoryLimit)}). Use Chrome or Edge for bigger ones.
            {/if}
          </p>
          {#if transfers.some(isFinished)}
            <button class="small" onclick={() => files.clearFinished()}>Clear finished</button>
          {/if}
        </section>
      {/if}

      <div class="actions">
        {#if (shareSupported || cameraSupported) && audioShareSupported}
          <label class="check">
            <input type="checkbox" bind:checked={shareAudio} onchange={toggleShareAudio} disabled={sharing}>
            Share audio
          </label>
        {/if}
        {#if shareSupported}
          <button class="primary" disabled={status != 'online' || sharing} onclick={() => share(false)}>
            Share screen
          </button>
        {/if}
        {#if cameraSupported}
          <button disabled={status != 'online' || sharing} onclick={() => share(true)}>
            Share camera
          </button>
        {/if}
        {#if sharing}
          <button onclick={stopSharing}>Stop sharing</button>
        {/if}
        {#if current}
          <button onclick={leave}>Leave</button>
        {/if}
      </div>
    {/if}

    {#if !viewSupported}
      <p class="error">This browser can't play H.264 streams here. Open the https:// link, or try Chrome.</p>
    {/if}
    {#if !shareSupported && !cameraSupported && !needsPassword && status == 'online'}
      <p class="muted hint">
        {#if !window.isSecureContext}
          Sharing needs the https:// link.
        {:else if !encodesItself && (canRecordScreen() || canRecordCamera())}
          Sharing from this browser needs ffmpeg on the computer that runs the web gateway (on its PATH, or named by
          FFMPEG). Chrome and Edge share without it.
        {:else}
          This browser can't share video, it can only watch. Use Chrome or Edge, on a phone Chrome (Android) or Safari
          (iPhone).
        {/if}
      </p>
    {/if}
    {#if error}
      <p class="error">{error}</p>
    {/if}
  </aside>

  <main class="stage" bind:this={stage}>
    <div
      class="view"
      class:hidden={watched == null}
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
    {#if watched != null}
      <div class="controls">
        <button aria-label="Zoom out" disabled={zoom <= 1} onclick={() => zoomAt(zoom / ZOOM_STEP)}>-</button>
        <button aria-label="Reset zoom" disabled={zoom == 1} onclick={resetZoom}>{Math.round(zoom * 100)}%</button>
        <button aria-label="Zoom in" disabled={zoom >= MAX_ZOOM} onclick={() => zoomAt(zoom * ZOOM_STEP)}>+</button>
        {#if audioPlaySupported}
          <button aria-pressed={!audioMuted} onclick={toggleMute}>{audioMuted ? 'Unmute' : 'Mute'}</button>
        {/if}
        <button onclick={fullscreen}>Fullscreen</button>
      </div>
    {:else if sharing}
      <p class="muted">You are sharing your screen. Nobody else here shares.</p>
    {:else if current}
      <p class="muted">Nobody shares here yet. Share your screen, or wait for someone.</p>
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
  .check {
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
  }
  .check input {
    width: auto;
    min-height: 0;
    margin: 0;
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
  .lock {
    margin-right: 4px;
    font-size: 0.85em;
  }
  .lock-form {
    margin-top: 6px;
  }
  .user {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
    padding: 4px 0;
  }
  .small {
    min-height: 32px;
    padding: 4px 10px;
  }
  .file {
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding: 8px;
    border: 1px solid var(--line);
    border-radius: 6px;
  }
  .file-head {
    display: flex;
    justify-content: space-between;
    gap: 8px;
  }
  .name {
    overflow-wrap: anywhere;
  }
  .file-actions {
    display: flex;
    gap: 8px;
  }
  .hint {
    margin: 8px 0;
    font-size: 12px;
  }
  progress {
    width: 100%;
  }
  .app.dragging .stage {
    outline: 3px dashed var(--accent);
    outline-offset: -6px;
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

# ECPS web client

Watch streams from a phone or PC browser, and share a screen from a PC browser.
Browsers can't speak raw TCP/UDP, so a small gateway sits next to the server:

```
browser <-- HTTPS/WebSocket --> gateway <-- ECPS TCP/UDP --> ecps server
```

The gateway decrypts video to pass it on, so run it on a machine you trust.

## Setup

```bash
npm install
npm run build
```

## Run

```bash
ECPS_HOST=127.0.0.1 ECPS_PORT=43254 WEB_PASSWORD=choose-one npm run gateway
```

On Windows cmd use `set NAME=value` first, in PowerShell `$env:NAME = 'value'`.

| Variable | Default | Meaning |
|---|---|---|
| `ECPS_HOST` | `127.0.0.1` | ecps server address |
| `ECPS_PORT` | `43254` | ecps server port |
| `WEB_HOST` | `127.0.0.1` | address the page listens on |
| `WEB_PORT` | `8080` | port the page listens on |
| `WEB_PASSWORD` | empty | password for the page, empty means no password |

## HTTPS for phones

Browsers only allow video decoding on HTTPS pages or `localhost`. A free tunnel
gives a public HTTPS link without opening ports:

```bash
winget install Cloudflare.cloudflared
cloudflared tunnel --url http://localhost:8080
```

## Limits

- Hosts must stream H.264. HEVC and AV1 streams won't play.
- Phone browsers can't capture the screen, so sharing is PC only.
- There is no remote mouse or keyboard control.
- Audio needs WebCodecs: playing works in Chrome, Edge and Safari, sharing in Chrome and Edge. Chrome shares
  system audio on Windows and ChromeOS, elsewhere only a tab's audio. Tick "Share audio" in the picker too.
- Video goes over TCP to the browser, so it has more delay than the desktop app.
- Browsers without WebCodecs, such as Firefox on Android, use a slower MP4 fallback. Add `?mse` to the link to force it elsewhere.

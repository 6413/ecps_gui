every communication program sucks

code is made for easily flexible communication program

structural design of prot.h made by github.com/6413
client uses github.com/6413/fan for window related things

Supports changing video encoder and decoder in runtime.
Built in encoders and decoders:
  - OpenH264
  - X264
  - NVENC, NVCUVID

![image](https://github.com/user-attachments/assets/b5268106-993f-4d6a-a611-9c7bed6cc685)
![image](https://github.com/user-attachments/assets/d0f2ce5d-b866-4474-b4bc-d870fedc3a08)

## Audio

The host can share what the computer is playing ("Share audio" next to the stream button or in
Stream Settings), viewers turn it on and off with "Play audio". Audio is Opus, 48 kHz stereo in
20 ms packets, sent over UDP next to the video.

- Windows: WASAPI loopback of the default output device.
- Linux: the monitor of the default PulseAudio/PipeWire sink.

## Remote control

The host picks "Mouse + Keyboard" under Input Control, viewers then click the stream to control it.
Control turns off when the stream stops.

- Windows: SendInput. Windows doesn't let it reach programs running as administrator.
- Linux X11: XTest (libXtst).
- Linux Wayland: a virtual mouse and keyboard through `/dev/uinput`, which needs write access, e.g.
  `sudo setfacl -m u:$USER:rw /dev/uinput` or a udev rule. On several monitors the compositor
  decides how the virtual mouse maps to them.

## Web version

Browsers connect through a small gateway that runs next to the ecps server.
It needs Node.js.

1. Start the ecps server.
2. Build the page once, and again after changing the web code:

```bash
cd web
npm install
npm run build
```

3. Start the gateway from the `web` folder:

```bash
ECPS_HOST=127.0.0.1 ECPS_PORT=43254 WEB_PASSWORD=choose-one npm run gateway
```

In PowerShell:

```powershell
$env:ECPS_HOST = '127.0.0.1'
$env:ECPS_PORT = '43254'
$env:WEB_PASSWORD = 'choose-one'
npm run gateway
```

4. Open http://localhost:8080 on the same machine.

`ECPS_HOST` and `ECPS_PORT` are the ecps server's address. An empty `WEB_PASSWORD`
means no password. Phones and other devices need an HTTPS link, see
[web/README.md](web/README.md) for that and for all the settings.

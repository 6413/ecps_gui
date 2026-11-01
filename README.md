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

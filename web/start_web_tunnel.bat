@echo off
rem the ecps server has to be running already
cd /d "%~dp0"

rem empty means no password on the page
set WEB_PASSWORD=
set ECPS_HOST=127.0.0.1
set ECPS_PORT=43254
set WEB_PORT=8080

if not exist node_modules call npm install
if not exist dist\index.html call npm run build

start "ECPS gateway" cmd /k npm run gateway

rem phones need https, the tunnel window prints a https://...trycloudflare.com link
timeout /t 2 /nobreak >nul
where cloudflared >nul 2>nul || (
  echo cloudflared is missing, install it with: winget install Cloudflare.cloudflared
  pause
  exit /b 1
)
start "ECPS tunnel" cmd /k cloudflared tunnel --url http://localhost:%WEB_PORT%

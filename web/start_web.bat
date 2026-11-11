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

rem give the gateway a moment to start listening
timeout /t 2 /nobreak >nul
start "" http://localhost:%WEB_PORT%

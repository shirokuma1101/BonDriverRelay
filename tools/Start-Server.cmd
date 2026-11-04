@echo off
setlocal
"%~dp0BonDriverRelayServer.exe" --serve --config "%~dp0server.toml"
if errorlevel 1 pause

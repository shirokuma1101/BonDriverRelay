@echo off
setlocal
"%~dp0BonDriverRelayServer.exe" --validate-config --config "%~dp0server.toml"
pause

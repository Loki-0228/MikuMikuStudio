@echo off
setlocal
set "MMS_CACHE_MEMORY_ONLY=1"
start "" "%~dp0..\..\MikuMikuDance.exe" %*

@echo off
REM Test launch for fivem2open77 cars on the VM XBUNIVERSE server (192.168.0.213).
REM  1. Brings the game folder in line with the server's required mods. A direct boot skips the
REM     launcher's mod install, so without this a re-converted car would still load the old files.
REM     Hash-checked, cached, only changed files are written; see tools\sync_required_mods.py.
REM  2. Boots the game directly (no EAC bootstrapper: TweakXL is not in the EAC integrity catalog)
REM     through Launch-Open77-XBUNIVERSE-Local.bat, which starts the VM server and the relays.
setlocal
set "GAMEDIR=C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077"
set "SYNC=%USERPROFILE%\source\repos\fivem2open77\tools\sync_required_mods.py"
set "MANIFEST=http://192.168.0.213:11822/mods/v1/manifest"
set "PY=python.exe"
if exist "%LOCALAPPDATA%\Programs\Python\Python313\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python313\python.exe"

tasklist /FI "IMAGENAME eq Cyberpunk2077.exe" 2>nul | find /I "Cyberpunk2077.exe" >nul
if errorlevel 1 goto :sync
echo Cyberpunk 2077 is already running. Close it first so the car files can be updated.
pause
exit /b 1

:sync
if not exist "%SYNC%" goto :nosync
echo Syncing the VM server's required mods into the game folder ...
"%PY%" "%SYNC%" --manifest "%MANIFEST%" --game "%GAMEDIR%" --allow-executable
if not errorlevel 1 goto :launch
echo.
echo ERROR: could not bring the game folder in line with the server's mods (see above).
echo        Is the VM server up? Check http://192.168.0.213:11822/health
pause
exit /b 1

:nosync
echo WARNING: %SYNC% not found - skipping the mod sync.

:launch
endlocal
set "OP77_DIRECT=1"
call "%~dp0Launch-Open77-XBUNIVERSE-Local.bat"

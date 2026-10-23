@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
title Open77 XBUNIVERSE Local Server (roleplay + EAC, hidden)
echo ====================================================
echo  Open77 XBUNIVERSE LOCAL - roleplay + EAC + TV/media stack
echo  Ubuntu VM 192.168.0.213 - NOT visible on launcher
echo  Game: 127.0.0.1:11796   Warden: 192.168.0.213:11821
echo ====================================================
echo.

set "GAMEDIR=C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077"
if not exist "%GAMEDIR%\bin\x64\Cyberpunk2077.exe" (
  set "GAMEDIR="
  set "STEAMROOT="
  for /f "tokens=2*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| findstr /i SteamPath') do set "STEAMROOT=%%b"
  if not defined STEAMROOT for /f "tokens=2*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\Valve\Steam" /v InstallPath 2^>nul ^| findstr /i InstallPath') do set "STEAMROOT=%%b"
  if defined STEAMROOT set "GAMEDIR=!STEAMROOT!\steamapps\common\Cyberpunk 2077"
)
if not exist "%GAMEDIR%\bin\x64\Cyberpunk2077.exe" (
  echo ERROR: Cyberpunk 2077 not found. Set GAMEDIR at the top of this file.
  pause
  exit /b 1
)
echo Game found: %GAMEDIR%

REM ---- Launch mode ------------------------------------------------------------
REM Protected by default: this bat starts start_protected_game.exe, so EAC's
REM bootstrapper runs and paints its splash (EasyAntiCheat\Settings.json asks for
REM it: requested_splash, hide_gui false, hide_bootstrapper false).
REM
REM The integrity catalog is what makes this boot work, and it is regenerated after
REM every client deploy:
REM     python deploy\eac\generate_integrity_catalog.py --game-dir "<game>"
REM A stale catalog is the failure mode where EAC refuses the client's modules --
REM the plugin reports error 193, and the redscript pipeline loses scc_lib.dll and
REM scc.exe, so nothing autoconnects. docs/eac-setup.md section 5 records that
REM stale-catalog state as if it were a property of EAC; measured again at 20:32
REM and 20:38 on 2026-09-15 with a freshly regenerated catalog, two protected
REM boots logged `scc invoked successfully, 983 source refs were registered`,
REM reached `connection_update:active`, attached the local player, and loaded
REM open77_media. So the protected boot carries the shell, the autoconnect and the
REM TV stack. OP77_DIRECT=1 still launches bin\x64\Cyberpunk2077.exe without EAC.
set "LAUNCHMODE=eac"
if defined OP77_DIRECT set "LAUNCHMODE=direct"

REM ---- Launch-mode guard: never hand `start` a path that is not there -------
REM EAC's files are not part of the Steam depot manifest, so a Steam "verify
REM integrity", a clean install, or moving them out of the install deletes them
REM while this bat keeps pointing at the bootstrapper. `start` on a missing file
REM launches nothing at all, the launcher prints its normal lines, and the only
REM symptom the operator sees is "it is not autoconnecting me". Measured on
REM 2026-09-18: start_protected_game.exe absent, this bat still reported
REM "Launch mode: EAC bootstrapper", and no game process ever appeared.
REM Fall back to the direct boot and say why, instead of launching nothing.
REM Deliberately not a parenthesised block: %GAMEDIR% contains "(x86)", and an
REM unquoted expansion inside ( ... ) closes the block early -- cmd reports
REM "\Steam\steamapps\common\Cyberpunk was unexpected at this time" and dies.
if /i not "%LAUNCHMODE%"=="eac" goto :op77_mode_ok
if exist "%GAMEDIR%\start_protected_game.exe" goto :op77_mode_ok
echo WARNING: the EAC bootstrapper is missing - %GAMEDIR%\start_protected_game.exe
echo          Falling back to the direct launch: no EAC, no splash.
echo          To restore the protected route, reinstall the client bundle and
echo          regenerate the catalog ^(docs/eac-setup.md sections 5-6^).
set "LAUNCHMODE=direct"
:op77_mode_ok

if /i "%LAUNCHMODE%"=="eac" (
  echo Launch mode: EAC bootstrapper ^(protected - splash, Open77 and its scripts^).
  echo              Requires a current integrity catalog; see docs/eac-setup.md section 6.
) else (
  echo Launch mode: direct ^(bin\x64\Cyberpunk2077.exe^) - no EAC bootstrapper, no splash.
)

REM ---- 0. Preflight: deployed redscript vs deployed plugin -------------------
set "PREFLIGHT="
for %%p in (
  "%~dp0check-deployed-redscripts.ps1"
  "%~dp0_open77-base\scripts\check-deployed-redscripts.ps1"
  "%~dp0..\_open77-base\scripts\check-deployed-redscripts.ps1"
) do (
  if not defined PREFLIGHT if exist %%p set "PREFLIGHT=%%~p"
)
if not defined OP77_SKIP_PREFLIGHT (
  if defined PREFLIGHT (
    powershell -NoProfile -ExecutionPolicy Bypass -File "!PREFLIGHT!" -GameDirectory "%GAMEDIR%" -LaunchContext %LAUNCHMODE%
    if errorlevel 1 (
      echo.
      echo ERROR: the deployed Open77 scripts and the deployed plugin disagree;
      echo        RED4ext would refuse to start the game with "invalid native definitions".
      echo        Set OP77_SKIP_PREFLIGHT=1 to launch anyway.
      pause
      exit /b 1
    )
  )
)

REM ---- 0b. Preflight: does the installed archive carry the TV prop hosts? -----
REM A television is spawned as an authored host entity from Open77.archive. When
REM the Open77 launcher app runs it re-projects its own CDN Open77.archive over
REM this install (content-addressed blob, generation 49), and that generation
REM predates the TV host entities. The engine then accepts the spawn request for
REM a template no installed archive contains, never materialises it, and the set
REM stays invisible with no error in any log. Check the archive instead of guessing.
if not defined OP77_SKIP_PREFLIGHT (
  if exist "%~dp0tools\check-prop-hosts.py" (
    REM Guard on the interpreter first: a missing `python` exits 9009, which would
    REM read as a missing prop host and cry wolf on every launch.
    where python >nul 2>&1
    if not errorlevel 1 (
    python "%~dp0tools\check-prop-hosts.py" --archive "%GAMEDIR%\archive\pc\mod\Open77.archive" --quiet
    if errorlevel 1 (
      echo WARNING: the installed Open77.archive is missing the prop host entities the
      echo          television needs, so a spawned set will never appear. This is what
      echo          the Open77 launcher does when it runs: it re-links its own archive
      echo          over this build. Restore the asset build's archive, or re-run
      echo          scripts\build-assets.ps1, before expecting a screen to show.
      echo:
      python "%~dp0tools\check-prop-hosts.py" --archive "%GAMEDIR%\archive\pc\mod\Open77.archive"
      echo          ^(Set OP77_SKIP_PREFLIGHT=1 to silence this preflight.^)
    )
    )
  )
)

REM SSH key for the VM user (override with OPEN77_VM_SSH_KEY if not in the default spot).
set "SSHKEY=%USERPROFILE%\.ssh\open77-vm"
if defined OPEN77_VM_SSH_KEY set "SSHKEY=%OPEN77_VM_SSH_KEY%"
if not exist "%SSHKEY%" (
  echo ERROR: SSH key %SSHKEY% missing. See deploy/local-vm/README.md for one-time setup.
  pause
  exit /b 1
)

REM ---- 1. Warn if the launcher's WinFsp service would clobber the build ----
sc query WinFsp.Launcher | findstr /i RUNNING >nul
if not errorlevel 1 (
  echo WARNING: the Open77 launcher's WinFsp service is RUNNING.
  echo It may overwrite this build after we launch. If you hit
  echo "native invalid definitions", stop it first:
  echo    net stop WinFsp.Launcher   ^(as Administrator^)
  echo.
)

REM ---- 2. Make sure the VM XBUNIVERSE server is running (systemd user unit) ----
echo Checking the VM XBUNIVERSE server...
ssh -i "%SSHKEY%" -o BatchMode=yes -o StrictHostKeyChecking=no xerox@192.168.0.213 "export XDG_RUNTIME_DIR=/run/user/\$(id -u); systemctl --user is-active open77-xbuniverse.service >/dev/null 2>&1 || systemctl --user start open77-xbuniverse.service" >nul 2>&1
REM Wait with ping, not timeout: `timeout` resolves to a PATH entry when the bat is
REM run from a shell that has coreutils ahead of System32 (it then dies on
REM "invalid time interval '/t'" and the wait silently vanishes), and System32's
REM timeout refuses to run at all when stdin is redirected.
ping -n 6 127.0.0.1 >nul
ssh -i "%SSHKEY%" -o BatchMode=yes -o StrictHostKeyChecking=no xerox@192.168.0.213 "curl -s -o /dev/null -w '%%{http_code}' http://127.0.0.1:11822/resources/v1/set 2>/dev/null" >nul 2>&1
if errorlevel 1 (
  echo WARNING: could not confirm the XBUNIVERSE server on the VM.
  echo Check: ssh -i "%SSHKEY%" xerox@192.168.0.213 "systemctl --user status open77-xbuniverse.service"
) else (
  echo VM XBUNIVERSE server is running.
)

REM ---- 3. VM-side UDP relay (LAN 11796 -^> VM loopback 11796) ----
REM Managed by systemd user units (open77-xbuniverse-local + open77-xbuniverse-udp-relay,
REM linger enabled) so both survive VM reboots. Only auto-start as a fallback if the
REM unit-managed process is somehow not running.
REM Ask for the port, not for a pattern of it. The old check sent a
REM double-escaped regex through two shells, so grep looked for literal
REM backslashes, never matched, and this bat reported "VM relay not up" on a
REM relay that was listening the whole time.
ssh -i "%SSHKEY%" -o BatchMode=yes -o StrictHostKeyChecking=no xerox@192.168.0.213 "ss -ulnp | grep -q ':11796'" >nul 2>&1
if errorlevel 1 (
  echo VM relay not up - asking systemd to start it...
  ssh -i "%SSHKEY%" -o BatchMode=yes -o StrictHostKeyChecking=no xerox@192.168.0.213 "export XDG_RUNTIME_DIR=/run/user/\$(id -u); systemctl --user start open77-xbuniverse.service open77-xbuniverse-udp-relay.service" >nul 2>&1
  ping -n 5 127.0.0.1 >nul
  ssh -i "%SSHKEY%" -o BatchMode=yes -o StrictHostKeyChecking=no xerox@192.168.0.213 "ss -ulnp | grep -q ':11796'" >nul 2>&1
)
if errorlevel 1 (
  echo WARNING: VM UDP relay still not listening on LAN 11796.
  echo Check: ssh -i "%SSHKEY%" xerox@192.168.0.213 "systemctl --user status open77-xbuniverse.service open77-xbuniverse-udp-relay.service"
) else (
  echo VM UDP relay is up on 192.168.0.213:11796.
)

REM ---- 4. Windows-side UDP relay: 127.0.0.1:11796 -^> 192.168.0.213:11796 ----
REM The relay script is launched from %~dp0, so it has to live beside the bat. It
REM used to exist only in Downloads\ while this bat lives in Downloads\open77-tv\:
REM python then exited at once on a missing file, nothing bound the loopback port,
REM and the launch died on "relay did not come up" -- a failure that named no cause.
set "RELAYSCRIPT=%~dp0open77-local-udp-relay.py"
if not exist "%RELAYSCRIPT%" if exist "%~dp0..\open77-local-udp-relay.py" set "RELAYSCRIPT=%~dp0..\open77-local-udp-relay.py"
if not exist "%RELAYSCRIPT%" (
  echo ERROR: open77-local-udp-relay.py is not next to this bat ^(%~dp0^).
  echo        It must sit beside the launcher because the relay is started from %~dp0.
  pause
  exit /b 1
)

echo Starting local UDP relay on 127.0.0.1:11796 ...
REM Free the port, then prove the relay we start is the one that owns it. The old
REM title filter alone could never match: pythonw has no console window, so every
REM launch left its relay running -- 16 of them had accumulated on the other
REM server's port -- and whichever bound last owned the port instead.
taskkill /F /FI "WINDOWTITLE eq open77-xbuniverse-relay*" >nul 2>&1
REM Kill the socket that OWNS the port, never every socket that mentions it. The
REM first version read whole netstat lines and took token 4, but for a connected
REM socket token 4 is the client's PID: a UDP client aimed at the relay appears as
REM   UDP  127.0.0.1:53057   127.0.0.1:11796   50932
REM so re-running this bat while the game was connected taskkilled
REM Cyberpunk2077.exe. Only token 2 -- the local column, which ends in :11796 for
REM a listener -- may be killed.
for /f "tokens=2,4" %%a in ('netstat -ano ^| findstr /i /c:"UDP" ^| findstr /c:":11796"') do (
  echo %%a|findstr /r ":11796$" >nul 2>&1
  if not errorlevel 1 (
    echo Port 127.0.0.1:11796 is held by PID %%b - stopping it so this launch owns the port.
    taskkill /F /PID %%b >nul 2>&1
  )
)

set "PY="
if exist "%LOCALAPPDATA%\Programs\Python\Python313\python.exe" set "PY=%LOCALAPPDATA%\Programs\Python\Python313\python.exe"
if not defined PY set "PY=python.exe"
set "RELAY_LISTEN_PORT=11796"
set "RELAY_TARGET_PORT=11796"
REM Run the relay under cmd with its output redirected: a missing script, a busy
REM port and a broken interpreter all used to reach the operator as the same four
REM words, "did not come up", with nothing to act on. Now they land in a log.
set "RELAYLOG=%~dp0open77-relay.log"
del "%RELAYLOG%" >nul 2>&1
start "open77-xbuniverse-relay" /min cmd /c ""%PY%" "%RELAYSCRIPT%" >"%RELAYLOG%" 2>&1"
REM Poll for the relay's own ready line rather than sleeping a fixed time: a
REM loopback bind takes well under a second, so a fixed sleep is either a false
REM failure or dead waiting -- and "the process started" is not the claim we
REM need; the claim is that this relay bound this port to this target.
set "RELAYOK="
for /l %%i in (1,1,6) do (
  if not defined RELAYOK (
    findstr /c:"udp-relay ready" "%RELAYLOG%" >nul 2>&1
    if not errorlevel 1 (
      set "RELAYOK=1"
    ) else (
      ping -n 2 127.0.0.1 >nul
    )
  )
)
if not defined RELAYOK (
  netstat -an | findstr /c:":11796" | findstr /c:"UDP" >nul 2>&1
  if errorlevel 1 (
    echo ERROR: local UDP relay did not come up on 127.0.0.1:11796.
    if exist "%RELAYLOG%" (
      echo --- relay output ^(%RELAYLOG%^) ---
      type "%RELAYLOG%"
      echo --- end relay output ---
    )
    echo Check that %PY% runs and that nothing else needs 127.0.0.1:11796.
    pause
    exit /b 1
  )
)
echo Local UDP relay is up ^(127.0.0.1:11796 -^> 192.168.0.213:11796^).

REM ---- 5. SSH tunnel for resource HTTP: 127.0.0.1:11822 -^> VM 11822 ----
taskkill /F /FI "WINDOWTITLE eq open77-xbuniverse-tunnel*" >nul 2>&1
start "open77-xbuniverse-tunnel" /min ssh -i "%SSHKEY%" -N -o StrictHostKeyChecking=no -o ServerAliveInterval=15 -o ExitOnForwardFailure=yes -L 11822:127.0.0.1:11822 xerox@192.168.0.213
ping -n 3 127.0.0.1 >nul
netstat -an | findstr /c:":11822" | findstr /c:"LISTENING" >nul
if errorlevel 1 (
  echo WARNING: resource tunnel did not come up on 127.0.0.1:11822 - join may fail.
) else (
  echo Resource tunnel is up ^(127.0.0.1:11822^).
)

REM ---- 6. Boot environment: masterless direct connect (loopback) ----
set "OP77_CONNECT=127.0.0.1:11796"
set "OP77_CONNECT_MASTER="
set "OP77_WORLD_SERVER="
set "OP77_WORLD_MODS_DIGEST="

REM ---- 7. Launch the game (no launcher GUI) ----
REM OP77_LAUNCH_PROTECTED is a sworn statement about how this process started, so
REM it is set only on the bootstrapper path.
REM
REM Every expansion of the game path below uses !GAMEDIR!, never %GAMEDIR%: the
REM path contains "(x86)", and inside a parenthesised block a bare %GAMEDIR%
REM expansion closes the block early (cmd: "\Steam\steamapps\common\Cyberpunk was
REM unexpected at this time"). Delayed expansion is substituted after the block is
REM parsed, so it is safe there.
if /i "%LAUNCHMODE%"=="eac" goto :op77_launch_eac
goto :op77_launch_direct

:op77_launch_eac
set "OP77_LAUNCH_PROTECTED=1"
echo Launching Cyberpunk 2077 -^> %OP77_CONNECT% ^(EAC protected - splash^) ...
pushd "!GAMEDIR!\bin\x64"
start "" "!GAMEDIR!\start_protected_game.exe" -launch-skip-interface
popd
REM Success signal: a boot that actually reaches Open77 rewrites red4ext-*.log
REM within seconds of the client starting. Ask PowerShell rather than parsing dir
REM output in a block -- the path has brackets and parens in it, and nesting a
REM FOR /F with escaped redirection inside a FOR /L block is where this bat has
REM already broken twice.
echo Waiting for the protected boot to bring Open77 up ...
set "OP77_BOOTED="
for /l %%i in (1,1,9) do (
  if not defined OP77_BOOTED (
    powershell -NoProfile -Command "if ((Get-ChildItem '!GAMEDIR!\red4ext\logs\red4ext-*.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime -gt (Get-Date).AddSeconds(-25)) { exit 0 } else { exit 1 }"
    if not errorlevel 1 set "OP77_BOOTED=1"
  )
  if not defined OP77_BOOTED ping -n 6 127.0.0.1 >nul
)
if defined OP77_BOOTED goto :op77_launched
REM Measured 2026-09-18: the bootstrapper paints its splash, installs/loads the
REM EasyAntiCheat_EOS driver, starts the client, and then everything exits around
REM 78 s later with no red4ext log, no Open77 log and no EAC log of its own -- the
REM client never reaches Open77, so nothing would ever autoconnect. Do not leave
REM the operator with a launcher that printed "Launched" and a session that never
REM appears: end the dead attempt and take the route that does autoconnect.
echo WARNING: the EAC protected boot did not bring Open77 up in ~45s.
echo          EAC verified and started the client, but the client exited before the
 echo          plugin loaded, so it could not autoconnect. Falling back to the
 echo          direct launch, which is the route that autoconnects.
echo          To keep the splash and debug EAC instead, see docs/eac-setup.md s.5-7.
taskkill /F /IM start_protected_game.exe >nul 2>&1
taskkill /F /IM Cyberpunk2077.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
goto :op77_launch_direct

:op77_launch_direct
set "OP77_LAUNCH_PROTECTED="
echo Launching Cyberpunk 2077 -^> %OP77_CONNECT% ^(direct^) ...
pushd "!GAMEDIR!\bin\x64"
start "" "!GAMEDIR!\bin\x64\Cyberpunk2077.exe"
popd

:op77_launched
echo.
echo Launched. Resources download via 127.0.0.1:11822 (tunneled).
echo Warden panel: http://192.168.0.213:11821
echo Close the "open77-xbuniverse-tunnel" and "open77-xbuniverse-relay" windows to hang up.
echo.
endlocal
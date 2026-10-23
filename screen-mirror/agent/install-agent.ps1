# open77 headless mirror agent - installer for the work PC (Windows).
#
# Copies the agent to %LOCALAPPDATA%\open77-mirror-agent, installs Electron,
# and registers a logon scheduled task so it starts at every logon with NO
# visible browser (a tray icon is the only UI).
#
# Run in PowerShell (no admin needed - per-user logon task):
#   powershell -ExecutionPolicy Bypass -File install-agent.ps1
#
# After it runs, edit %LOCALAPPDATA%\open77-mirror-agent\config.json to set the
# channel + token, then run the task (or log off/on).

$ErrorActionPreference = "Stop"

$src  = Split-Path -Parent $MyInvocation.MyCommand.Path
$dest = Join-Path $env:LOCALAPPDATA "open77-mirror-agent"
$taskName = "open77-mirror-agent"

Write-Host ">> copying agent to $dest"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Path (Join-Path $src "*") -Destination $dest -Force -Exclude "node_modules"

Write-Host ">> installing Electron (this can take a minute)"
Push-Location $dest
try {
  if (Test-Path "package-lock.json") { npm ci }
  else { npm install }
  if ($LASTEXITCODE -ne 0) { throw "npm install failed" }

  # Newer npm blocks postinstall scripts by default, which skips Electron's
  # binary download. Run its installer explicitly if electron.exe is missing.
  if (!(Test-Path "node_modules\electron\dist\electron.exe")) {
    Write-Host ">> Electron binary missing after npm (postinstall blocked) - running its installer"
    node node_modules\electron\install.js
    if (!(Test-Path "node_modules\electron\dist\electron.exe")) {
      # Fall back to unpacking the cached download by hand.
      $zip = Get-ChildItem "$env:LOCALAPPDATA\electron\Cache\electron-v*-win32-x64.zip" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
      if ($zip) {
        Write-Host ">> unpacking cached $($zip.Name)"
        Expand-Archive -Path $zip.FullName -DestinationPath "node_modules\electron\dist" -Force
      }
    }
    if (!(Test-Path "node_modules\electron\dist\electron.exe")) { throw "electron.exe still missing after install attempts" }
  }
} finally { Pop-Location }

if (!(Test-Path (Join-Path $dest "config.json"))) {
  Copy-Item (Join-Path $dest "config.example.json") (Join-Path $dest "config.json")
  Write-Host ">> created config.json from the example - EDIT IT with the channel + token"
}

$electron = Join-Path $dest "node_modules\electron\dist\electron.exe"
if (!(Test-Path $electron)) { throw "electron.exe not found after install: $electron" }

Write-Host ">> registering logon task '$taskName'"
$action    = New-ScheduledTaskAction -Execute $electron -Argument "`"$dest`""
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null

Write-Host ">> starting now"
Start-ScheduledTask -TaskName $taskName

Write-Host ""
Write-Host "Done. The agent now starts at every logon."
Write-Host "  config : $dest\config.json   (set channel + token here)"
Write-Host "  tray   : right-click the tray icon for status / reconnect / quit"
Write-Host "  remove : Unregister-ScheduledTask -TaskName '$taskName' -Confirm:`$false"

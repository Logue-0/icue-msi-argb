# icue-msi-argb installer. Usage:  .\install.ps1            (install / update)
#                                   .\install.ps1 -Uninstall (restore MSI's original DLL)
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  $a = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""; if ($Uninstall) { $a += ' -Uninstall' }
  Start-Process powershell -Verb RunAs -ArgumentList $a -Wait; exit }

$plugin = Join-Path ${env:ProgramFiles} 'Corsair\Corsair iCUE5 Software\plugins\MSI'
$data   = Join-Path $env:ProgramData 'icue-msi-shim'
$sdk    = Join-Path $plugin 'MysticLight_SDK_x64.dll'
$real   = Join-Path $plugin 'MysticLight_SDK_x64_real.dll'
$new    = Join-Path $PSScriptRoot 'bin\MysticLight_SDK_x64.dll'
$logf   = Join-Path $PSScriptRoot 'install.log'
function Say($s) { Write-Host $s; Add-Content -Path $logf -Value ("{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $s) }
function MsiSigned($f) { (Test-Path $f) -and (Get-AuthenticodeSignature $f).Status -eq 'Valid' }

try {
  if (-not (Test-Path (Join-Path $plugin 'CueMsiPlugin.dll'))) { throw "iCUE MSI plugin not found in $plugin. Install iCUE 5 and its MSI plugin first." }

  Say 'Stopping iCUE and its plugin host...'
  Stop-Process -Name iCUE -Force -ErrorAction SilentlyContinue; Start-Sleep 2
  Stop-Service iCUEDevicePluginHost -Force -ErrorAction SilentlyContinue; Start-Sleep 2
  Get-Process iCUEDevicePluginHost -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep 1

  if ($Uninstall) {
    if (MsiSigned $real) { Copy-Item $real $sdk -Force; Remove-Item $real -Force; Say 'Original MSI SDK restored.' }
    elseif (MsiSigned $sdk) { Say 'Nothing to do: the MSI SDK in place is already the original.' }
    else { throw "No original MSI SDK found ($real). Reinstall the iCUE MSI plugin to restore it." }
  } else {
    if (-not (Test-Path $new)) { throw "Missing $new" }
    if (MsiSigned $sdk) { Copy-Item $sdk $real -Force; Say 'Original MSI SDK kept as MysticLight_SDK_x64_real.dll.' }
    elseif (-not (MsiSigned $real)) { throw "Neither $sdk nor $real is the MSI-signed SDK. Reinstall the iCUE MSI plugin, then run this again." }
    Copy-Item $new $sdk -Force; Say 'icue-msi-argb installed.'
    New-Item -ItemType Directory -Force $data | Out-Null
    if (-not (Test-Path (Join-Path $data 'config.txt'))) { Copy-Item (Join-Path $PSScriptRoot 'config.example.txt') (Join-Path $data 'config.txt'); Say "Created $data\config.txt from the example - edit it for your setup." }
    else { Say "Kept existing $data\config.txt." }
  }
  Start-Service iCUEDevicePluginHost -ErrorAction SilentlyContinue
  Say 'Done. Start iCUE.'
} catch { Say "ERROR: $($_.Exception.Message)"; exit 1 }

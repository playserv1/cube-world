# Starts a Cube World client from the editor binaries: signs in as a guest, lists the rooms of the `dev`
# environment (both room types) and enters the first server. Several clients can run side by side.
#
#   .\Scripts\RunClient.ps1 -Name Ann
#   .\Scripts\RunClient.ps1 -Name Bob -Extra "-selftest -screenshot=20 -quitafter=40"
param(
    [string]$Name = "",
    [string]$Extra = "",
    [string]$Engine = ""
)
$ErrorActionPreference = "Stop"
$Project = Resolve-Path (Join-Path $PSScriptRoot "..\CubeWorld.uproject")
$Root = Split-Path $Project
if ($Engine -eq "") {
    # The Launcher records where it installed each engine version.
    $Record = "C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat"
    if (Test-Path $Record) {
        $Install = (Get-Content $Record -Raw | ConvertFrom-Json).InstallationList | Where-Object { $_.AppName -eq "UE_5.8" } | Select-Object -First 1
        if ($Install) { $Engine = $Install.InstallLocation }
    }
    if ($Engine -eq "") { $Engine = "D:\EpicGames\UE_5.8" }
}
$Editor = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $Editor)) { throw "No editor at $Editor; pass -Engine <the folder that holds Engine\>." }
if ($Name -eq "") { $Name = "Player-" + (Get-Random -Minimum 1000 -Maximum 9999) }
$Log = Join-Path $Root "Saved\Logs\client-$Name.log"
$Args = @("`"$Project`"", "-game", "-windowed", "-resx=1280", "-resy=720", "-log", "-autoplay", "-name=$Name", "-abslog=`"$Log`"")
if ($Extra -ne "") { $Args += $Extra.Split(" ") }
Start-Process -FilePath $Editor -ArgumentList $Args -WorkingDirectory $Root | Out-Null
Write-Host "client $Name, log $Log"

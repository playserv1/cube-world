# Starts a Cube World client from the editor binaries: signs in as a guest, lists the rooms of the `ue`
# environment and travels to the first server. Several clients can run side by side.
#
#   .\Scripts\RunClient.ps1 -Name Ann
#   .\Scripts\RunClient.ps1 -Name Bob -Extra "-selftest -screenshot=20 -quitafter=40"
param(
    [string]$Name = "",
    [string]$Extra = "",
    [string]$Engine = "D:\EpicGames\UE_5.8\UE_5.8"
)
$ErrorActionPreference = "Stop"
$Project = Resolve-Path (Join-Path $PSScriptRoot "..\CubeWorld.uproject")
$Root = Split-Path $Project
$Editor = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.exe"
if ($Name -eq "") { $Name = "Player-" + (Get-Random -Minimum 1000 -Maximum 9999) }
$Log = Join-Path $Root "Saved\Logs\client-$Name.log"
$Args = @("`"$Project`"", "-game", "-windowed", "-resx=1280", "-resy=720", "-log", "-autoplay", "-name=$Name", "-abslog=`"$Log`"")
if ($Extra -ne "") { $Args += $Extra.Split(" ") }
Start-Process -FilePath $Editor -ArgumentList $Args -WorkingDirectory $Root | Out-Null
Write-Host "client $Name, log $Log"

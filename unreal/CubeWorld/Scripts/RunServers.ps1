# Starts the three Cube World dedicated servers on this machine, one per region, as the editor run headless
# (the Launcher's engine has no Server target; see Source/CubeWorldServer.Target.cs). Each server claims a free
# region, registers its room with PlayServ (environment `dev`, room type `cubeworld-ue`) under this machine's
# address, and listens on its own UDP port. Logs go to Saved/Logs/server-<name>.log.
#
#   .\Scripts\RunServers.ps1              # alpha:7777, beta:7778, gamma:7779
#   .\Scripts\RunServers.ps1 -Count 1     # one server only
#   .\Scripts\RunServers.ps1 -PublicHost 203.0.113.7   # the address players connect to, when not this machine's own
#
# Needs Config/DedicatedServerGame.ini with the server key (see DedicatedServerGame.example.ini).
param(
    [int]$Count = 3,
    [int]$FirstPort = 7777,
    [string]$PublicHost = "",
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
$Editor = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $Editor)) { throw "No editor at $Editor; pass -Engine <the folder that holds Engine\>." }
$Names = @("alpha", "beta", "gamma", "delta", "epsilon")
if (-not (Test-Path (Join-Path $Root "Config\DedicatedServerGame.ini"))) {
    Write-Error "Config\DedicatedServerGame.ini is missing: copy DedicatedServerGame.example.ini and put the server key in it."
}
New-Item -ItemType Directory -Force (Join-Path $Root "Saved\Logs") | Out-Null
for ($I = 0; $I -lt $Count; $I++) {
    $Name = $Names[$I]
    $Port = $FirstPort + $I
    $Log = Join-Path $Root "Saved\Logs\server-$Name.log"
    $Args = @("`"$Project`"", "/Engine/Maps/Entry", "-server", "-log", "-port=$Port", "-servername=$Name", "-unattended", "-nosound", "-abslog=`"$Log`"")
    if ($PublicHost -ne "") { $Args += "-PublicHost=$PublicHost" }
    Start-Process -FilePath $Editor -ArgumentList $Args -WorkingDirectory $Root | Out-Null
    Write-Host "server $Name on UDP $Port, log $Log"
    Start-Sleep -Seconds 2
}

# Plays Cube World on this machine alone, with no platform at all (-cubeoffline): two or three Unreal servers side by
# side in the lower row of regions (3, 4, 5) and, if asked, a client that walks over the borders between them. Nothing
# signs in to dev, which is shared and live; each server keeps its own world and hands everyone the starting inventory.
# Logs go to Saved/Logs/offline-<name>.log.
#
#   .\Scripts\RunOffline.ps1                              # servers on UDP 7777 and 7778 (regions 3 and 4)
#   .\Scripts\RunOffline.ps1 -Client -WalkTo 30           # and a client that walks from region 3 into region 4
#   .\Scripts\RunOffline.ps1 -Servers 3 -Client -WalkTo 60 -NullRhi -QuitAfter 40   # unattended, no window
#   .\Scripts\RunOffline.ps1 -Client -WalkTo 30 -DoorRegions 4   # region 4 over the JSON socket, as a C# server is played
#   .\Scripts\RunOffline.ps1 -Stop                        # ends every offline process of this project
#
# Every server also opens its JSON door (WebSocket, ten above its UDP port). -DoorRegions lists the regions the client
# plays through that door instead of Iris, so crossings between Iris and a socket are tried without a C# server. The
# door is plain ws on this machine: only the platform's TLS front makes it wss.
param(
    [int]$Servers = 2,
    [int]$FirstPort = 7777,
    [switch]$Client,
    [string]$Name = "Ann",
    [string]$WalkTo = "",
    [switch]$NullRhi,
    [int]$QuitAfter = 0,
    [string]$ClientExtra = "",
    [string]$DoorRegions = "",
    [string]$Engine = "",
    [switch]$Stop
)
$ErrorActionPreference = "Stop"
$Project = Resolve-Path (Join-Path $PSScriptRoot "..\CubeWorld.uproject")
$Root = Split-Path $Project

if ($Stop) {
    Get-CimInstance Win32_Process -Filter "Name like 'UnrealEditor%'" |
        Where-Object { $_.CommandLine -like "*cubeoffline*" -and $_.CommandLine -like "*$Project*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Host "stopped $($_.ProcessId)" }
    return
}

if ($Engine -eq "") {
    # The Launcher records where it installed each engine version.
    $Record = "C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat"
    if (Test-Path $Record) {
        $Install = (Get-Content $Record -Raw | ConvertFrom-Json).InstallationList | Where-Object { $_.AppName -eq "UE_5.8" } | Select-Object -First 1
        if ($Install) { $Engine = $Install.InstallLocation }
    }
    if ($Engine -eq "") { $Engine = "D:\EpicGames\UE_5.8" }
}
$EditorCmd = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Editor = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $EditorCmd)) { throw "No editor at $EditorCmd; pass -Engine <the folder that holds Engine\>." }

$Names = @("alpha", "beta", "gamma")
$Doors = @($DoorRegions -split "[, ]+" | Where-Object { $_ -ne "" } | ForEach-Object { [int]$_ })
$Peers = @()
for ($I = 0; $I -lt $Servers; $I++) {
    $Region = 3 + $I
    if ($Doors -contains $Region) { $Peers += "$Region@ws://127.0.0.1:$($FirstPort + $I + 10)" }
    else { $Peers += "$Region@127.0.0.1:$($FirstPort + $I)" }
}
$PeerList = $Peers -join ","
New-Item -ItemType Directory -Force (Join-Path $Root "Saved\Logs") | Out-Null

for ($I = 0; $I -lt $Servers; $I++) {
    $Log = Join-Path $Root "Saved\Logs\offline-$($Names[$I]).log"
    $Args = @("`"$Project`"", "/Engine/Maps/Entry", "-server", "-cubeoffline", "-region=$(3 + $I)", "-peers=$PeerList",
        "-port=$($FirstPort + $I)", "-servername=$($Names[$I])", "-log", "-unattended", "-nosound", "-abslog=`"$Log`"")
    Start-Process -FilePath $EditorCmd -ArgumentList $Args -WorkingDirectory $Root -WindowStyle Minimized | Out-Null
    Write-Host "server $($Names[$I]): region $(3 + $I), UDP $($FirstPort + $I), door ws $($FirstPort + $I + 10), log $Log"
}

if ($Client) {
    Start-Sleep -Seconds 8
    $Log = Join-Path $Root "Saved\Logs\offline-client-$Name.log"
    $Args = @("`"$Project`"", "/Engine/Maps/Entry", "-game", "-cubeoffline", "-peers=$PeerList", "-name=$Name", "-autoplay",
        "-windowed", "-resx=1280", "-resy=720", "-log", "-abslog=`"$Log`"")
    if ($WalkTo -ne "") { $Args += "-walkto=$WalkTo" }
    if ($NullRhi) { $Args += "-nullrhi" }
    if ($QuitAfter -gt 0) { $Args += "-quitafter=$QuitAfter" }
    if ($ClientExtra -ne "") { $Args += $ClientExtra -split " " }
    Start-Process -FilePath ($(if ($NullRhi) { $EditorCmd } else { $Editor })) -ArgumentList $Args -WorkingDirectory $Root | Out-Null
    Write-Host "client $Name, log $Log"
}

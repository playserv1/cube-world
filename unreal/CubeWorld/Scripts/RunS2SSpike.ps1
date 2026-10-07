# Runs the server-to-server replication spike (Source/CubeWorld/S2SSpike) on this machine, with no platform at all:
# server A mirrors a projectile and an actor with an ability system component through a second net driver to server B,
# and headless clients watch from A's or B's game port. Every process logs its numbers and exits after -QuitAfter
# seconds; logs go to Saved/S2S/<Run>/.
#
#   powershell -ExecutionPolicy Bypass -File Scripts\RunS2SSpike.ps1 -Run base -ClientToA -ClientToB
#   powershell -ExecutionPolicy Bypass -File Scripts\RunS2SSpike.ps1 -Run full -AscMode Full
#   powershell -ExecutionPolicy Bypass -File Scripts\RunS2SSpike.ps1 -Run p100 -Projectiles 100
#   powershell -ExecutionPolicy Bypass -File Scripts\RunS2SSpike.ps1 -Run relay -Relay anyrole -ClientToB
#   powershell -ExecutionPolicy Bypass -File Scripts\RunS2SSpike.ps1 -Run refuse -ClientToB -IrisVerboseB
#   powershell -ExecutionPolicy Bypass -File Scripts\RunS2SSpike.ps1 -Run beacon -Beacon
#
# Ports: A's game 17777, B's game 17778, A's S2S listener 17790 (other sessions use 7777/7778).
param(
    [string]$Run = "base",
    [int]$Projectiles = 1,
    [string]$AscMode = "Mixed",
    [string]$Relay = "",
    [switch]$ClientToA,
    [switch]$ClientToB,
    [switch]$Beacon,
    [int]$QuitAfter = 45,
    [switch]$IrisVerboseB,
    [int]$TickRateA = 0,
    [int]$TickRateB = 0,
    [int]$AQuitAfter = 0,
    [string]$ExtraA = "",
    [string]$ExtraB = "",
    [string]$Engine = "D:\EpicGames\UE_5.8"
)
$ErrorActionPreference = "Stop"
$Project = Resolve-Path (Join-Path $PSScriptRoot "..\CubeWorld.uproject")
$Root = Split-Path $Project
$EditorCmd = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$LogDir = Join-Path $Root "Saved\S2S\$Run"
New-Item -ItemType Directory -Force $LogDir | Out-Null
Get-ChildItem $LogDir -Filter *.log -ErrorAction SilentlyContinue | Remove-Item -Force

# The stock game instance: none of Cube World's client or server logic runs in the spike.
$Common = @("-log", "-unattended", "-nosound", "-s2sspike",
    "-ini:Engine:[/Script/EngineSettings.GameMapsSettings]:GameInstanceClass=/Script/Engine.GameInstance")
$Map = "/Engine/Maps/Entry?game=/Script/CubeWorld.S2SSpikeGameMode"
$Procs = @()

function Start-Unreal([string]$Name, [string[]]$Arguments) {
    $Log = Join-Path $LogDir "$Name.log"
    $All = @("`"$Project`"") + $Arguments + $Common + @("-s2sname=$Name", "-abslog=`"$Log`"")
    $P = Start-Process -FilePath $EditorCmd -ArgumentList $All -WorkingDirectory $Root -WindowStyle Hidden -PassThru
    Write-Host "$Name pid $($P.Id) log $Log"
    return $P
}

$LinkA = if ($Beacon) { "-s2sbeaconhost=17790" } else { "-s2shost=17790" }
$LinkB = if ($Beacon) { "-s2sbeaconpeer=127.0.0.1:17790" } else { "-s2speer=127.0.0.1:17790" }

# A outlives B unless -AQuitAfter makes it leave first (to see what B does when the link drops).
$QuitA = if ($AQuitAfter -gt 0) { $AQuitAfter } else { $QuitAfter + 10 }
$ArgsA = @($Map, "-server", "-port=17777", $LinkA, "-s2sprojectiles=$Projectiles", "-s2sascmode=$AscMode", "-s2squitafter=$QuitA")
if ($TickRateA -gt 0) { $ArgsA += "-ini:Engine:[/Script/OnlineSubsystemUtils.IpNetDriver]:NetServerMaxTickRate=$TickRateA" }
if ($ExtraA -ne "") { $ArgsA += $ExtraA -split " " }
$Procs += Start-Unreal "A" $ArgsA
Start-Sleep -Seconds 4

$ArgsB = @($Map, "-server", "-port=17778", $LinkB, "-s2squitafter=$QuitAfter")
if ($Relay -ne "") { $ArgsB += "-s2srelay=$Relay" }
if ($TickRateB -gt 0) { $ArgsB += "-ini:Engine:[/Script/OnlineSubsystemUtils.IpNetDriver]:NetServerMaxTickRate=$TickRateB" }
# LogIris VeryVerbose shows the bridge refusing the copies ("NetRole isn't Authority").
if ($IrisVerboseB) { $ArgsB += "-ini:Engine:[Core.Log]:LogIris=VeryVerbose" }
if ($ExtraB -ne "") { $ArgsB += $ExtraB -split " " }
$Procs += Start-Unreal "B" $ArgsB
Start-Sleep -Seconds 6

# B outlives its clients and A outlives B, so the final numbers are taken on live links.
$ClientQuit = $QuitAfter - 12
if ($ClientToA) { $Procs += Start-Unreal "clientA" @("127.0.0.1:17777", "-game", "-nullrhi", "-s2squitafter=$ClientQuit") }
if ($ClientToB) { $Procs += Start-Unreal "clientB" @("127.0.0.1:17778", "-game", "-nullrhi", "-s2squitafter=$ClientQuit") }

# Every process exits on its own (-s2squitafter); anything still up a minute later is stopped.
$Deadline = (Get-Date).AddSeconds($QuitAfter + 70)
foreach ($P in $Procs) {
    $Left = [int][Math]::Max(1, ($Deadline - (Get-Date)).TotalSeconds)
    if (-not $P.WaitForExit($Left * 1000)) { Write-Host "stopping $($P.Id)"; Stop-Process -Id $P.Id -Force }
}
Write-Host "done: $LogDir"

# Runs the GAS lab on this machine (Source/CubeWorld/GasLab): two or four Unreal dedicated servers, one per region of
# a 2 x 2 world, linked server to server, playing one of the research scenarios with server-side bots. Nothing touches
# the PlayServ platform. Logs go to Saved/Logs/gaslab-<Tag>-<region>.log; Scripts/AnalyzeGasLab.py reads them.
#
#   .\Scripts\RunGasLab.ps1 -Servers 4 -Scenario corner -Seconds 30              # the creeper at the four-server corner
#   .\Scripts\RunGasLab.ps1 -Servers 2 -Scenario cross -Seconds 25               # burning and a cooldown carried over a border
#   .\Scripts\RunGasLab.ps1 -Servers 4 -Scenario corner -Mode area                # the creeper carries its effects to each server
#   .\Scripts\RunGasLab.ps1 -Servers 4 -Scenario corner -Relay ws://127.0.0.1:7900/s2s   # over the mock platform relay
#   .\Scripts\RunGasLab.ps1 -Servers 2 -Scenario cross -NoClock                   # each server keeps its own clock (control)
#   .\Scripts\RunGasLab.ps1 -Servers 4 -Scenario soak -Bots 10 -Seconds 60        # many bots walking and throwing everywhere
#   .\Scripts\RunGasLab.ps1 -Servers 2 -Client -ClientExtra "-gaswalk=1800,1200;3000,1200 -gaswalkat=6"   # a client too
#   .\Scripts\RunGasLab.ps1 -Servers 4 -Scenario corner -TickRate 120 -Tag corner-120hz   # servers at 120 Hz
#   .\Scripts\RunGasLab.ps1 -Stop
param(
    [int]$Servers = 4,
    [string]$Scenario = "corner",
    [ValidateSet("forward", "area")][string]$Mode = "forward",
    [string]$Relay = "",
    [double]$Delay = 0,
    [switch]$NoClock,
    [int]$Bots = 0,
    [int]$TickRate = 0,
    [int]$Seconds = 0,
    [int]$FirstPort = 7801,
    [switch]$Client,
    [string]$Name = "Ann",
    [switch]$Window,
    [string]$ClientExtra = "",
    [string]$Tag = "",
    [string]$Engine = "D:\EpicGames\UE_5.8",
    [switch]$Stop
)
$ErrorActionPreference = "Stop"
$Project = Resolve-Path (Join-Path $PSScriptRoot "..\CubeWorld.uproject")
$Root = Split-Path $Project
$EditorCmd = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Editor = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.exe"

function Stop-Lab {
    Get-CimInstance Win32_Process -Filter "Name like 'UnrealEditor%'" |
        Where-Object { $_.CommandLine -like "*-gaslab*" -and $_.CommandLine -like "*$Project*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; Write-Host "stopped $($_.ProcessId)" }
}
if ($Stop) { Stop-Lab; return }
if ($Tag -eq "") { $Tag = "$Scenario-$Mode" + $(if ($Relay -ne "") { "-relay" } else { "-direct" }) + $(if ($NoClock) { "-noclock" } else { "" }) }

$Epoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$Peers = (0..($Servers - 1) | ForEach-Object { "$_@127.0.0.1:$($FirstPort + $_)" }) -join ","
$Common = @("-gaslab", "-gasepoch=$Epoch", "-log", "-unattended", "-nosound",
    "-ini:Engine:[/Script/EngineSettings.GameMapsSettings]:GameInstanceClass=/Script/Engine.GameInstance")
if ($NoClock) { $Common += "-gasnoclock" }
# The servers' frame rate (DefaultEngine.ini says 30): a message waits up to one frame on each side.
if ($TickRate -gt 0) { $Common += "-ini:Engine:[/Script/OnlineSubsystemUtils.IpNetDriver]:NetServerMaxTickRate=$TickRate" }
New-Item -ItemType Directory -Force (Join-Path $Root "Saved\Logs") | Out-Null

for ($R = 0; $R -lt $Servers; $R++) {
    $Log = Join-Path $Root "Saved\Logs\gaslab-$Tag-$R.log"
    if (Test-Path $Log) { Remove-Item $Log -Force }
    $Args = @("`"$Project`"", "/Engine/Maps/Entry?game=/Script/CubeWorld.GasLabGameMode", "-server", "-region=$R", "-port=$($FirstPort + $R)",
        "-peers=$Peers", "-gasscenario=$Scenario", "-gasmode=$Mode", "-gasbots=$Bots", "-abslog=`"$Log`"") + $Common
    if ($Relay -ne "") { $Args += "-gasrelay=$Relay" }
    if ($Delay -gt 0) { $Args += "-gaslinkdelay=$Delay" }
    Start-Process -FilePath $EditorCmd -ArgumentList $Args -WorkingDirectory $Root -WindowStyle Minimized | Out-Null
    Write-Host "server region $R on UDP $($FirstPort + $R), link TCP $($FirstPort + $R + 100), log $Log"
}

if ($Client) {
    Start-Sleep -Seconds 4
    $Log = Join-Path $Root "Saved\Logs\gaslab-$Tag-client-$Name.log"
    if (Test-Path $Log) { Remove-Item $Log -Force }
    $Args = @("`"$Project`"", "127.0.0.1:$FirstPort`?gasname=$Name", "-game", "-abslog=`"$Log`"") + $Common
    if ($Window) { $Args += @("-windowed", "-resx=1280", "-resy=720") } else { $Args += "-nullrhi" }
    if ($ClientExtra -ne "") { $Args += $ClientExtra -split " " }
    Start-Process -FilePath ($(if ($Window) { $Editor } else { $EditorCmd })) -ArgumentList $Args -WorkingDirectory $Root | Out-Null
    Write-Host "client $Name, log $Log"
}

if ($Seconds -gt 0) {
    Start-Sleep -Seconds $Seconds
    Stop-Lab
}

param([int]$Wait = 30, [int]$Shots = 45, [string]$Out = "C:\Users\klaud\Desktop\switch\ports\wip\speedracer-revival\local\intro\load")
# With the original already running: wait for its intro, click PLAY (real mouse, so it moves the cursor),
# then screenshot its windows ~4 times a second to catch the loading screen and the fade.
Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Out | Out-Null
Start-Sleep -Seconds $Wait
& "$env:SR_TOOLS\win-shots.ps1" -Process SR_GAME -OutPrefix "$Out\pre" | Out-Null
& "$env:SR_TOOLS\win-click.ps1" -Process SR_GAME -X 450 -Y 241 -Real
for ($i = 0; $i -lt $Shots; $i++) {
  & "$env:SR_TOOLS\win-shots.ps1" -Process SR_GAME -OutPrefix "$Out\l$('{0:D2}' -f $i)" | Out-Null
  Start-Sleep -Milliseconds 250
}
(Get-ChildItem $Out).Count

param([int]$Wait = 20, [int]$After = 14)
# With the original already running: wait for its intro, click PLAY (real mouse), wait for the race to be ready.
Start-Sleep -Seconds $Wait
& "$env:SR_TOOLS\win-click.ps1" -Process SR_GAME -X 450 -Y 241 -Real
Start-Sleep -Seconds $After
& "$env:SR_TOOLS\win-shots.ps1" -Process SR_GAME -OutPrefix "C:\Users\klaud\Desktop\switch\ports\wip\speedracer-revival\local\intro\ready"

<#
  Tests the packaged installer against fake game/user folders (never the real game).
  usage: powershell -File tests\test_installer.ps1 <dist\cts4p-x.y.z.zip>
#>
param([Parameter(Mandatory)] [string]$Zip)
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:TEMP ('cts4p_installer_test_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$pkg = Join-Path $work 'pacote'
Expand-Archive $Zip $pkg
$failures = 0

function Check($ok, $what) {
    if ($ok) { Write-Host "  [ OK ] $what" -ForegroundColor Green } else { Write-Host "  [FAIL] $what" -ForegroundColor Red; $script:failures++ }
}
function New-Fake([string]$name, [bool]$foreignDll) {
    $game = Join-Path $work "$name\Jogo\The Sims 4"; $user = Join-Path $work "$name\Docs\The Sims 4"
    New-Item -ItemType Directory -Force (Join-Path $game 'Game\Bin'), (Join-Path $user 'Mods') | Out-Null
    Set-Content (Join-Path $game 'Game\Bin\TS4_x64.exe') 'fake'
    if ($foreignDll) { Copy-Item "$env:SystemRoot\System32\winmm.dll" (Join-Path $game 'Game\Bin\winmm.dll') }
    Set-Content (Join-Path $user 'Options.ini') "[options]`r`nscriptmodsenabled = 0`r`nmodsdisabled = 1`r`nlanguage = pt" -Encoding ASCII
    return @{ game = $game; user = $user; bin = (Join-Path $game 'Game\Bin'); mod = (Join-Path $user 'Mods\cts4p') }
}
function Inst($f, $action) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pkg 'instalar.ps1') -Action $action -GameDir $f.game -UserDir $f.user -Quiet *> $null
    return $LASTEXITCODE
}
function Is-Ours($path) { (Test-Path $path) -and (Get-Item $path).VersionInfo.ProductName -eq 'cts4p' }

Write-Host "Scenario A: another mod's winmm.dll + S4MP, install -> update -> uninstall"
$a = New-Fake 'A' $true
Set-Content (Join-Path $a.user 'Mods\S4MP_release.ts4script') 'old mod'
$foreign = (Get-FileHash (Join-Path $a.bin 'winmm.dll')).Hash
Check ((Inst $a 'Install') -eq 0) 'install exits 0'
Check (Is-Ours (Join-Path $a.bin 'winmm.dll')) 'our winmm.dll installed'
Check (Test-Path (Join-Path $a.mod 'cts4p.ts4script')) 'ts4script installed'
$ini = Get-Content (Join-Path $a.user 'Options.ini') -Raw
Check ($ini -match 'scriptmodsenabled = 1' -and $ini -match 'modsdisabled = 0' -and $ini -match 'language = pt') 'Options.ini: script mods on, other keys kept'
Check (-not (Test-Path (Join-Path $a.user 'Mods\S4MP_release.ts4script'))) 'conflicting S4MP moved out of Mods'
Check ((Get-Content (Join-Path $a.mod 'config.json') -Raw | ConvertFrom-Json).signaling_url -eq 'ws://127.0.0.1:8787/ws') 'config.json created'
Start-Sleep -Seconds 1
Check ((Inst $a 'Install') -eq 0) 'update (second install) exits 0'
Check ((Inst $a 'Uninstall') -eq 0) 'uninstall exits 0'
Check ((Test-Path (Join-Path $a.bin 'winmm.dll')) -and (Get-FileHash (Join-Path $a.bin 'winmm.dll')).Hash -eq $foreign) "other mod's winmm.dll restored byte-identical after an update"
Check (-not (Test-Path (Join-Path $a.mod 'cts4p.ts4script'))) 'ts4script removed'
Check (Test-Path (Join-Path $a.mod 'config.json')) 'config.json kept'

Write-Host "Scenario B: clean game, install -> uninstall leaves no winmm.dll"
$b = New-Fake 'B' $false
Check ((Inst $b 'Install') -eq 0) 'install exits 0'
Check (Is-Ours (Join-Path $b.bin 'winmm.dll')) 'our winmm.dll installed'
Check ((Inst $b 'Uninstall') -eq 0) 'uninstall exits 0'
Check (-not (Test-Path (Join-Path $b.bin 'winmm.dll'))) 'no winmm.dll left behind'

Write-Host "Scenario D: update from the pre-rename version (Mods\ts4mp) keeps the player's settings"
$d = New-Fake 'D' $true
$legacy = Join-Path $d.user 'Mods\ts4mp'
New-Item -ItemType Directory -Force $legacy | Out-Null
Set-Content (Join-Path $legacy 'ts4mp.ts4script') 'old scripts'
[IO.File]::WriteAllText((Join-Path $legacy 'config.json'), '{"signaling_url":"wss://exemplo/ws","nickname":"Dani"}')
$dForeign = (Get-FileHash (Join-Path $d.bin 'winmm.dll')).Hash
Check ((Inst $d 'Install') -eq 0) 'install over the old version exits 0'
$dCfg = Get-Content (Join-Path $d.mod 'config.json') -Raw | ConvertFrom-Json
Check ($dCfg.nickname -eq 'Dani' -and $dCfg.signaling_url -eq 'wss://exemplo/ws') 'old config.json migrated (name and server kept)'
Check (-not (Test-Path $legacy)) 'old Mods\ts4mp folder moved to the backup (no duplicate scripts)'
Check ((Inst $d 'Uninstall') -eq 0) 'uninstall exits 0'
Check ((Get-FileHash (Join-Path $d.bin 'winmm.dll')).Hash -eq $dForeign) "other mod's winmm.dll still restored"

Write-Host "Scenario C: refuses while the game is running"
$c = New-Fake 'C' $false
$fakeGame = Join-Path $work 'TS4_x64.exe'
Copy-Item "$env:SystemRoot\System32\timeout.exe" $fakeGame
$proc = Start-Process $fakeGame -ArgumentList '30' -PassThru -WindowStyle Hidden
try {
    Check ((Inst $c 'Install') -ne 0) 'install refused with exit code != 0'
    Check (-not (Test-Path (Join-Path $c.bin 'winmm.dll'))) 'nothing copied while the game runs'
} finally { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ("test_installer: {0} ({1} failure(s))" -f ($(if ($failures) { 'FAILED' } else { 'PASSED' }), $failures))
exit $failures

<#
  cts4p - build completo, testes e pacote de distribuição.

    build_all.bat                  compila (Release x64), testa e gera dist\cts4p-<versão>.zip
    build_all.bat -FullTests       + testes de rede entre processos (sinalização, sala, save, construção) e overlay
    build_all.bat -SkipTests       só compila e empacota
    build_all.bat -Install         no fim, instala neste PC usando o próprio instalador do pacote

  Requisitos: Visual Studio 2022 (C++), CMake, The Sims 4 instalado (o .ts4script é
  compilado com o Python 3.7 do próprio jogo). Python 3 no PATH só para -FullTests.
#>
[CmdletBinding()]
param([switch]$SkipTests, [switch]$FullTests, [switch]$Install, [string]$GameDir)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Native = Join-Path $Root 'native'
$Release = Join-Path $Native 'build\Release'
$Dist = Join-Path $Root 'dist'
$results = New-Object System.Collections.Generic.List[string]

function Step([string]$text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Pass([string]$name) { $results.Add("[ OK ] $name"); Write-Host "  [ OK ] $name" -ForegroundColor Green }
function Die([string]$text) { Write-Host "  [ERRO] $text" -ForegroundColor Red; exit 1 }

function Run([string]$name, [string]$exe, [string[]]$arguments, [int]$timeoutSec = 180) {
    $out = Join-Path $env:TEMP ("cts4p_build_" + [guid]::NewGuid().ToString('N') + '.log')
    $startArgs = @{ FilePath = $exe; NoNewWindow = $true; PassThru = $true; RedirectStandardOutput = $out; RedirectStandardError = "$out.err" }
    if ($arguments -and $arguments.Count -gt 0) { $startArgs.ArgumentList = $arguments }  # PS 5.1 rejects an empty ArgumentList
    $p = Start-Process @startArgs
    $null = $p.Handle  # PowerShell 5.1: caches the handle so ExitCode is available after exit
    if (-not $p.WaitForExit($timeoutSec * 1000)) { $p.Kill(); Die "${name}: tempo esgotado ($timeoutSec s)" }
    $text = (Get-Content $out -Raw -ErrorAction SilentlyContinue) + (Get-Content "$out.err" -Raw -ErrorAction SilentlyContinue)
    Remove-Item $out, "$out.err" -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { Write-Host $text; Die "$name falhou (código $($p.ExitCode))" }
    Pass $name
    return $text
}

# ------------------------------------------------------------------ toolchain
Step 'Ferramentas'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Die 'Visual Studio não encontrado (vswhere.exe ausente).' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Die 'Visual Studio sem o workload "Desenvolvimento para desktop com C++".' }
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $vs 'VC\vcpkg' }
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
if (-not (Test-Path $cmake)) { Die 'CMake não encontrado.' }
Pass "Visual Studio: $vs"
Pass "vcpkg: $env:VCPKG_ROOT"

if (-not $GameDir) {
    foreach ($key in 'HKLM:\SOFTWARE\Maxis\The Sims 4', 'HKLM:\SOFTWARE\WOW6432Node\Maxis\The Sims 4') {
        try { $GameDir = ((Get-ItemProperty -Path $key -ErrorAction Stop).'Install Dir').TrimEnd('\'); break } catch { }
    }
}
$GameBin = Join-Path $GameDir 'Game\Bin'
$BaseZip = Join-Path $GameDir 'Data\Simulation\Gameplay\base.zip'
if (-not (Test-Path (Join-Path $GameBin 'python37_x64.dll'))) { Die "The Sims 4 não encontrado ('$GameDir'). Use -GameDir." }
Pass "The Sims 4: $GameDir"

# ------------------------------------------------------------------ native
Step 'Compilando a DLL nativa (Release x64, /W4 /WX)'
Push-Location $Native
try {
    # Through cmd: native stderr (e.g. harmless CMake warnings) must not become PowerShell errors.
    $log = cmd /c "`"$cmake`" --preset x64-release 2>&1 && `"$cmake`" --build --preset x64-release 2>&1" | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Host $log; Die 'compilação falhou' }
    $warnings = ([regex]::Matches($log, ' warning (C|LNK|MSB|RC)\d+')).Count
    if ($warnings -gt 0) { Write-Host $log; Die "$warnings warning(s) na compilação" }
} finally { Pop-Location }
$version = (Get-Item (Join-Path $Release 'winmm.dll')).VersionInfo.ProductVersion
Pass "winmm.dll $version (0 warnings)"

# ------------------------------------------------------------------ scripts
Step 'Compilando o cts4p.ts4script com o Python 3.7 do jogo'
$py37 = Join-Path $Release 'py37host.exe'
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$ts4script = Join-Path $Dist 'cts4p.ts4script'
Run 'cts4p.ts4script compilado e importado do pacote' $py37 @("`"$GameBin`"", "`"$BaseZip`"", '""', "`"$Root\tools\build_ts4script.py`"", "`"$Root\srcScripts`"", "`"$ts4script`"") | Out-Null

# ------------------------------------------------------------------ tests
if (-not $SkipTests) {
    Step 'Testes'
    Push-Location $Release
    try {
        Run 'proxy winmm (181 exports)' (Join-Path $Release 'test_proxy.exe') @() | Out-Null
        Run 'WebRTC loopback 127.0.0.1' (Join-Path $Release 'test_net.exe') @('loopback') | Out-Null
        Run 'hook de construção vs Simulation_x64.dll real' (Join-Path $Release 'test_build.exe') @("`"$GameBin\Simulation_x64.dll`"") | Out-Null
        Run 'Criar juntos: arquivos da Biblioteca' (Join-Path $Release 'test_tray.exe') @() | Out-Null
        $scratch = Join-Path $env:TEMP 'cts4p_build_pytest'
        New-Item -ItemType Directory -Force -Path $scratch | Out-Null
        Run 'ponte Python no Python 3.7 do jogo (a partir do .ts4script)' $py37 @("`"$GameBin`"", "`"$BaseZip`"", "`"$ts4script`"", "`"$Root\tests\python\test_game_python.py`"", "`"$Release\winmm.dll`"", "`"$scratch`"") | Out-Null

        if ($FullTests) {
            $python = (Get-Command python -ErrorAction SilentlyContinue).Source
            if (-not $python) { Die '-FullTests precisa de Python 3 no PATH (servidor de sinalização local).' }
            $server = Start-Process $python -ArgumentList @('-I', "`"$Root\signaling\python\server.py`"", '--host', '127.0.0.1', '--port', '8787') -PassThru -WindowStyle Hidden
            try {
                Start-Sleep -Seconds 2
                $url = 'ws://127.0.0.1:8787/ws'
                Run 'servidor de sinalização (protocolo)' (Join-Path $Release 'test_net.exe') @('signaling', $url) | Out-Null
                $frame = Join-Path $Native 'tests\data_buildop_frame.bin'
                foreach ($pair in @(@('host', 'join', 'sala entre 2 processos'), @('p3host', 'p3join', 'manifesto + save 12 MB + gameplay'), @('p4host', 'p4join', 'replicação de construção + portão'))) {
                    $code = Join-Path $env:TEMP ("cts4p_code_" + $pair[0] + '.txt')
                    Remove-Item $code -ErrorAction SilentlyContinue
                    $h = Start-Process (Join-Path $Release 'test_net.exe') -ArgumentList @($pair[0], $url, "`"$code`"", "`"$frame`"") -PassThru -NoNewWindow -RedirectStandardOutput "$code.host.log"
                    $null = $h.Handle
                    $j = Start-Process (Join-Path $Release 'test_net.exe') -ArgumentList @($pair[1], $url, "`"$code`"", "`"$frame`"") -PassThru -NoNewWindow -RedirectStandardOutput "$code.join.log"
                    $null = $j.Handle
                    if (-not ($h.WaitForExit(180000) -and $j.WaitForExit(180000))) { Die "$($pair[2]): tempo esgotado" }
                    if ($h.ExitCode -ne 0 -or $j.ExitCode -ne 0) { Get-Content "$code.host.log", "$code.join.log"; Die "$($pair[2]) falhou" }
                    Pass $pair[2]
                }
                Run 'overlay D3D11 (janela de teste)' (Join-Path $Release 'test_overlay.exe') @() | Out-Null
            } finally { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
        }
    } finally { Pop-Location }
}

# ------------------------------------------------------------------ package
Step 'Pacote'
$name = "cts4p-$version"
$stage = Join-Path $Dist $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'signaling\cloudflare'), (Join-Path $stage 'signaling\python') | Out-Null
Copy-Item (Join-Path $Release 'winmm.dll') $stage
Copy-Item $ts4script $stage
Copy-Item (Join-Path $Root 'installer\instalar.bat'), (Join-Path $Root 'installer\instalar.ps1'), (Join-Path $Root 'installer\LEIA-ME.txt') $stage
Copy-Item (Join-Path $Native 'config.example.json') $stage
Copy-Item (Join-Path $Root 'signaling\cloudflare\worker.js'), (Join-Path $Root 'signaling\cloudflare\wrangler.toml'), (Join-Path $Root 'signaling\cloudflare\README.md') (Join-Path $stage 'signaling\cloudflare')
Copy-Item (Join-Path $Root 'signaling\python\server.py') (Join-Path $stage 'signaling\python')
$zip = Join-Path $Dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hashes = Get-ChildItem $stage -Recurse -File | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16), $_.FullName.Substring($stage.Length + 1) }
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256.txt'), $hashes)
Pass ("{0} ({1:N0} KB)" -f $zip, ((Get-Item $zip).Length / 1KB))

if (-not $SkipTests) {
    Step 'Teste do instalador (pastas falsas, nunca o jogo real)'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tests\test_installer.ps1') -Zip $zip *> (Join-Path $env:TEMP 'cts4p_installer_test.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $env:TEMP 'cts4p_installer_test.log'); Die 'teste do instalador falhou' }
    Pass 'instalador: instalar, atualizar (inclusive do ts4mp antigo), desinstalar e restaurar'
}

if ($Install) {
    Step 'Instalando neste PC com o instalador do pacote'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $stage 'instalar.ps1') -Action Install -Quiet
    if ($LASTEXITCODE -ne 0) { Die 'instalação falhou' }
    Pass 'instalado'
}

Write-Host "`n==== Resumo ====" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host $_ }
Write-Host "`nPacote para o outro PC: $zip" -ForegroundColor Green

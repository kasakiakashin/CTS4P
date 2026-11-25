<#
  CTS4P (Co-op The Sims 4 Players) - instalador / desinstalador

  Uso normal: dois cliques em instalar.bat (abre um menu).
  Linha de comando:
    instalar.ps1 -Action Install   [-GameDir <pasta do jogo>] [-UserDir <Documentos\...\The Sims 4>] [-SignalingUrl wss://...] [-Quiet]
    instalar.ps1 -Action Uninstall [-GameDir ...] [-UserDir ...] [-Quiet]

  O que faz:
    * acha o jogo (registro do Windows / Steam / EA app) e a pasta do usuário (onde fica o Options.ini);
    * faz backup do que for substituir em <pasta do usuário>\cts4p_backup\<data-hora>\;
    * copia winmm.dll para Game\Bin e cts4p.ts4script para Mods\cts4p;
    * liga os mods de script no Options.ini (com backup);
    * cria Mods\cts4p\config.json na primeira instalação;
    * registra tudo em Mods\cts4p\install.json, usado para desinstalar e restaurar.
  Nunca apaga uma winmm.dll que não seja do cts4p.
#>
[CmdletBinding()]
param(
    [ValidateSet('Menu', 'Install', 'Uninstall')] [string]$Action = 'Menu',
    [string]$GameDir,
    [string]$UserDir,
    [string]$SignalingUrl,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$DefaultSignaling = 'ws://127.0.0.1:8787/ws'

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Ok([string]$text) { Say "  [OK] $text" 'Green' }
function Warn([string]$text) { Say "  [AVISO] $text" 'Yellow' }
function Fail([string]$text) { Say "  [ERRO] $text" 'Red'; throw $text }

function Ask([string]$question, [bool]$default) {
    if ($Quiet) { return $default }
    $suffix = if ($default) { '[S/n]' } else { '[s/N]' }
    $answer = Read-Host "$question $suffix"
    if ([string]::IsNullOrWhiteSpace($answer)) { return $default }
    return $answer.Trim().ToLower().StartsWith('s')
}

function Test-GameDir([string]$dir) {
    return $dir -and (Test-Path (Join-Path $dir 'Game\Bin\TS4_x64.exe'))
}

function Find-GameDir {
    if ($GameDir) {
        if (Test-GameDir $GameDir) { return (Resolve-Path $GameDir).Path }
        Fail "TS4_x64.exe não encontrado em '$GameDir\Game\Bin'."
    }
    foreach ($key in 'HKLM:\SOFTWARE\Maxis\The Sims 4', 'HKLM:\SOFTWARE\WOW6432Node\Maxis\The Sims 4') {
        try {
            $dir = (Get-ItemProperty -Path $key -ErrorAction Stop).'Install Dir'
            if (Test-GameDir $dir) { return $dir.TrimEnd('\') }
        } catch { }
    }
    $candidates = @(
        "$env:ProgramFiles\EA Games\The Sims 4",
        "${env:ProgramFiles(x86)}\Origin Games\The Sims 4",
        "${env:ProgramFiles(x86)}\Steam\steamapps\common\The Sims 4",
        "$env:ProgramFiles\Steam\steamapps\common\The Sims 4"
    )
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem).Root) {
        $candidates += (Join-Path $drive 'SteamLibrary\steamapps\common\The Sims 4')
    }
    foreach ($dir in $candidates) { if (Test-GameDir $dir) { return $dir } }
    if ($Quiet) { Fail 'Não encontrei o The Sims 4. Use -GameDir.' }
    $dir = Read-Host 'Não encontrei o The Sims 4. Cole o caminho da pasta do jogo (a que tem a pasta Game)'
    if (Test-GameDir $dir) { return $dir.Trim('"').TrimEnd('\') }
    Fail 'Caminho inválido: TS4_x64.exe não está em Game\Bin.'
}

function Find-UserDir {
    if ($UserDir) {
        if (-not (Test-Path $UserDir)) { New-Item -ItemType Directory -Path $UserDir | Out-Null }
        return (Resolve-Path $UserDir).Path
    }
    $ea = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Electronic Arts'
    $standard = Join-Path $ea 'The Sims 4'
    if (Test-Path (Join-Path $standard 'Options.ini')) { return $standard }
    if (Test-Path $ea) {
        foreach ($d in Get-ChildItem $ea -Directory) {
            if (Test-Path (Join-Path $d.FullName 'Options.ini')) { return $d.FullName }
        }
    }
    if (Test-Path $standard) { return $standard }
    Fail "Pasta do usuário do The Sims 4 não encontrada em '$ea'. Abra o jogo uma vez e tente de novo."
}

function Test-OurWinmm([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    # 'ts4mp' = this same mod before it was renamed to CTS4P (0.4.x): ours, never a foreign dll.
    return (Get-Item $path).VersionInfo.ProductName -in @('cts4p', 'ts4mp')
}

function Get-Sha256([string]$path) { return (Get-FileHash -Algorithm SHA256 -Path $path).Hash }

function Assert-GameClosed {
    $running = Get-Process -Name TS4_x64, TS4_DX9_x64 -ErrorAction SilentlyContinue
    if ($running) { Fail 'Feche o The Sims 4 antes de instalar ou desinstalar.' }
}

function Set-IniValue([string]$file, [string]$key, [string]$value) {
    $lines = @(Get-Content -Path $file)
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match "^\s*$([regex]::Escape($key))\s*=") { $lines[$i] = "$key = $value"; $found = $true }
    }
    if (-not $found) { $lines += "$key = $value" }
    Set-Content -Path $file -Value $lines -Encoding ASCII
}

function Get-IniValue([string]$file, [string]$key) {
    foreach ($line in Get-Content -Path $file) {
        if ($line -match "^\s*$([regex]::Escape($key))\s*=\s*(.*)$") { return $Matches[1].Trim() }
    }
    return $null
}

# ---------------------------------------------------------------------------------------------- install
function Install-Cts4p {
    $dll = Join-Path $Here 'winmm.dll'
    $script = Join-Path $Here 'cts4p.ts4script'
    if (-not (Test-Path $dll) -or -not (Test-Path $script)) { Fail "winmm.dll e cts4p.ts4script precisam estar ao lado do instalador ($Here)." }
    $version = (Get-Item $dll).VersionInfo.ProductVersion

    Say "`n== Instalando CTS4P $version ==" 'Cyan'
    Assert-GameClosed
    $game = Find-GameDir
    $user = Find-UserDir
    $bin = Join-Path $game 'Game\Bin'
    $mods = Join-Path $user 'Mods'
    $modDir = Join-Path $mods 'cts4p'
    Ok "Jogo: $game"
    Ok "Pasta do usuário: $user"

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backup = Join-Path $user "cts4p_backup\$stamp"
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null

    # 1) winmm.dll: back up anything that isn't ours (another mod's proxy) before replacing it.
    #    On updates, keep pointing at the backup made by the FIRST install, or the uninstaller
    #    would forget the other mod's dll.
    $targetDll = Join-Path $bin 'winmm.dll'
    $foreignBackedUp = $false
    $foreignBackup = $null
    # Migration from the pre-rename install (Mods\ts4mp, 0.4.x): carry its config and install manifest
    # over (the manifest remembers another mod's winmm.dll backup), then retire the old folder.
    $legacyDir = Join-Path $mods 'ts4mp'
    if (Test-Path (Join-Path $legacyDir 'ts4mp.ts4script')) {
        Ok 'Instalação antiga encontrada (ts4mp): migrando para CTS4P.'
        foreach ($name in 'config.json', 'install.json') {
            $src = Join-Path $legacyDir $name
            if ((Test-Path $src) -and -not (Test-Path (Join-Path $modDir $name))) { Copy-Item $src (Join-Path $modDir $name) }
        }
        Move-Item $legacyDir (Join-Path $backup 'ts4mp_antigo') -Force
        Ok 'Pasta Mods\ts4mp movida para o backup (suas opções foram mantidas).'
    }
    $oldManifestPath = Join-Path $modDir 'install.json'
    if (Test-Path $oldManifestPath) {
        try {
            $old = Get-Content $oldManifestPath -Raw | ConvertFrom-Json
            if ($old.foreign_winmm_backup -and (Test-Path $old.foreign_winmm_backup)) {
                $foreignBackedUp = $true
                $foreignBackup = $old.foreign_winmm_backup
            }
        } catch { Warn 'install.json anterior ilegível; continuando.' }
    }
    if (Test-Path $targetDll) {
        if (Test-OurWinmm $targetDll) {
            Ok "Atualizando CTS4P $((Get-Item $targetDll).VersionInfo.ProductVersion) -> $version"
            Copy-Item $targetDll (Join-Path $backup 'winmm.dll.cts4p-anterior')
        } else {
            Warn "Já existe uma winmm.dll de OUTRO programa/mod em Game\Bin. Ela vai para o backup e volta ao desinstalar."
            $foreignBackup = Join-Path $backup 'winmm.dll'
            Copy-Item $targetDll $foreignBackup
            $foreignBackedUp = $true
        }
    }
    # 2) previous cts4p files (config, scripts) go to the backup too.
    foreach ($name in 'cts4p.ts4script', 'config.json') {
        $old = Join-Path $modDir $name
        if (Test-Path $old) { Copy-Item $old (Join-Path $backup $name) }
    }

    # 3) S4MP and the old ts4multiplayer conflict with CTS4P (same game hooks).
    $conflicts = @(Get-ChildItem $mods -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(S4MP|s4mp|sims4multiplayer|ts4multiplayer)' -and $_.FullName -notlike "$modDir*" })
    if ($conflicts.Count -gt 0) {
        Warn ("Encontrei outro mod multiplayer que conflita: " + (($conflicts | ForEach-Object Name) -join ', '))
        if (Ask 'Mover esses arquivos para o backup (podem ser restaurados depois)?' $true) {
            $conflictDir = Join-Path $backup 'mods_conflitantes'
            New-Item -ItemType Directory -Force -Path $conflictDir | Out-Null
            foreach ($c in $conflicts) { if (Test-Path $c.FullName) { Move-Item $c.FullName (Join-Path $conflictDir $c.Name) -Force } }
            Ok 'Mods conflitantes movidos para o backup.'
        }
    }

    # 4) copy the files and verify.
    Copy-Item $dll $targetDll -Force
    Copy-Item $script (Join-Path $modDir 'cts4p.ts4script') -Force
    if ((Get-Sha256 $dll) -ne (Get-Sha256 $targetDll)) { Fail 'Falha ao copiar winmm.dll (arquivo diferente após a cópia).' }
    if ((Get-Sha256 $script) -ne (Get-Sha256 (Join-Path $modDir 'cts4p.ts4script'))) { Fail 'Falha ao copiar cts4p.ts4script.' }
    Ok "winmm.dll -> $bin"
    Ok "cts4p.ts4script -> $modDir"

    # 5) script mods must be enabled in Options.ini.
    $options = Join-Path $user 'Options.ini'
    if (Test-Path $options) {
        $scripts = Get-IniValue $options 'scriptmodsenabled'
        $disabled = Get-IniValue $options 'modsdisabled'
        if ($scripts -ne '1' -or $disabled -ne '0') {
            if (Ask 'Os mods de script estão desligados no jogo. Ligar agora?' $true) {
                Copy-Item $options (Join-Path $backup 'Options.ini')
                Set-IniValue $options 'scriptmodsenabled' '1'
                Set-IniValue $options 'modsdisabled' '0'
                Ok 'Mods e mods de script ligados no Options.ini.'
            } else {
                Warn 'Ligue em: Opções do Jogo > Outros > Ativar conteúdo personalizado e mods + Mods de script.'
            }
        } else { Ok 'Mods de script já estavam ligados.' }
    } else {
        Warn 'Options.ini não existe ainda: abra o jogo uma vez e ligue "Mods de script" nas opções.'
    }

    # 6) config.json only on first install (keeps the player's settings on updates).
    $config = Join-Path $modDir 'config.json'
    if (-not (Test-Path $config)) {
        $url = $SignalingUrl
        if (-not $url -and -not $Quiet) {
            Say "`nEndereço do servidor de sinalização (o Cloudflare Worker publicado, ex.: wss://cts4p-signal.SEU-SUBDOMINIO.workers.dev/ws)."
            $url = Read-Host 'Cole aqui, ou Enter para usar só nesta rede (servidor local)'
        }
        if ([string]::IsNullOrWhiteSpace($url)) { $url = $DefaultSignaling }
        $cfg = [ordered]@{ signaling_url = $url.Trim(); ice_servers = @('stun:stun.l.google.com:19302', 'stun:stun1.l.google.com:19302'); nickname = '' }
        [IO.File]::WriteAllText($config, ($cfg | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
        Ok "config.json criado (servidor: $($cfg.signaling_url))"
    } elseif ($SignalingUrl) {
        $cfg = Get-Content $config -Raw | ConvertFrom-Json
        $cfg | Add-Member -NotePropertyName signaling_url -NotePropertyValue $SignalingUrl -Force
        [IO.File]::WriteAllText($config, ($cfg | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
        Ok "config.json: servidor atualizado para $SignalingUrl"
    } else { Ok 'config.json existente mantido.' }

    # 7) manifest used by the uninstaller.
    $manifest = [ordered]@{
        version = $version
        installed_at = (Get-Date).ToString('s')
        game_dir = $game
        user_dir = $user
        backup_dir = $backup
        foreign_winmm_backed_up = $foreignBackedUp
        foreign_winmm_backup = $foreignBackup
        files = @(
            @{ path = $targetDll; sha256 = (Get-Sha256 $targetDll) },
            @{ path = (Join-Path $modDir 'cts4p.ts4script'); sha256 = (Get-Sha256 (Join-Path $modDir 'cts4p.ts4script')) }
        )
    }
    [IO.File]::WriteAllText((Join-Path $modDir 'install.json'), ($manifest | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
    Ok "Backup em: $backup"
    Say "`nPronto! Abra o The Sims 4 e aperte INSERT para abrir o painel do CTS4P." 'Green'
}

# ---------------------------------------------------------------------------------------------- uninstall
function Uninstall-Cts4p {
    Say "`n== Desinstalando CTS4P ==" 'Cyan'
    Assert-GameClosed
    $game = Find-GameDir
    $user = Find-UserDir
    $bin = Join-Path $game 'Game\Bin'
    $modDir = Join-Path $user 'Mods\cts4p'
    $manifestPath = Join-Path $modDir 'install.json'
    $manifest = $null
    if (Test-Path $manifestPath) { $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json }

    $targetDll = Join-Path $bin 'winmm.dll'
    if (Test-OurWinmm $targetDll) {
        Remove-Item $targetDll -Force
        Ok 'winmm.dll do CTS4P removida de Game\Bin.'
    } elseif (Test-Path $targetDll) {
        Warn 'A winmm.dll em Game\Bin não é do cts4p: não foi mexida.'
    }
    # Restore another mod's winmm.dll that we moved aside on install.
    if ($manifest -and $manifest.foreign_winmm_backed_up) {
        $saved = if ($manifest.foreign_winmm_backup) { $manifest.foreign_winmm_backup } else { Join-Path $manifest.backup_dir 'winmm.dll' }
        if ((Test-Path $saved) -and -not (Test-Path $targetDll)) {
            Copy-Item $saved $targetDll
            Ok 'A winmm.dll original (de outro mod) foi restaurada.'
        }
    }
    $script = Join-Path $modDir 'cts4p.ts4script'
    if (Test-Path $script) { Remove-Item $script -Force; Ok 'cts4p.ts4script removido.' }
    if (Test-Path $manifestPath) { Remove-Item $manifestPath -Force }

    if (Ask 'Apagar também config.json e logs do CTS4P (Mods\cts4p)?' $false) {
        Remove-Item $modDir -Recurse -Force
        Ok 'Pasta Mods\cts4p removida.'
    } else {
        Ok 'config.json e logs mantidos em Mods\cts4p.'
    }
    if ($manifest -and (Test-Path (Join-Path $manifest.backup_dir 'mods_conflitantes'))) {
        Say "Mods conflitantes que foram movidos na instalação estão em: $(Join-Path $manifest.backup_dir 'mods_conflitantes')"
    }
    Say "`nCTS4P desinstalado." 'Green'
}

# ---------------------------------------------------------------------------------------------- entry
try {
    switch ($Action) {
        'Install' { Install-Cts4p }
        'Uninstall' { Uninstall-Cts4p }
        default {
            Say '==== CTS4P - Co-op The Sims 4 Players ====' 'Cyan'
            Say '  1) Instalar ou atualizar'
            Say '  2) Desinstalar (restaura o que foi substituído)'
            Say '  3) Sair'
            switch (Read-Host 'Escolha') {
                '1' { Install-Cts4p }
                '2' { Uninstall-Cts4p }
                default { Say 'Nada foi alterado.' }
            }
        }
    }
    exit 0
} catch {
    Say "`nA operação foi interrompida: $($_.Exception.Message)" 'Red'
    exit 1
}

# CTS4P — Fase 3: ponte Python, compatibilidade, save e sincronização de gameplay

Versão 0.3.0 (DLL nativa + `cts4p.ts4script`), contra The Sims 4 1.129.39.1030.

## Arquitetura

```
 PC do HOST                                          PC do CONVIDADO
 ┌──────────────────────────────┐                    ┌──────────────────────────────┐
 │ Simulação Python (a única    │                    │ Simulação local CONGELADA    │
 │ que roda)                    │                    │ (TimeService.update = no-op) │
 │  ├ cliente real (host)       │                    │                              │
 │  └ cliente VIRTUAL p/ cada   │   GAME_MSG         │ omega.send(cliente local,    │
 │    convidado (Sim ativo e    │ ─────────────────► │   msg_id, protobuf)          │
 │    seletor próprios)         │   (espelho da UI)  │  → a UI mostra o mundo do    │
 │                              │                    │    host                      │
 │ executa comandos do convidado│ ◄───────────────── │ comandos da UI encaminhados  │
 │ como o cliente virtual       │   COMMAND          │ (287 comandos Live)          │
 └──────────────┬───────────────┘                    └──────────────┬───────────────┘
        ctypes  │ anéis SPSC em memória (~25 µs ida e volta)        │ ctypes
 ┌──────────────┴───────────────┐   WebRTC P2P       ┌──────────────┴───────────────┐
 │ winmm.dll (núcleo nativo)    │ ◄────────────────► │ winmm.dll                    │
 │ manifesto, save zstd, overlay│                    │                              │
 └──────────────────────────────┘                    └──────────────────────────────┘
```

* **Espelho:** tudo que a simulação do host envia à UI do host (`Client.send_message`) vai também para os convidados: necessidades, humor, filas de interação, dinheiro, relógio, objetos. Ficam de fora as operações do próprio cliente do host (câmera, Sim ativo, seletor).
* **Controle independente:** cada convidado ganha um *cliente virtual* no host. Os comandos dele rodam com esse cliente como `_connection`, então o Sim ativo, o menu de torta, os diálogos e o seletor são dele. As respostas pessoais voltam só para ele (contexto de roteamento sobre `Distributor.add_event`/`add_op`).
* **Entrada e viagens:** o host pausa o relógio, salva num slot dedicado (`Slot_00054d50.save`), envia comprimido (zstd) e só despausa quando todos carregaram esse save. Isso se repete a cada troca de lote do host (viagem, volta do CAS).

## Status

| Item | Status |
|---|---|
| Ponte IPC Python↔DLL (`ctypes` + anéis SPSC, evento de despertar) | ✅ testada no Python 3.7.0 do próprio jogo: mediana de 0,025 ms |
| Comparação de DLCs/pacotes e `.package`/`.ts4script` (fingerprint FNV-1a) + avisos no overlay | ✅ testada entre 2 processos (detecta faltando aqui/lá e versão diferente) |
| Save: compressão zstd, envio em blocos com controle de fluxo, hash, backup e gravação atômica | ✅ 12 MB em 0,22 s, arquivo idêntico byte a byte |
| Mensagens de gameplay e fragmentação de mensagens grandes | ✅ testadas (rede real e Python 3.7) |
| `.ts4script` compilado pelo Python 3.7 do jogo; imports e atributos do jogo verificados contra 1.129 | ✅ 26 módulos; 177 referências, 0 problemas (o verificador pega erros, conferido com controle negativo) |
| Espelho da UI, cliente virtual, encaminhamento de comandos, relógio, necessidades, dinheiro | ⚠ implementado contra o código real do jogo, **não testado dentro do jogo** |
| Viagem entre lotes (ressincronização por save) | ⚠ idem |
| CAS do convidado (aparência enviada e aplicada no host) | ⚠ **experimental** |
| Carregar o save no convidado | ⚠ manual: o save chega no slot "CTS4P - <host>" e o convidado clica em Carregar Jogo. Abrir sozinho exige engenharia reversa do menu principal |

## Descobertas desta fase

* **O jogo não inclui o pacote `ctypes`:** só `_ctypes_x64.pyd`. `import ctypes` falha dentro do The Sims 4. O CTS4P leva o `ctypes` puro do CPython 3.7.0 em `cts4p/vendor` (licença PSF; só dois imports viraram relativos).
* **libdatachannel no Windows não verifica certificado TLS** (corrigido na Fase 2 com patch + lista de CAs do Windows).
* **`DataChannel::send()` retorna `false` quando enfileira**, sem que isso seja erro. Corrigido; antes disso o envio do save abortava.

## Como compilar e testar

```powershell
# DLL + testes
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg"
cmake --preset x64-release; cmake --build --preset x64-release      # em native\

# verificação estática contra o jogo instalado
python tools\check_game_imports.py srcScripts _decompiled_ref "<jogo>\Data\Simulation\Gameplay\base.zip" "<pyc extraídos>"

# .ts4script com o Python 3.7 do jogo
native\build\Release\py37host.exe "<jogo>\Game\Bin" "<jogo>\Data\Simulation\Gameplay\base.zip" "" tools\build_ts4script.py srcScripts dist\cts4p.ts4script

# testes no Python do jogo (do .ts4script)
native\build\Release\py37host.exe "<jogo>\Game\Bin" "<jogo>\Data\Simulation\Gameplay\base.zip" dist\cts4p.ts4script tests\python\test_game_python.py native\build\Release\winmm.dll <pasta temporária>
```

## Comandos no jogo (Ctrl+Shift+C)

* `cts4p.status` — papel, sala, convidados e estado da sincronização.
* `cts4p.resync` — host: reenvia o save; convidado: pede um save novo.

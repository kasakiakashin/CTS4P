# CTS4P — Co-op The Sims 4 Players

Mod que deixa dois ou mais jogadores no **mesmo mundo do The Sims 4**: cada um controla os próprios Sims, o host simula e o convidado vê tudo em tempo real. A conexão é **P2P (WebRTC)** e usa um código de sala `CTS-XXXX`, sem Hamachi, Radmin ou abrir portas. Tem painel próprio dentro do jogo (tecla `INSERT`).

Feito sobre o antigo [ts4multiplayer](https://github.com/dptetc/ts4mp) (Python, 2019) e reescrito para a versão atual do jogo (**1.129.39.1030**, 64-bit, DX11, Python 3.7).

> **Estado:** em desenvolvimento ativo (0.5.0). Até a 0.4.x o projeto se chamava *ts4mp*; o instalador migra essa instalação sozinho. Veja a tabela de [Status](#status): algumas partes já foram validadas dentro do jogo, outras ainda não.

---

## Arquitetura

```
 PC do HOST                                               PC do CONVIDADO
 ┌─────────────────────────────────────┐                  ┌─────────────────────────────────────┐
 │ TS4_x64.exe (render/UI, com DRM)     │                  │ TS4_x64.exe                         │
 │   └ IDXGISwapChain::Present ─► overlay ImGui            │   └ overlay ImGui                    │
 │ Simulation_x64.dll (simulação)       │                  │ Simulation_x64.dll (CONGELADA)       │
 │   └ despachante cliente→sim 0x24380 ─► hook de construção (MinHook, AOB + RTTI)               │
 │ Python 3.7 do jogo                   │                  │ Python 3.7 do jogo                   │
 │   └ cts4p.ts4script                  │  GAME_MSG ──────►│   └ omega.send → UI do convidado     │
 │       cliente virtual por convidado  │ ◄────── COMMAND  │     comandos da UI → host            │
 │            ▲ ctypes (anéis SPSC, ~25 µs)                │            ▲ ctypes                  │
 │ winmm.dll  │ (núcleo nativo C++20)   │ ◄═ WebRTC P2P ═► │ winmm.dll  │                        │
 │   sala/sinalização, manifesto, save zstd, replay        │                                      │
 └─────────────────────────────────────┘                  └─────────────────────────────────────┘
                  ▲ só para se encontrarem: servidor de sinalização (Cloudflare Worker / Python)
```

* **`winmm.dll` (proxy):** o jogo a carrega sozinho da pasta `Game\Bin`. Ela repassa as **181 funções** da `winmm.dll` verdadeira do Windows (mesmos ordinais) e só ativa o CTS4P dentro do `TS4_x64.exe`. O `dinput8.dll` não serviria: o jogo não o importa.
* **Host autoritativo + cliente fino:** só o host simula. Tudo que a simulação dele manda à própria UI (necessidades, humor, filas, dinheiro, relógio) é espelhado aos convidados. Cada convidado tem um **cliente virtual** no host, com Sim ativo e seletor próprios. Os comandos da UI do convidado (**287 comandos Live**, gerados a partir do código do jogo) rodam no host como esse cliente, e as respostas pessoais (menu de torta, diálogos) voltam só para ele.
* **Entrada e viagens:** o host pausa, salva num slot dedicado (`Slot_00054d50.save`, sem tocar nos saves do jogador), comprime com zstd e envia. O convidado carrega esse save.
* **Construção:** toda operação de construção chega ao `Simulation_x64.dll` como um quadro `RequestBuildBuyOp` (`0x3a0f613c`). O hook copia o quadro, envia e o outro jogo o reaplica pelo mesmo despachante, na mesma thread. Um **portão** só libera isso com os dois jogos no mesmo lote, e o **Lot State Hash** devolve o lote do host se algo divergir.

Detalhes: [docs/FASE1_RELATORIO.md](docs/FASE1_RELATORIO.md) (engenharia reversa), [docs/FASE3.md](docs/FASE3.md) (sincronização), [docs/FASE4.md](docs/FASE4.md) (construção), [docs/TESTES_NO_JOGO.md](docs/TESTES_NO_JOGO.md) (testes no jogo real e bugs corrigidos).

## Status

| Recurso | Estado |
|---|---|
| Proxy `winmm.dll`, overlay DX11 (INSERT), loopback 127.0.0.1 | ✅ validado no jogo real |
| Salas P2P (WebRTC, código `CTS-XXXX`), ping, sinalização com TLS verificado | ✅ validado (jogo real ↔ convidado de teste) |
| Comparação de DLCs/mods entre os jogadores | ✅ validado no jogo real |
| Envio do save do host (zstd, hash, backup, gravação atômica) | ✅ validado no jogo real (jogo como convidado) |
| **Criar juntos**: cada um cria o seu Sim e envia a família da Biblioteca para os outros |  ✅ validado no jogo real (envio e recebimento) |
| Sincronização automática do save quando o host salva | ⚠ implementado, falta testar com 2 jogadores |
| Hook do Modo Construção (AOB + validação + RTTI) | ✅ validado no jogo real |
| Reaplicar uma operação de construção (parede) pelo caminho do CTS4P | ✅ validado no jogo real (teste local) |
| Espelho de gameplay, cliente virtual, comandos do convidado, relógio, dinheiro | ⚠ implementado, falta testar dentro de um lote com 2 jogadores |
| Replicação de construção entre PCs + portão + Lot State Hash | ⚠ caminho de rede testado; falta testar em 2 PCs |
| CAS do convidado | ⚠ experimental |
| Ctrl+Z / Ctrl+Y | ❌ usam outra mensagem (`SendBuildBuyEvent`); por ora o Lot State Hash corrige |
| Ghost Cursor 3D | ❌ por ora só coordenadas no painel |
| Carregar o save no convidado sem clicar | ❌ manual: menu principal > Carregar Jogo |

## Instalar (jogadores)

1. Baixe o `cts4p-<versão>.zip` e extraia.
2. Feche o jogo e dê dois cliques em **`instalar.bat`** > **1 (Instalar)**.
   O instalador acha o jogo, faz backup do que substituir, liga os mods de script, move mods multiplayer conflitantes (S4MP) para o backup e cria o `config.json`.
3. Abra o jogo e aperte `INSERT`.

Desinstalar: `instalar.bat` > **2**. Remove só os arquivos do CTS4P e restaura o que tinha sido substituído (inclusive a `winmm.dll` de outro mod).

## Jogar

1. **Servidor de sinalização** (uma vez):
   * **Internet:** publique o Worker gratuito, `cd signaling\cloudflare && npx wrangler login && npx wrangler deploy` ([guia](signaling/cloudflare/README.md)), e use `wss://…workers.dev/ws` em `Documentos\Electronic Arts\The Sims 4\Mods\cts4p\config.json` nos dois PCs.
   * **Mesma rede:** `python signaling\python\server.py` num PC e `ws://<IP>:8787/ws` nos dois.
2. **Host:** entre no lote > `INSERT` > **Conexão** > **Criar sala** > passe o código.
3. **Convidado:** no menu principal > `INSERT` > digite o código > **Entrar**. Quando o save chegar: **Carregar Jogo > "CTS4P - <host>"**.

Console (`Ctrl+Shift+C`): `cts4p.status`, `cts4p.resync`.

## Compilar (desenvolvedores)

Requisitos: Windows 10/11 x64, **Visual Studio 2022** (Desenvolvimento para desktop com C++), CMake ≥ 3.25 e **The Sims 4 instalado** (o `.ts4script` é compilado com o Python 3.7 do próprio jogo). Para `-FullTests`, também Python 3 no PATH.

```powershell
.\build_all.bat              # compila (Release x64, /W4 /WX), testa e gera dist\cts4p-<versão>.zip
.\build_all.bat -FullTests   # + rede entre processos (sala, save de 12 MB, replicação de construção), overlay
.\build_all.bat -SkipTests   # só compila e empacota
.\build_all.bat -Install     # instala neste PC usando o próprio instalador do pacote
```

A primeira compilação baixa e compila as dependências via vcpkg (~10 min): MinHook, Dear ImGui, libdatachannel (com patch para verificar TLS no Windows, em `native/ports/`), OpenSSL, zstd e nlohmann-json, todas estáticas. A saída é uma única `winmm.dll`.

### Testes

| Teste | O que cobre |
|---|---|
| `test_proxy` | as 181 funções vão para a `winmm.dll` do System32; o mod fica passivo fora do jogo |
| `test_net loopback` | WebRTC completo dentro de um processo (ICE, DTLS, canais, handshake, ping, chat) |
| `test_net signaling` / `host`+`join` | servidor de sinalização; sala real entre 2 processos |
| `test_net p3host`+`p3join` | comparação de mods, save de 12 MB byte a byte, mensagens de gameplay |
| `test_net p4host`+`p4join` | quadro real de construção atravessa a rede; quadros inválidos e portão fechado são recusados |
| `test_build` | o localizador acha o despachante no `Simulation_x64.dll` **real** (AOB único + validação + RTTI) |
| `test_overlay` | janela D3D11: hook do Present, INSERT, ImGui e screenshot |
| `tests/python/test_game_python.py` | roda no **Python 3.7 do jogo** (via `py37host`): ponte `ctypes`, quadros e fragmentação |
| `tests/test_installer.ps1` | instalar / atualizar / desinstalar / restaurar em pastas falsas |
| `tools/check_game_imports.py` | verifica cada import e atributo do jogo usado pelos scripts contra o código descompilado da versão instalada |
| `tests/fake_guest.py` / `fake_host.py` | jogo real ↔ jogador de teste, comandados pelo canal `debug_control` (desligado por padrão) |

## Estrutura

```
native/                 núcleo C++20 (winmm.dll): proxy, hooks, overlay, rede, ponte IPC, save, construção
  src/proxy/            181 encaminhamentos gerados de System32\winmm.dll
  src/hook/             MinHook, scanner AOB, limites de função (.pdata), resolvedor RTTI
  src/build/            observador/replicador do Modo Construção
  src/net/ src/sync/    sessão WebRTC, protocolo, manifesto, save zstd
  src/ui/ src/render/   overlay ImGui e hooks DX11
  tools/py37host/       roda scripts no Python 3.7 do jogo (compilar .pyc, testes)
srcScripts/cts4p/       scripts do jogo (viram cts4p.ts4script)
  sync/                 host, convidado, roteamento, comandos (tabela gerada), save, CAS, Lot State Hash
  vendor/ctypes/        ctypes do CPython 3.7 (o jogo só traz o _ctypes)
signaling/              servidor de sinalização: Cloudflare Worker e alternativa em Python
installer/              instalar.bat / instalar.ps1 / LEIA-ME.txt
tools/                  build_all, gerador da tabela de comandos, verificador de API, engenharia reversa
tests/                  testes de Python, instalador e jogador de teste
docs/                   relatórios de cada fase
```

## Problemas

* **Log:** `Documentos\Electronic Arts\The Sims 4\Mods\cts4p\cts4p_native.log` (inclui os logs do Python).
* **Painel > Diagnóstico:** versões, hooks, scripts carregados e mods detectados.
* **"Scripts Python não conectaram":** os mods de script estão desligados, ou o `cts4p.ts4script` não está em `Mods`.
* **"Assinatura não encontrada" na aba Construção:** o jogo foi atualizado. O resto do mod continua funcionando e a construção se desliga sozinha até uma nova versão do CTS4P.
* **Conexão não abre entre redes diferentes:** NAT restritivo/CGNAT. Configure um servidor TURN em `ice_servers` no `config.json`.

## Licença e créditos

GPL-3.0 (herdada do [ts4multiplayer](https://github.com/dptetc/ts4mp) de 2019, em que o CTS4P se baseia; veja [LICENSE](LICENSE)). O `ctypes` em `srcScripts/cts4p/vendor` é do CPython 3.7.0 (licença PSF). The Sims é marca da Electronic Arts; este projeto não é afiliado à EA. Use só offline e nunca envie saves modificados para a Galeria.

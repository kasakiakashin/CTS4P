# ts4mp — Fase 1: Relatório de Engenharia Reversa & Auditoria


> **Nota:** a partir da 0.5.0 o projeto se chama **CTS4P** (Co-op The Sims 4 Players). Este relatório descreve o ts4mp original (2019) e o plano inicial, por isso mantém os nomes antigos.

> Gerado em 2026-11-22 contra **The Sims 4 1.129.39.1030** (Steam, pt_BR).
> Tudo aqui foi extraído da instalação local — nenhuma assinatura foi presumida.
> Referência descompilada: `_decompiled_ref/` (ignorada no git; contém código da EA, nunca commitar).

---

## 1. Ambiente detectado

| Item | Valor |
|---|---|
| Instalação do jogo | `<biblioteca Steam>\steamapps\common\The Sims 4\` (via `HKLM\SOFTWARE\Maxis\The Sims 4\Install Dir`) |
| Pasta do usuário | `%USERPROFILE%\Documents\Electronic Arts\The Sims 4\` |
| Versão | `gameversion = 1.129.39.1030`, `codeversion = 1.200.000.721` |
| Python do jogo | CPython **3.7** x64 (`python37_x64.dll`, magic `3394`) |
| Executável | `Game\Bin\TS4_x64.exe` (45 MB, DX11) + `TS4_DX9_x64.exe` (legado) |
| Scripts | `Data\Simulation\Gameplay\{base,core,simulation}.zip` + `Game\Bin\Python\generated.zip` (protobufs) |
| Script mods | `scriptmodsenabled = 1`, `modsdisabled = 0` |
| **Mod já instalado** | **`Mods\S4MP_release.ts4script` + `s4mpconfig` + `S4MP_logs`** ⚠ |

**Ferramentas de build**

| Ferramenta | Status |
|---|---|
| CMake | 4.4.4 ✅ |
| Visual Studio 2022 Community | 17.14.44, MSVC **19.44** (toolset 14.44.35207) ✅ |
| Windows SDK | 10.0.26100.0 ✅ |
| vcpkg | embutido no VS (`VC\vcpkg\vcpkg.exe`) ✅ |
| Ninja | embutido no VS ✅ |
| Python host | 3.12.10 e 3.14 (não há 3.7 — compilar `.pyc` exige 3.7, ver §7.5) |
| Git | 2.56 ✅ |
| Smoke test | DLL C++20 x64 (`std::format`, `std::span`) compilada em Release ✅ |

**Python embutido no jogo** — disponíveis: `_socket`, `select`, `selectors`, `threading`, `queue`, `struct`, `pickle`, `json`, **`_ctypes`**.
Ausentes: `asyncio`, `_ssl` (sem TLS nativo no Python).

---

## 2. Mapa do repositório (`dptetc/ts4mp`, último commit 2019-03-19)

```
Setup.ps1                     wizard: grava server.txt/client.txt e reescreve multiplayer_*.py a partir de *.tmp
ts4mpChat.package             tuning da interação "simmy_chatdialog" (affordance 15046892415517235728)
ts4mpPlumbbob.package         objeto plumbob do jogador remoto (definição 9716920233444718179)
srcPackages/ts4mpPlumbbob/    fontes desempacotadas do plumbob (MODL/MLOD/OBJD/COBJ/STBL/…)
srcScripts/ts4mp/
  configs/server_config.py    HOST/PORT (ngrok) + MULTIPLAYER_MOD_ENABLED
  core/mp.py                  comandos mp.host / mp.connect; ciclo de vida
  core/multiplayer_server.py  socket TCP 9999, threads listen/send/heartbeat
  core/multiplayer_client.py  socket TCP cliente
  core/networking.py          framing: pickle + tamanho de 8 bytes
  core/mp_sync.py             filas incoming/outgoing; parser de comandos por string; client_sync/server_sync
  core/mp_commands.py         comandos debug; transferência de zoneObjects-*.sav (arquitetura)
  core/csn.py                 notificações + chat (Scumbumbo)
  core/pending_client_commands.py  roteia resposta de pie menu ao cliente certo
  core/overrides/
    mp_essential_overrides.py  send_message, on_tick, TimeService.update, wrapper de comandos
    mp_overrides.py            Client.on_add/on_remove, cliente fantasma 1000, push_speed, dialogs
    system_distributor.py      SystemDistributor: fan-out do Distributor para N clientes
    mp_buildbuy_overrides.py   vazio (só imports) — Build Mode nunca foi implementado
  core/injections/affordances.py  injeta a interação de chat em todo Sim
  debug/log.py, debug/reload.py
  utils/native/{injector,decorator,undecorated}.py  monkey-patch + libs de terceiros
```

### 2.1 Como a arquitetura de 2019 funciona (é engenhosa e vale preservar o conceito)

O TS4 já é **cliente/servidor internamente**: o "servidor" é a simulação Python + `Simulation_x64.dll`; o "cliente" é o `TS4_x64.exe` (render/UI). Eles conversam via `omega` com mensagens protobuf (`Distributor` → `MSG_OBJECTS_VIEW_UPDATE`, etc.).

O ts4mp explora isso:

1. **Host** cria um **cliente fantasma** (`Client` id `1000`, conta `865431`) em `Client.on_add`.
2. `SystemDistributor` substitui o `Distributor` único por um `Distributor` **por cliente**, fazendo fan-out das ops.
3. `Client.send_message` é sobrescrito: mensagens para o id 1000 viram `ProtocolBufferMessage` e vão pela rede.
4. **Cliente remoto** congela a própria simulação (`TimeService.update` vira no-op) e só repassa ao seu `TS4_x64.exe` os protobufs recebidos (`omega.send(client.id, msg_id, bytes)`). É um *thin client* renderizando o mundo do host.
5. Comandos de UI do cliente (`interactions.choices`, `clock.setspeed`, …) são re-registrados com um wrapper que os serializa como **string** e envia ao host, que faz parse e executa com `_connection = 1000`.

Limitação estrutural: a arquitetura do lote (paredes/pisos/terreno) vive no **C++ do cliente** (vem do save local), não nos protobufs — por isso o original tentou copiar `saves/scratch/zoneObjects-*.sav` à mão.

---

## 3. Quebras de compatibilidade (2019 → 1.129.39)

Verificadas contra o código descompilado. "OK" = assinatura idêntica.

### 3.1 APIs que mudaram

| # | Local no ts4mp | 2019 | Atual (1.129) | Impacto |
|---|---|---|---|---|
| B1 | `mp_overrides.send_selectable_sims_update` (cópia da EA) | `sim_info.is_enabled_in_skewer` (property) | **`sim_info.get_is_enabled_in_skewer()`** (método; property removida) | Bound method sempre truthy → **todos os Sims marcados selecionáveis**. Corrigir ou, melhor, não copiar a função (wrappear a original). |
| B2 | `mp_sync` forward de `ui_dialog_pick_result` | `(dialog_id, *choices)` | **`(dialog_id, ingredient_check, prepped_ingredient_check, *choices)`** | Parser posicional por string desalinha args → pickers quebram. |
| B3 | Comandos não encaminhados (novos desde 2019) | — | `ui.dialog.pick_result_definiton_and_count`, `ui.dialog.pick_result_with_recipe_id`, e muitos outros em 68 módulos `server_commands/*` | Ações do cliente executadas localmente no thin client (sem efeito) ou perdidas. |
| B4 | `interactions.choices` / `has_choices` | args básicos | 13–15 parâmetros (`pick_type, x,y,z, lot_id, level, control, alt, shift, reference_id, referred_object_id, preferred_object_id, is_routable`) | Parser por string frágil; `_connection` sobrescrito pelo índice `-1` (errado se houver kwargs). |
| B5 | `mp_commands.load_zone` | `persistence_module.run_persistence_operation(op, pb, 0, None)` | mesmo nome, mas **definido condicionalmente** (stub quando nativo indisponível); `kPersistenceOpLoadZoneObjects = 3` | Funciona, mas recarregar objetos de zona por fora do fluxo de load é inseguro na versão atual (zone spin-up mudou muito). |
| B6 | `Setup.ps1` + `*.tmp` + `server.txt/client.txt` | gera código-fonte | — | Incompatível com distribuição `.ts4script` (zip compilado). Remover. |

### 3.2 APIs que continuam válidas (OK)

`clock.GameClock.push_speed(speed, source, validity_check, reason, immediate)` · `_update_speed(immediate)` · `GameSpeedChangeSource.GAMEPLAY` ·
`TimeService.update(self, time_slice=True)` (corpo idêntico ao copiado) · `Client.__init__(session_id, account, household_id)` · `Client.send_message(msg_id, msg)` (agora loga se inativo) ·
`Client.on_add/on_remove` (mesma estrutura) · `ClientManager.create_client(client_id, account, household_id)` / `get_first_client` / `get_first_client_id` / `get_client_by_account` ·
`Account(account_id, persona_name)` · `Distributor` (`journal`, `_pending_creates`, `client`, `events`, `add_client`, `_add_ops_for_client_connect`, `remove_client`, `add_op`, `add_event`, `process`, `_send_view_updates_for_client(client, all_ops)`) ·
`Journal.add(obj, op, ignore_deferral=False)` · `_build_journal_seed(op, obj, manager_id)` · `DistributorService.start` · `animation.arb.set_tag_functions` ·
`UiDialogBase.distribute_dialog(dialog_type, dialog_msg, immediate=False)` · `add_listener` · `objects.system.create_object(definition_or_id, …)` · `Sim.on_add` ·
`world.travel_service.travel_sim_to_zone(sim_id, zone_id)` · `gsi_handlers.distributor_handlers.archive_operation/archiver/sim_archiver` ·
`RequiredTargetParam(target_id).target_id` · todos os nomes de comando (`clock.*`, `interactions.*`, `sims.set_active`, `ui.dialog.*`, `lighting.*`, `careers.*`) ·
`Operation.SELECTABLE_SIMS_UPDATE = 120`, `Operation.SELECT_CAREER_UI = 151`, `MSG_OBJECT_IS_INTERACTABLE = 5`, `MSG_PIE_MENU_CREATE = 6`, `MSG_OBJECTS_VIEW_UPDATE = 11`.

### 3.3 Defeitos do código de 2019 (independentes de versão)

| # | Onde | Problema | Severidade |
|---|---|---|---|
| D1 | `mp_essential_overrides` | **Substitui** `sims4.core_services.on_tick` em vez de wrappear → mata `directory_watcher` e GSI. | Alta |
| D2 | `wrapper_client` | `time.sleep(0.2)` **na thread principal do jogo** a cada comando → travada de 200 ms por clique. | Alta |
| D3 | `networking.py` | `pickle.loads` de dados da rede → **execução remota de código** por qualquer peer. | **Crítica (segurança)** |
| D4 | `networking.py` | Tamanho do frame via `sys.getsizeof` (inclui overhead do objeto) e `recv(8)` sem loop → frames corrompidos sob carga. | Alta |
| D5 | `send_loop` (ambos) | `for x in lista: lista.remove(x)` → pula metade das mensagens. | Alta |
| D6 | `multiplayer_client.send_loop` | Loop sem sleep segurando o lock → 100% de CPU. | Média |
| D7 | `heartbeat_loop` | Adiciona à fila sem lock; threads não-daemon (jogo pode não fechar). | Média |
| D8 | Servidor | `bind("localhost", 9999)` → só aceita conexões locais (exigia ngrok). | Alta |
| D9 | I/O em disco | `ArbritraryFileMessage` grava em `saves/scratch/zoneObjects-*.sav`; logs em arquivo por linha. | Média |
| D10 | `.gitignore` | Contém marcadores de conflito de merge e ignora `*.txt`/`*.xml`. | Baixa |
| D11 | Escopo | Só 1 cliente; cliente fantasma fixo `1000`; sem handshake/versão. | — |

---

## 4. Pontos de entrada (hooks) mapeados na versão atual

### 4.1 Objetos / Modo Compra (`build_buy.py`, chamados pelo C++)

| Callback | Assinatura exata |
|---|---|
| Início/fim de sessão BB | `c_api_buildbuy_session_begin(zone_id: int, account_id: int)` / `c_api_buildbuy_session_end(zone_id, account_id, zone_data_change)` |
| Mover objeto | `c_api_set_object_location(zone_id, obj_id, routing_surface, transform)` |
| Mover c/ parent/slot | `c_api_set_object_location_ex(zone_id, obj_id, routing_surface, transform, parent_id, parent_type_info, slot_hash)` |
| Inventário da família | `c_api_house_inv_obj_added(zone_id, household_id, obj_id, obj_def_id)` / `c_api_house_inv_obj_removed(...)` |
| Dados de save do objeto | `c_api_buildbuy_get_save_object_data(zone_id: int, obj_id: int)` |
| Dinheiro | `c_api_modify_household_funds(amount: int, household_id: int, reason, zone_id: int)` |
| Callbacks Python | `register_build_buy_enter_callback(cb)` / `register_build_buy_exit_callback(cb)` |
| Criação Python | `objects.system.create_object(definition_or_id, obj_id=0, init=None, post_add=None, loc_type=None, …)` |

Swatch/rotação/escala chegam ao Python via `c_api_set_object_location*` (transform) e pelas ops do `distributor` do objeto; a criação/remoção passa pelo `ObjectManager` (`objects/object_manager.py`). Detalhar na Fase 4.

### 4.2 Lote / Construção (`build_buy.py`)

`c_api_wall_contour_update(zone_id, wall_type)` · `c_api_foundation_and_level_height_update(zone_id)` · `c_api_navmesh_update(zone_id)` ·
`c_api_on_apply_blueprint_lot_begin/end(zone_id)` · `c_api_on_lot_clearing_begin/end(zone_id)` · `c_api_buildbuy_zones_changed(...)` ·
`c_api_buildbuy_dynamic_areas_changed(...)` · `c_api_buildbuy_lot_traits_changed(zone_id)` · `c_api_buildbuy_venue_type_changed(zone_id)`.

➡ **São notificações "algo mudou", sem o conteúdo da mudança.** Não há API Python para criar parede/telhado/escada/terreno.

### 4.3 Sims, interações, UI

Comandos (todos `CommandType.Live`): `interactions.{has_choices,choices,phone_choices,select,cancel,cancel_si,push}`, `sims.set_active(sim_id)`, `ui.dialog.{respond,pick_result,text_input,…}`, `careers.*`, `lighting.set_color_and_intensity`. Dispatcher: `sims4.commands.Command` → `invoke_command(*args, _session_id=0, **kw)` → `parse_args(full_arg_spec, args, _session_id)` (`core/sims4/commands.py:7246`).
➡ Ponto ideal de encaminhamento: capturar os **args crus (strings) antes do `parse_args`**, eliminando o parser por regex de 2019.

### 4.4 Tempo

`GameClock.push_speed/pop_speed/set_clock_speed(speed, source, reason, immediate)` · `_sync_clock_and_broadcast_gameclock(immediate)` · `set_game_time(h, m, s)` · comandos `clock.setspeed(speed='one')`, `clock.pause/unpause`, `clock.toggle_pause_unpause`. Simulação: `TimeService.update(time_slice=True)`.

### 4.5 Zona, carregamento e viagem

`Zone.start_services(gameplay_zone_data, save_slot_data)` · `Zone.update(absolute_ticks)` · `on_objects_loaded()` · `on_households_and_sim_infos_loaded()` · `on_loading_screen_animation_finished()` · `on_active_lot_clearing_begin/end()` · `save_zone(save_slot_data=None)` · `load_zone()` · `on_teardown(client)` · `travel_sim_to_zone(sim_id, zone_id)` / `travel_sims_to_zone(sim_ids, zone_id)` · `services.stop_global_services()`.

---

## 5. Descobertas nativas (o que muda o plano)

### 5.1 Dois binários com papéis distintos

| | `TS4_x64.exe` (cliente) | `Simulation_x64.dll` (servidor) |
|---|---|---|
| Papel | Render DX11, UI, ferramentas de construção (`WallTool`) | Python host, `_buildbuy`, `_omega`, `_lot`, persistência, estado do lote, Undo/Redo |
| Proteção | **Seção `.ooa` (DRM EA)**; import table oculta (só importa `Core/Activation64.dll`) | **Sem DRM**, import table normal, `.reloc` presente |
| RTTI | não visível (empacotado) | **2.830 classes com RTTI** |
| ASLR | `DYNAMIC_BASE` desligado | relocável |

### 5.2 Proxy DLL: **`winmm.dll`, não `dinput8.dll`**

O exe referencia `winmm.dll` (`timeGetTime`, `timeBeginPeriod`), `version.dll`, `dsound.dll`, `xinput1_x.dll`, `d3d11.dll`, `dxgi.dll`. **`dinput8.dll` não é referenciado** → um proxy dinput8 nunca seria carregado. `Simulation_x64.dll` também importa `WINMM.dll` estaticamente → o proxy é carregado **antes** do Python subir. `winmm.dll` não é KnownDLL, então a busca pela pasta do exe funciona.

### 5.3 O pipeline de construção passa por mensagens — é aqui que se faz o hook

Encontrado em `Simulation_x64.dll` (RTTI + strings + `Consts.proto`):

- `EA::Sims4::PkgRequestBuildBuyOpMsg` + string `RequestBuildBuyOp` — **toda operação de construção é um pacote enviado do exe para a simulação**.
- `Consts.proto`: `EVENT_BB_OP = 706`, `EVENT_BB_OP_FAIL = 719`, `EVENT_BB_LOT_DATA_UPDATE = 708`, `EVENT_BB_UNDOREDO_STATE_CHANGE = 728`, `EVENT_BB_LOT_CLEAR = 723`, `EVENT_BB_LOT_UPDATE_FEATURE_LAYER = 731`.
- `BuildBuy.proto`: `BBOpFailed{error_code, op_id}`, `BBUndoRedoState{canUndo, canRedo}`, `ApplyBlueprintData{blueprint_data: bytes, …}`.
- Classes: `BuildBuy::LotOperations` / `ILotOperations`, `ObjectOperations`, `UndoRedoStack` / `IRequestOp`, `SerializedUndoRedoStack`, `UndoRedoManager::ServerOpRequester`, `BuildBuySessionManager`, `ClearLotCommand`, `FloorTesselator`, `Terrain::TerrainBuilder`, `CTProductWall/Roof/Stair/Fence/Foundation/PoolTrim/HalfWallTrim`.
- Outros pacotes do mesmo transporte: `PkgExecuteCmdMsg`, `PkgExecuteCmdWithArgsMsg`, `PkgRequestClockSyncMsg`, `PkgOmegaPDUMsg`.

**Conclusão:** paredes, telhados, escadas, terreno, piscinas, cercas **e Undo/Redo** viajam como `PkgRequestBuildBuyOpMsg` pelo canal `omega` em memória e são aplicados por `LotOperations` dentro da `Simulation_x64.dll`. Interceptar **o despachante desse pacote na DLL** (sem DRM, com RTTI) captura o comando arquitetônico **serializado** — basta repassar os bytes e reinjetá-los no peer. Não é necessário enganchar as ferramentas de UI no exe protegido.

### 5.4 Protobufs reconstruídos

57 arquivos `.proto` reconstruídos em `_decompiled_ref/proto/` (ex.: `DistributorOps.proto` com 248 mensagens, `FileSerialization.proto`, `Consts.proto`). Servem de fonte de verdade para o protocolo de rede das fases seguintes.

---

## 6. Plano técnico revisado (Fases 2–5)

### Correções ao plano original (baseadas em evidência)

1. **Proxy = `winmm.dll`** (§5.2).
2. **Hooks de construção na `Simulation_x64.dll`**, não no `TS4_x64.exe`. O exe só é tocado indiretamente (Present via vtable do `IDXGISwapChain`, que vive no `dxgi.dll`).
3. **Resolução de alvos: RTTI primeiro, AOB depois.** Para métodos virtuais (`LotOperations`, `UndoRedoStack`, handlers de `Pkg*Msg`), achar o `CompleteObjectLocator` pelo nome RTTI → vtable → slot é **mais resiliente a patches** que AOB. AOB com wildcards para funções não-virtuais, sempre com validação (≥2 âncoras) e fallback "desativa feature" em vez de crash.
4. **IPC Python↔C++ em processo**, não Named Pipe: a DLL já está no processo do jogo; o Python a acessa com `ctypes` (C ABI exportada) + **ring buffer SPSC lock-free** em memória compartilhada do próprio processo. Latência de microssegundos, sem syscalls. Named Pipe fica só para ferramentas externas (debug/console).
5. **Nada de `pickle`.** Envelope binário próprio (cabeçalho fixo + protobuf para payloads de jogo, que já são protobuf), assinado por sessão.

### Fase 2 — Core nativo (C++20)

```
native/
  CMakeLists.txt              C++20, /W4 /permissive-, x64, vcpkg manifest
  vcpkg.json                  minhook, imgui[dx11-binding,win32-binding], libdatachannel, zstd, spdlog, nlohmann-json
  src/proxy/                  winmm.def + forwarders gerados (exports da System32\winmm.dll)
  src/core/                   bootstrap, log, config, thread do worker
  src/hook/                   wrapper MinHook, RTTI resolver, AOB scanner (padrões "48 8B ?? ?? 89")
  src/render/                 Present hook (D3D11 dummy swapchain → vtable[8]), ImGui overlay (INSERT)
  src/net/                    sala TS4-XXXX, sinalização WebSocket, ICE/STUN, DataChannels (reliable + unreliable)
  src/ipc/                    ring buffer SPSC + C API para ctypes
  tools/sigdump/              utilitário: valida todas as assinaturas contra o binário atual
```

Validação de saída: build Release limpo; DLL carrega com o jogo, menu principal abre, overlay aparece, sem regressão de FPS; `sigdump` verde.

**Decisão pendente:** a sinalização WebRTC precisa de um servidor público (WebSocket leve). Opções: Cloudflare Worker (grátis), um VPS seu, ou modo "copiar/colar oferta SDP" sem servidor (fallback). STUN público (Google) para NAT comum; **TURN** é necessário para NAT simétrico/CGNAT — custa banda.

### Fase 3 — Handshake, save e gameplay

- Handshake: versão do mod, versão do jogo, lista de packs (`sims4.common` / `Pack` enum em `Consts.proto`), hash de cada `.package`/`.ts4script` em `Mods/`.
- Save: host salva (`persistence_commands`), comprime com **zstd**, envia em chunks pelo DataChannel confiável; cliente grava em slot dedicado e carrega.
- Python: manter o conceito *host autoritativo + thin client + SystemDistributor*, reescrito: sem threads de rede em Python, sem disco, encaminhamento de comandos por **args crus** (§4.3), N clientes (ids dinâmicos em vez de `1000`), wrappers em vez de cópias de funções da EA (corrige B1, D1).
- `.ts4script` compilado com Python 3.7 (ver §7.5).

### Fase 4 — Compra & Construção

- Compra: replicar via ops do distributor (já fluem para o thin client) + hooks `c_api_set_object_location*`, `house_inv_*`, criação/remoção no `ObjectManager`.
- Construção: hook no despachante de `PkgRequestBuildBuyOpMsg` na `Simulation_x64.dll` → bytes do op → **sequenciados pelo host** (ordem global, inclusive Undo/Redo) → reinjetados em cada peer pelo mesmo despachante. Lockstep determinístico, latência = 1 RTT em build mode.
- **Lot State Hash**: hash periódico do estado do lote (candidato: serialização usada em `ApplyBlueprintData.blueprint_data` / chunk do lote no save) → divergência ⇒ host envia snapshot do lote e o cliente reaplica (fluxo `c_api_on_apply_blueprint_lot_begin/end`).
- Ghost cursor: posição de pick/cursor do peer projetada no overlay ImGui (matriz view-proj obtida do render).
- **Esta é a fase de maior risco**: o formato binário de `PkgRequestBuildBuyOpMsg` e o caminho de reinjeção precisam de RE com disassembler (ver §7.4). Primeiro entregável: DLL "observadora" que só registra ops com timestamp, para mapear o formato.

### Fase 5 — Build & instalação

`build_all.ps1`: CMake Release → compila `.pyc` 3.7 → empacota `.ts4script` → valida `.package` → instala (`winmm.dll` em `Game\Bin`, scripts em `Mods\ts4mp\`) com backup e `-Uninstall`. `README.md` técnico.

---

## 7. Riscos, pré-requisitos e decisões

1. **S4MP instalado** em `Mods/` sobrescreve os mesmos pontos (`Client`, `Distributor`). Precisa ser movido para fora de `Mods/` durante o desenvolvimento.
2. **Patches mensais**: hooks por RTTI + AOB com auto-validação; se uma assinatura falhar, a feature desliga e o overlay avisa — o jogo não crasha.
3. **DRM (`.ooa`)**: não patchamos código do exe; só vtable do DXGI e código da `Simulation_x64.dll`. Uso offline. Nunca enviar saves modificados para a Galeria.
4. **Disassembler para a Fase 4**: precisamos de Ghidra (requer JDK 21) ou IDA Free, e x64dbg para depurar. Instalação requer sua aprovação.
5. **Compilar `.pyc` 3.7**: não há Python 3.7 no sistema. Opções: (a) instalar Python 3.7.9 x64 oficial, (b) distribuir `.py` dentro do `.ts4script` (o jogo aceita fonte e compila), (c) usar o próprio jogo. Recomendo (a).
6. **Servidor de sinalização** público (Fase 2) — decisão sua.
7. **Pacotes de 2019** (`ts4mpChat`, `ts4mpPlumbbob`): o schema de tuning de interação mudou desde 2019; validar com um editor de `.package` na Fase 3.

## 8. Artefatos desta fase

- `_decompiled_ref/core/` (121), `_decompiled_ref/generated/` (58), `_decompiled_ref/simulation/` (2.725, em andamento), `_decompiled_ref/simulation_priority/` (36 módulos auditados), `_decompiled_ref/proto/` (57 `.proto`).
- Relatório de qualidade da descompilação: `_decompiled_ref/*/_decompile_report.tsv` (ok / partial / timeout).
- `.gitignore`: adicionado `_decompiled_ref/`.

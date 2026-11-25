# CTS4P — Fase 4: Modo Construção

Versão 0.4.1, contra The Sims 4 1.129.39.1030.

## Atualização 0.4.1: formato decodificado com capturas reais + replicação experimental

Capturas do seu jogo (41 mensagens, 21 `RequestBuildBuyOp`) revelaram o layout do *stream*:

| Campo do stream | Conteúdo |
|---|---|
| `+0x20` / `+0x28` (u32) | tamanho total do quadro |
| `+0x30` | ponteiro para o quadro |
| quadro | `u32 msg_id` (0x3a0f613c) · `u32 canal` (=a2, 3) · `u32 payload_len` · `payload` (43–462 bytes) |

As 21 operações capturadas seguem o layout sem exceção. As outras mensagens (`SendBuildBuyEvent` etc.) usam um buffer diferente e não são replicadas.

**Replicação (experimental):**
1. **Envio:** no hook, o quadro completo de cada `RequestBuildBuyOp` local é copiado e enviado pelo canal confiável (`MsgType::BuildOp = 128`). O host repassa as operações de um convidado aos outros.
2. **Validação:** quem recebe confere o cabeçalho (ID e tamanho) e põe na fila.
3. **Aplicação:** a fila é executada **na thread de despacho do próprio jogo**, com um stream montado a partir de um stream real do mesmo processo (*template*) e o `self`/`a5` capturados localmente. A chamada vai ao despachante original, protegida por SEH.
4. **Em caso de falha:** se a aplicação falhar, o convidado pede o lote do host (o Lot State Hash corrige).

Validado fora do jogo: um quadro real atravessa a rede intacto até a fila do outro lado; quadros com tamanho errado ou com outro ID são recusados. **A aplicação dentro do jogo ainda precisa de teste:** construa uma parede, aperte Ctrl+Z e use o botão "Teste local: reaplicar minha última operação" na aba Construção.

## Onde está a construção (análise estática real, não suposição)

Ferramenta: [tools/re/rtti_map.py](../tools/re/rtti_map.py) (pefile + capstone sobre o `Simulation_x64.dll` instalado).

* **Alvo: `Simulation_x64.dll`, não `TS4_x64.exe`.** O exe tem DRM da EA (`.ooa`); a lógica de construção (`LotOperations`, `UndoRedoStack`, `BuildBuySessionManager`) fica na DLL da simulação, que não tem DRM e tem RTTI.
* **Despachante cliente→simulação: RVA `0x24380`** (15.385 bytes). Toda mensagem que a interface do jogo manda à simulação passa por ele, escolhida por um ID de 32 bits. Assinatura deduzida do prólogo:
  `u64 Dispatch(void* self, u32 a2, u32 msg_id, u32 a4, void* a5, Stream* stream)`.
* **41 mensagens mapeadas** (lista em [build_observer.cpp](../native/src/build/build_observer.cpp)). As de construção:
  `RequestBuildBuyOp 0x3a0f613c` (paredes, pisos, telhados, escadas, terreno, desfazer/refazer), `RequestBuildBuyLotUpdate 0x2b6ec145`, `SendBuildBuyEvent 0xd5f39c51`, `RequestHouseholdFundsUpdate 0xb0b3b5e3`, `SendLotDisplayLevel 0x8e887f70`, entre outras.
* `PkgRequestBuildBuyOpMsg`: objeto com contagem de referências, de 0x58 bytes. A vtable (`0xc0bb88`) tem só destrutor, AddRef e Release; o objeto é criado em `0x96e50`.

## O que esta etapa entrega

| Item | Status |
|---|---|
| Scanner AOB com curingas + limites de função (`.pdata`) + resolvedor RTTI em tempo de execução | ✅ testado contra o `Simulation_x64.dll` real: acha `0x24380` (casamento único), valida pelas constantes e acha as vtables `0xc0bb88`/`0xd751f0` |
| Hook MinHook no despachante, com validação e fallback por referência de string (desliga sozinho se o jogo mudar) | ✅ compila e é validado offline; **ainda não rodou dentro do jogo** |
| Observador: conta as 41 mensagens, grava cada operação de construção (argumentos + memória do pacote) em `Mods/cts4p/build_capture.jsonl` | ✅ implementado; os dados vêm quando você construir |
| Aba **Construção** no overlay (hook, contadores, últimas operações, hash) | ✅ |
| **Lot State Hash** + auto-correção sem desconectar (o lote do host prevalece, com reenvio do save) | ✅ implementado; não testado com 2 PCs |
| Onde cada jogador está agindo (coordenadas de clique, andar, Modo Construção) | ✅ no overlay (aba Jogadores) |
| **Reprodução em tempo real** de paredes/telhados/escadas/terreno/Ctrl+Z no outro PC | ⚠ **implementada (0.4.1), experimental**: caminho de rede validado; aplicação no jogo ainda não testada |
| **Ghost Cursor desenhado em 3D no mundo** | ❌ **ainda não**: precisa das matrizes da câmera (o Python só tem a câmera do último save) |

## Por que a reprodução ainda não existe

O hook já vê cada `RequestBuildBuyOp` com seu `Stream`, mas o conteúdo (onde ficam os bytes da operação e como reinjetá-los) só aparece com o jogo rodando. Inventar esse formato seria chute, e um replay errado pode corromper o lote. Próximo passo:

1. Instalar a 0.4.0, entrar num lote e, no Modo Construção, fazer ações isoladas: uma parede, um piso, um telhado, uma escada, terreno, Ctrl+Z, Ctrl+Y.
2. O arquivo `Mods/cts4p/build_capture.jsonl` traz cada operação com a memória do pacote.
3. Com isso: descobrir o buffer de bytes dentro do `Stream`, enviar ao host e reinjetar pelo mesmo `Dispatch` (mesma thread e argumentos), com o Lot State Hash conferindo cada passo.

## Ghost Cursor 3D

Para desenhar no mundo é preciso projetar a posição do outro jogador na tela de quem vê, o que exige a matriz de visão/projeção da câmera. Caminho: observar o constant buffer de câmera nas chamadas `ID3D11DeviceContext` (já estamos dentro do pipeline DX11) ou achar o objeto de câmera no `TS4_x64.exe`. Por enquanto, o overlay mostra as coordenadas e o modo de cada jogador.

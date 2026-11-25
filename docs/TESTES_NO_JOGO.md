# Testes dentro do jogo e bugs corrigidos (0.4.2)

Sessão de testes autônoma em 24/11, com o jogo real (1.129.39.1030) neste PC.

## Como testar sem um segundo PC

* **`tests/fake_guest.py`**: um "convidado falso" (Python 3.12 + a mesma `winmm.dll`) entra na sala do jogo e fala o protocolo do cts4p.
* **`tests/fake_host.py`**: o contrário; o jogo entra como convidado numa sala criada pelo script.
* **Canal de teste**: o jogo é comandado por um *named pipe* local (`\\.\pipe\cts4p-debug`: `status`, `host`, `join <código>`, `leave`). Fica **desligado por padrão** e só liga com `"debug_control": true` no `config.json`.

O acesso à janela do jogo (clicar e carregar um lote) não foi concedido durante a ausência do usuário. Por isso, o que exige estar dentro de um lote (save do host, cliente virtual, espelhamento, comandos, Lot State Hash, replay pela rede) segue sem teste no jogo.

## Resultados

| Teste no jogo real | Resultado |
|---|---|
| Jogo como **host**: sala criada, WebRTC até o convidado falso (RTT 0,27 ms), manifesto e comparação de conteúdo | ✅ |
| Python do host reage no menu principal (papel → host, convidado registrado, conferência de conteúdo) | ✅ após o bug #5 |
| Host fora de um lote adia o save e mantém a conexão | ✅ |
| Jogo como **convidado**: entra a partir do menu, Python envia `GUEST_HELLO` (`in_world: false`) | ✅ |
| Save de 1,0 MB (952 KB comprimido) recebido, hash conferido, gravado em `saves`, idêntico byte a byte | ✅ |
| Save do usuário intacto (`Slot_87654321.save`, 22/11 22:15:24); arquivo de teste apagado; `config.json` restaurado | ✅ |

## Bugs encontrados e corrigidos

1. **O save de sincronização sequestrava o slot do jogador.** O jogo grava `slot_id`/`slot_name` no registro *vivo* do save atual. Depois de uma sincronização, o "Salvar" normal iria para o slot do cts4p. Agora os valores originais são restaurados logo após o save de sincronização.
2. **Os comandos reencaminhados perdiam as regras do jogo.** 17 comandos (CAS) tinham `RESTRICT_SAVE_UNLOCKED` e 29 tinham `pack=`. O gerador agora extrai essas opções e o encaminhador reaplica exatamente as mesmas.
3. **O replay não funcionava para quem nunca construiu.** O stream-modelo agora vem de qualquer mensagem despachada pelo jogo (todas usam a mesma classe de stream).
4. **O replay podia ser aplicado no lote errado.** Agora há um portão (`build_gate`) controlado pelo Python: só abre com os dois jogos sincronizados no mesmo lote. Fechado, nada é enviado e o que chega é descartado. Testado entre 2 processos.
5. **Travamento no fluxo de entrada (crítico).** O Python só processava eventos dentro de `Zone.update`, que não existe no menu principal. O convidado nunca respondia e o host nunca enviava o save. Agora `sims4.core_services.on_tick` (chamado pelo `c_api_server_tick` em todo tick) faz o processamento fora de um lote. **Confirmado no jogo.**

Regressão final: 8 conjuntos nativos, testes no Python 3.7 do jogo e verificação estática (219 referências) — tudo passando, 0 warnings.

# 0.4.3: "Criar juntos", sincronização automática do save e revisão de segurança

Sessão autônoma em 24/11, com o jogo real neste PC (o jogo como convidado, `tests/fake_host.py` como host).

| Teste no jogo real | Resultado |
|---|---|
| Scripts 0.4.3 carregam; o gancho de `PersistenceService.save_game_gen` instala sem erro ("automatic save sync on") | ✅ |
| Save do host recebido e idêntico byte a byte | ✅ |
| Host tenta gravar em **outro slot** (0x00012345): o jogo recusa e nada é gravado | ✅ |
| Jogo envia a família mais recente da Biblioteca real ("Barros", 5 arquivos): chega idêntica byte a byte | ✅ |
| Host envia uma família de teste: o jogo grava na Biblioteca; arquivos de teste apagados em seguida | ✅ |
| Save do usuário intacto (`Slot_87654321.save`, 22/11 22:15:24) | ✅ |

Não testado (exige estar num lote com outra pessoa): o envio automático depois de salvar e se o jogo mostra na hora uma família que chegou com ele aberto.

## Bugs encontrados e corrigidos

6. **Um convidado conseguia gravar um save em qualquer slot do host** (inclusive o save do próprio jogador). Agora só quem entrou numa sala aceita saves, e só no slot do cts4p. A decisão usa o estado local, não o "papel" que o outro jogador diz ter.
7. **Nome inválido (UTF-8) derrubava o jogo.** Um nome de família ou de jogador com bytes inválidos fazia o JSON lançar exceção numa thread. Agora todo JSON usa substituição de caracteres inválidos e as mensagens da rede são tratadas com `try/catch`.
8. **Leitura do nome da família com estouro de inteiro** (comprimentos enormes no `.trayitem`). Corrigido e coberto por teste.
9. **Abuso de memória e disco.** O tamanho comprimido agora é limitado pelo tamanho real, só é aceita uma transferência por jogador de cada vez, há no máximo 20 famílias por jogador por sessão, e uma família que já existia é guardada como `.cts4p-bak`.
10. **Threads acumulando e transferências presas.** Um `SaveEnd` repetido ou desconhecido não cria mais threads, as threads terminadas são liberadas, e quando um jogador sai as transferências dele são encerradas (o botão "Enviar" não fica travado).

# Servidor de sinalização CTS4P — Cloudflare Worker (grátis)

O servidor só troca as mensagens iniciais (SDP/ICE) entre os jogadores de uma sala `CTS-XXXX`.
Depois que a conexão P2P abre, todo o tráfego do jogo vai direto entre os PCs; o Worker nem vê.

## Publicar (uma vez, ~3 minutos)

Pré-requisitos: Node.js instalado e uma conta gratuita na Cloudflare.

```powershell
cd signaling\cloudflare
npx wrangler login
npx wrangler deploy
```

O deploy imprime uma URL como `https://cts4p-signal.SEU-SUBDOMINIO.workers.dev`.
Confira no navegador: deve aparecer `cts4p signaling ok`.

## Configurar o mod

Em **cada PC**, edite `Documentos\Electronic Arts\The Sims 4\Mods\cts4p\config.json`
(o arquivo é criado na primeira vez que o jogo abre com o mod):

```json
{
  "signaling_url": "wss://cts4p-signal.SEU-SUBDOMINIO.workers.dev/ws"
}
```

Use `wss://` (não `https://`) e mantenha o `/ws` no final.
No jogo: `INSERT` → aba **Conexão** → **Testar servidor de sinalização** deve passar nas 5 etapas.

## Custos e limites

- Plano gratuito do Workers + Durable Objects (SQLite) — suficiente para uso pessoal.
- Usa a API de hibernação de WebSocket: salas paradas não consomem tempo de CPU.
- Máximo de 7 convidados por sala; mensagens de até 64 KB.

## Testar localmente (sem conta)

```powershell
cd signaling\cloudflare
npx wrangler dev --port 8788 --ip 127.0.0.1 --local
```

e use `ws://127.0.0.1:8788/ws` como `signaling_url`.

## Alternativa sem Cloudflare

`signaling/python/server.py` implementa o mesmo protocolo só com a biblioteca padrão do Python.
Serve para LAN, para um PC com porta aberta ou para testes:

```powershell
python signaling\python\server.py --port 8787
```

// cts4p signaling server for Cloudflare Workers (free plan).
//
// Only relays small JSON messages (SDP offers/answers and ICE candidates)
// between the host and guests of a "CTS-XXXX" room until their WebRTC link is
// up. No game data ever passes through here.
//
// One Durable Object per room keeps every participant on the same instance.
// Uses the WebSocket Hibernation API, so idle rooms cost nothing.
//
// Deploy:  npx wrangler deploy      (see README.md in this folder)
// Client:  wss://<worker>.<subdomain>.workers.dev/ws?room=CTS-ABCD&role=host|guest&name=Dani&proto=1

const ROOM_RE = /^CTS-[A-HJ-NP-Z2-9]{4}$/;
const MAX_GUESTS = 7;
const MAX_MESSAGE = 64 * 1024;
const MAX_NAME = 40;

function json(ws, obj) {
  try {
    ws.send(JSON.stringify(obj));
  } catch {
    // socket already closing
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/" || url.pathname === "/health") {
      return new Response("cts4p signaling ok\n", { headers: { "content-type": "text/plain; charset=utf-8" } });
    }
    if (url.pathname !== "/ws") return new Response("not found\n", { status: 404 });
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return new Response("expected a WebSocket upgrade\n", { status: 426 });
    }
    const room = (url.searchParams.get("room") || "").toUpperCase();
    if (!ROOM_RE.test(room)) return new Response("invalid room code\n", { status: 400 });
    const stub = env.ROOMS.get(env.ROOMS.idFromName(room));
    return stub.fetch(request);
  },
};

export class Room {
  constructor(state) {
    this.state = state;
  }

  // Live participants (sockets that completed the join).
  peers(except) {
    return this.state
      .getWebSockets()
      .filter((ws) => ws !== except)
      .map((ws) => ({ ws, ...(ws.deserializeAttachment() || {}) }))
      .filter((p) => p.id);
  }

  async fetch(request) {
    const url = new URL(request.url);
    const room = (url.searchParams.get("room") || "").toUpperCase();
    const role = url.searchParams.get("role");
    const name = (url.searchParams.get("name") || "Jogador").slice(0, MAX_NAME);

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server);

    const reject = (code, message) => {
      json(server, { t: "error", code, message });
      server.close(4000, code);
      return new Response(null, { status: 101, webSocket: client });
    };

    const peers = this.peers(server);
    const host = peers.find((p) => p.role === "host");
    if (role === "host") {
      if (host) return reject("room_taken", "room already has a host");
    } else if (role === "guest") {
      if (!host) return reject("no_host", "room not found");
      if (peers.filter((p) => p.role === "guest").length >= MAX_GUESTS) return reject("room_full", "room is full");
    } else {
      return reject("bad_request", "role must be host or guest");
    }

    const id = crypto.randomUUID().replaceAll("-", "").slice(0, 10);
    server.serializeAttachment({ id, role, name });
    json(server, { t: "welcome", id, room, host: role === "host" ? null : host.id });
    if (role === "guest") json(host.ws, { t: "peer_joined", id, name });
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    if (typeof message !== "string" || message.length > MAX_MESSAGE) return;
    let msg;
    try {
      msg = JSON.parse(message);
    } catch {
      return;
    }
    const me = ws.deserializeAttachment();
    if (!me || msg?.t !== "signal" || typeof msg.to !== "string") return;
    const target = this.peers(ws).find((p) => p.id === msg.to);
    // Star topology: guests may only signal the host.
    if (!target || (me.role === "guest" && target.role !== "host")) return;
    json(target.ws, { t: "signal", from: me.id, data: msg.data });
  }

  async webSocketClose(ws) {
    this.leave(ws);
  }

  async webSocketError(ws) {
    this.leave(ws);
  }

  leave(ws) {
    const me = ws.deserializeAttachment();
    if (!me) return;
    ws.serializeAttachment(null);
    for (const p of this.peers(ws)) {
      if (me.role === "host") {
        json(p.ws, { t: "room_closed" });
        p.ws.close(4002, "room_closed");
      } else if (p.role === "host") {
        json(p.ws, { t: "peer_left", id: me.id });
      }
    }
  }
}

"""Lot State Hash: structural fingerprint of the active lot + automatic healing.

Built only from data both players' games can read locally even while the
guest's simulation is frozen:
  * build_buy.get_all_block_polygons(plex)  rooms/blocks per level (walls,
    foundations, platforms, half walls that close rooms)
  * build_buy.get_lot_value(zone, False)    unfurnished value (walls, floors,
    paint, roofs, stairs, pools, fences, terrain paint all change it)
Furniture is deliberately excluded: the guest's frozen simulation does not
track objects the host creates, so they would cause false alarms.

Host: after build activity (exit from Build mode, observed build messages,
and every PERIOD seconds) sends its hash. Guest: compares; if it differs on
two consecutive checks, asks for a resync - the host's lot wins and the room
stays connected.
"""
import hashlib
import time

import build_buy
import services
from plex.plex_enums import INVALID_PLEX_ID

from cts4p import log
from cts4p.native.bridge import bridge
from cts4p.net import messages
from cts4p.sync.state import state

PERIOD = 60.0
DEBOUNCE = 3.0
HEAL_COOLDOWN = 120.0

_last_local = None
_pending_check_at = None
_last_periodic = 0.0
_mismatch_streak = 0
_last_heal = 0.0
_last_heal_text = ""


def _r(v):
    return int(round(float(v) * 100))  # centimetre precision


def compute():
    """Returns (hex digest, summary dict) or (None, reason) if the lot isn't available."""
    zone = services.current_zone()
    if zone is None or not zone.is_zone_running:
        return None, "fora de um lote"
    zone_id = services.current_zone_id()
    h = hashlib.sha1()
    summary = {"blocks": 0, "points": 0, "value": None}
    try:
        value = build_buy.get_lot_value(zone_id, False)
        summary["value"] = value[0] if isinstance(value, tuple) else value
        h.update(repr(value).encode())
    except Exception:
        log.exception("lot hash: get_lot_value failed")
    try:
        plex_service = services.get_plex_service()
        plex_id = (plex_service.get_active_zone_plex_id() if plex_service is not None else None) or INVALID_PLEX_ID
        polys = build_buy.get_all_block_polygons(plex_id) or {}
        for key in sorted(polys, key=repr):
            poly_data, level = polys[key]
            entry = [level]
            for poly in poly_data:
                pts = []
                for p in poly:
                    pts.append((_r(getattr(p, "x", 0)), _r(getattr(p, "z", getattr(p, "y", 0)))))
                    summary["points"] += 1
                entry.append(sorted(pts))
            h.update(repr(entry).encode())
            summary["blocks"] += 1
    except Exception:
        log.exception("lot hash: get_all_block_polygons failed")
    return h.hexdigest()[:16], summary


def _report(local, text):
    bridge.request({"op": "lot_hash", "local": local or "", "summary": text, "last_heal": _last_heal_text})


def request_check(delay=DEBOUNCE):
    """Schedules a (debounced) host-side hash broadcast."""
    global _pending_check_at
    if state.is_host:
        _pending_check_at = time.monotonic() + delay


def tick():
    """Called every game tick."""
    global _pending_check_at, _last_periodic
    if not state.is_host or not any(g.synced for g in state.guests.values()):
        return
    now = time.monotonic()
    after_build = _pending_check_at is not None and now >= _pending_check_at
    if not after_build and now - _last_periodic < PERIOD:
        return
    _pending_check_at = None
    _last_periodic = now
    broadcast("após construção" if after_build else "periódico")


def broadcast(reason):
    global _last_local
    digest, summary = compute()
    if digest is None:
        return
    _last_local = digest
    bridge.send_net(messages.LOT_HASH, messages.encode_json({
        "hash": digest, "zone_id": services.current_zone_id(), "reason": reason, "summary": summary,
    }))
    _report(digest, "Host: hash enviado aos convidados ({}; {} blocos, valor {})".format(reason, summary["blocks"], summary["value"]))


def on_host_hash(data):
    """Guest side: compare with our own lot; ask for a heal after two consecutive mismatches."""
    global _mismatch_streak, _last_heal, _last_heal_text
    if not state.guest_active or data.get("zone_id") != services.current_zone_id():
        return
    digest, summary = compute()
    if digest is None:
        return
    if digest == data.get("hash"):
        _mismatch_streak = 0
        _report(digest, "Idêntico ao host ({} blocos)".format(summary["blocks"]))
        return
    _mismatch_streak += 1
    log.info("lot hash mismatch #{}: local {} vs host {} ({})", _mismatch_streak, digest, data.get("hash"), data.get("summary"))
    if _mismatch_streak < 2 or time.monotonic() - _last_heal < HEAL_COOLDOWN:
        _report(digest, "Diferente do host ({}x) - conferindo de novo".format(_mismatch_streak))
        return
    _mismatch_streak = 0
    _last_heal = time.monotonic()
    _last_heal_text = time.strftime("%H:%M:%S") + " - lote divergente, pedido save do host"
    _report(digest, "Diferente do host: pedindo o lote do host (auto-correção)")
    log.notice("O lote ficou diferente do host. Pedindo o estado do host para corrigir...")
    bridge.send_net(messages.RESYNC_REQUEST, messages.encode_json({"reason": "lot_hash"}), target=state.host_peer or "")

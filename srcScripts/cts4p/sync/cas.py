"""CAS (Create-a-Sim) edits made by a guest, applied on the host. EXPERIMENTAL.

CAS runs inside each player's own game: the host tells the guest's game to
open CAS (client cheat "sims.exit2cas"), the guest edits locally and returns
to live mode. The guest then sends the edited appearance; the host sets the
same SimInfo fields (each one is a distributor Field, so setting it updates
the host UI and, through mirroring, every guest) and resends outfits.
"""
import base64

from cts4p import log

# SimInfoBaseWrapper properties that hold the appearance edited in CAS.
APPEARANCE_FIELDS = (
    "first_name", "last_name", "physique", "facial_attributes", "skin_tone", "skin_tone_val_shift",
    "pelt_layers", "custom_texture", "voice_pitch", "voice_actor", "voice_effect",
)


def _encode(value):
    if isinstance(value, (bytes, bytearray)):
        return {"b64": base64.b64encode(bytes(value)).decode("ascii")}
    if isinstance(value, (bool, int, float, str)) or value is None:
        return {"v": value}
    return None


def _decode(item):
    if not isinstance(item, dict):
        raise ValueError("bad field")
    if "b64" in item:
        return base64.b64decode(item["b64"])
    return item.get("v")


def collect(sim_info):
    """Guest side: appearance of `sim_info` as a JSON-safe dict."""
    fields = {}
    for name in APPEARANCE_FIELDS:
        try:
            encoded = _encode(getattr(sim_info, name))
        except Exception:
            continue
        if encoded is not None:
            fields[name] = encoded
    try:
        fields["outfits"] = {"b64": base64.b64encode(sim_info.save_outfits().SerializeToString()).decode("ascii")}
    except Exception:
        log.exception("could not serialize outfits of {}", sim_info)
    return fields


def apply(sim_info, fields):
    """Host side: applies fields produced by collect(). Returns the number applied."""
    applied = 0
    for name in APPEARANCE_FIELDS:
        if name not in fields:
            continue
        try:
            setattr(sim_info, name, _decode(fields[name]))
            applied += 1
        except Exception:
            log.warn("CAS sync: could not apply {} to {}", name, sim_info)
    if "outfits" in fields:
        try:
            msg = type(sim_info.save_outfits())()
            msg.ParseFromString(_decode(fields["outfits"]))
            sim_info.load_outfits(msg)
            applied += 1
        except Exception:
            log.exception("CAS sync: could not apply outfits to {}", sim_info)
    sim_info.resend_physical_attributes()
    return applied

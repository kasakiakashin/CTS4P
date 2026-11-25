"""Shared multiplayer state for the Python side.

Architecture (host-authoritative):
  * The host runs the only live simulation.
  * Each guest gets a *virtual client* on the host (its own active Sim and
    selectable Sims), so both players control their Sims independently.
  * Everything the host's simulation sends to the host UI is mirrored to the
    guests (needs, moods, queues, money, clock...), except what belongs to one
    player (their pie menus, dialogs, active-Sim/skewer updates), which is
    routed only to that player.
  * Guests freeze their own simulation and forward UI commands to the host.
  * Joining or zone changes are synchronized by sending the host's save.
"""

ROLE_IDLE = "idle"
ROLE_HOST = "host"
ROLE_GUEST = "guest"

# Dedicated save slot used for multiplayer syncs (never one of the player's own slots).
SYNC_SLOT_ID = 0x00054D50

# Session ids for virtual clients: high range that the game never assigns.
VCLIENT_ID_BASE = 0x7534_6D70_0000_0000
VACCOUNT_ID_BASE = 0x7534_6D70_0001_0000


class GuestInfo:
    """Host-side view of one connected guest."""

    def __init__(self, peer_id, name):
        self.peer_id = peer_id
        self.name = name
        self.local_client_id = None   # the guest's own client id inside its game
        self.vclient = None           # server.client.Client living on the host
        self.in_world = False
        self.synced = False           # loaded our latest save and got a virtual client
        self.game_msgs_sent = 0

    def __repr__(self):
        return "<Guest {} {}>".format(self.name, self.peer_id)


class State:
    def __init__(self):
        self.role = ROLE_IDLE
        self.room = ""
        self.local_name = ""
        self.native_ok = False
        # host side
        self.guests = {}              # peer_id -> GuestInfo
        self.vclients = {}            # vclient id -> GuestInfo
        self.save_generation = 0
        self.auto_sync_save = True    # refresh guests' copy whenever the host saves
        self.host_client_id = None
        self.waiting_pause = False
        # guest side
        self.host_peer = None
        self.guest_active = False     # welcomed: host stream applied, commands forwarded
        self.guest_frozen = False     # our own simulation stays stopped until we leave this lot
        self.announced_generation = None  # SAVE_INFO received, file not here yet
        self.local_client_id = None
        self.vclient_id = None
        self.expected_generation = None
        self.received_save_slot = None
        self.pending_cas_sim_id = None
        self.game_msg_seq = 0

    @property
    def is_host(self):
        return self.role == ROLE_HOST

    @property
    def is_guest(self):
        return self.role == ROLE_GUEST

    def guest_for_vclient_id(self, client_id):
        return self.vclients.get(client_id)

    def reset_session(self):
        self.guests.clear()
        self.vclients.clear()
        self.host_peer = None
        self.guest_active = False
        self.vclient_id = None
        self.expected_generation = None
        self.announced_generation = None
        self.received_save_slot = None
        self.pending_cas_sim_id = None
        # guest_frozen is kept: a lot that was driven by a host stays stopped until the player leaves it


state = State()

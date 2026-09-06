# Netplay with recomp-net (psxrecomp)

This is the **feature overview** for rollback-capable multiplayer in PSX
title projects that opt into `-DPSX_NETPLAY=ON` and ship `lib/recomp-net`
(plus lobby client). It describes what we are introducing for players and
title developers — not the internal soak checklist.

Deeper / implementation docs:

| Doc | Role |
|-----|------|
| [`NETPLAY_TOPOLOGY.md`](NETPLAY_TOPOLOGY.md) | Locked online vs LAN topology |
| [`ROLLBACK_MOTK_HOOKUP.md`](ROLLBACK_MOTK_HOOKUP.md) | MotK rollback integration notes |
| [`config_schema.md`](config_schema.md) `[netplay]` | Disc gate fields (`require_cue`, tracks, fingerprint) |
| [`lib/recomp-net/README.md`](../lib/recomp-net/README.md) | Transport / session library |
| Lobby server `WS_LOBBY.md` (recomp-net-server) | WebSocket lobby + SFU policy |

Opt-in at configure time: see [`GAME_PROJECT_SETUP.md`](GAME_PROJECT_SETUP.md)
(`PSX_NETPLAY`, `ENABLE_NETPLAY_IF_PRESENT`). The launcher NETPLAY button only
appears when the host advertises `GameInfo.netplay_supported`.

---

## Rollback netcode

Default match mode for opted-in titles is **rollback** (GGPO-style tip
prediction + resimulation), with **delay-sync** still available as an opt-out
(“Disable Rollback” in lobby settings, or `PSX_NET_MODE=delay`).

- Peers exchange pad tips over the session; missing remote input is
  predicted for a short horizon, then corrected by resim when the real tip
  arrives.
- Input delay (D) and prediction depth (P) can be auto-derived from RTT or
  set manually in the lobby.
- Savestate / snapshot rings and AV digests keep peers aligned; FMV / media
  paths use lockstep-friendly rules so digests stay meaningful.

Rollback is the product default for titles that ship it (e.g. MotK). Delay-sync
remains useful for debugging and for hosts that prefer fixed lag.

---

## Seats vs session slots

Lobby chat is masked for profanity and slurs on every path: the server
filters relayed lines, and every client runs recomp-net's
`rnet_chat_filter_apply` on each line it puts in its ring -- so LAN rooms
(host-relayed `MOTK5 CHAT`) are filtered without a server. The list is
`lib/recomp-net/data/chat_filter_words.txt`; `RNET_CHAT_FILTER=0` disables
the client pass.

The lobby browser also carries a per-game server chat (server op
`server_chat`, relayed to every client listed for the same title) and lists
only the players online for this title; both are online-only, since a LAN
room has no server. The same emoji and profanity handling applies.

Players can move themselves in the lobby and ask to swap seats (online via
the server's `seat_move` / `seat_swap_request` / `seat_swap_answer`; on LAN
via `MOTK5 SEATMOVE` / `SWAPASK` / `SWAPANS` / `SWAPRES` relayed by the host),
so the lobby host can hold any seat. The session is planned at launch
(`ae_np_plan_session_slots`): the host is always **session slot 0** — the
seat every host-only path keys on — and the other players follow in lobby-seat
order; each session slot drives the controller port of its lobby seat
(`PsxNetplayConfig.port_of_slot`), so the game sees a player where the lobby
seated them. Session slots are therefore compact (no holes) even when the
lobby is sparse; the sparse ports are what the game sees.

## Host in the spectator table

Online rooms let the host watch from the gallery and still run the match. The
host keeps session slot 0 — the seat every host-only path keys on (save
states, card sync, the start barrier, overlay host controls) — but its pad is
muted (`psx_netplay_stage_local` substitutes "no controller") and slot 0 maps
to no controller port; player seats sit at lobby seat + 1 and drive port
slot − 1. The server publishes `host_spectates` with the launch and sizes the
input relay for the extra forwarded slot; every peer derives its session slot
from that one flag. The session can therefore hold `PSX_MAX_PLAYERS + 1`
slots. LAN rooms have no gallery, so this is online-only.
Env for command-line sessions: `PSX_NET_HOST_SPECTATES=1` on every peer.

## Bring your own memory card (seat 2)

By default a match runs on the **host's** memory-card choices: at launch the
host hashes / sends both of its cards to every guest (the `SRAM` state op), and
guests play from a sandbox copy so their own cards are never written.

Some games need each player's *own* card in the machine — Yu-Gi-Oh! Forbidden
Memories duels load each duelist's deck from a separate card. For that, seat 2
(P2) can **bring its card**:

- In the lobby a memory-card glyph sits beside P2's name. P2 clicks it to
  offer its **local slot-1** card; the host can click it to disallow / allow
  guest cards. It lights up only when both agree, and every peer sees the same
  state. Default is off (host cards only).
- At launch, before the host's card broadcast, P2 uploads that card to the host
  over the session (`MEMCARD` state op, the one guest→host transfer). The host
  installs it as the match's **slot-2** card and the normal broadcast then
  carries it to everyone, P2 included.
- The host's real slot-2 card is neither read by the match nor written: the
  host rebinds slot 2 to `<memcard_dir>/netplay/guest_card2.mcd` for the
  session and restores its own file on shutdown. P2's real cards stay untouched
  too (guests already sandbox both slots); what the match writes to slot 2
  lands in P2's sandbox copy, not in P2's personal card.
- The value is **settled once by the host at start** and delivered with the
  launch caps (`guest_memcard_active`, or the trailing `MOTK1 START` line on
  LAN), so a toggle racing the start cannot leave peers disagreeing about
  whether a card is coming.

The room page also carries a lobby chat. Online it is the server's `chat` op,
echoed to everyone seated; on LAN the host relays it (`MOTK5 CHATREQ` from a
guest, `MOTK5 CHAT` to everyone). Join, leave and kick are announced as system
lines the same way (empty sender fields on the wire mark them as system).

Env override for command-line sessions: `PSX_NET_GUEST_MEMCARD=1` on **every**
peer (a host that expects a card from a seat that never sends one waits at the
`mc_guest_wait` barrier — the stall phase names it).

---

## Hybrid graphics (determinism + present quality)

Netplay must keep **guest simulation identical** across peers. Present quality
is separate.

When **OpenGL** is selected and netplay is active, psxrecomp can run a
**dual-raster** path:

| Layer | Scale | Role |
|-------|-------|------|
| Software rasterizer | **1×** (headless authority) | Deterministic VRAM / digests / rollback snaps |
| OpenGL | Player supersampling (e.g. 2×–4×) | Window present quality only |

Cost: extra CPU for the 1× SW pass. Benefit: peers can use different GL
settings without desyncing the sim. SW-only netplay forces scale 1 for the
whole path. Offline play keeps full supersampling with no dual-raster tax.

---

## Connectivity: ICE, TURN, SFU, LAN

### Online lobbies (WebSocket + SFU)

Online matches go through the lobby at **netplay.retcomm.net**
(WebSocket control plane). Match **UDP pad traffic** for online rooms uses an
**SFU star** (selective forwarding unit on the lobby server): every peer sends
to the SFU; the SFU fans out to other seats. Peers do **not** mesh each other
for game data online.

Current lobby policy prefers **always SFU** for online starts so CGNAT /
asymmetric NAT does not strand players on failed ICE attempts.

### ICE + TURN

**ICE** (with **TURN** relay via the project’s coturn on
`netplay.retcomm.net`) remains part of the stack for discovery,
signaling, and fallbacks. With `PSX_NETPLAY=ON`, `runtime.cmake` defaults
`RNET_ENABLE_ICE=ON` (libjuice) before adding `recomp-net`. Pass
`-DRNET_ENABLE_ICE=OFF` only for LAN-only / no-FetchContent builds.
libjuice is pulled as a **pinned URL tarball** (not `git clone`) so RetComM
AppImage / mismatched-libcurl hosts can still configure; offline builds can
vendor `lib/recomp-net/third_party/libjuice` or set `-DRNET_LIBJUICE_ROOT`.
“Force TURN” in the UI can raise delay floors for relay-heavy paths; it
does not replace the SFU online architecture above.

### LAN / Direct IP (P2P star)

Without a lobby start (LAN or Direct IP):

- **2 players:** peer-to-peer UDP.
- **3+ players:** **host-as-relay** local star — the session host fans out
  tips to other seats (same star shape as online SFU, but the game host is
  the hub).

Sim authority: pad **slot 0** is the session host (`START`, state transfer).
Guests rearrange among seats **1..N−1**.

---

## Multitap + seat ceiling

| Item | Value |
|------|--------|
| Library / lobby / UI ceiling | **8** seats (`RNET_MAX_SLOTS`, dual SCPH-1070) |
| Per-title cap | `game.toml` `players` / `PSX_MAX_PLAYERS` (e.g. MotK=2, Bomberman Party=5) |

**Offline:** recomp-ui exposes Multitap under Settings → INPUT on PSX titles
with `num_players >= 3`. Off hides seats beyond the two native controller
ports and caps `g_offline_pad_count` at 2 (`settings.toml` `[controller]
multitap`, default on). Multitap still arms at game-start when three or more
offline seats are live (`multitap_port` from `game.toml`).

**Multitap analog (hack):** tap seats are plain digital by default. Opt in
with `game.toml` / `settings.toml` `[controller] multitap_analog = true`
(or Settings → INPUT / Lobby Settings). When on, tap seats may report
DualShock (`0x73` + sticks) in multitap bulk status. Hosts publish
`match_caps.multitap_analog` so every peer applies the same setting at
launch. Not reliable across titles — leave off unless the game needs it.

**Netplay (psxrecomp only):** lobbies with more than **2** seats always
force SCPH-1070 multitap on (`force_session_pads_connected` /
session start when `slot_count >= 3`). The offline Multitap toggle does
not opt out of that. Empty tap seats are fine — not every slot needs a
device.

Rollback and delay-sync both carry multitap pad bytes.

**BIOS settle:** each peer advertises a BIOS offer (can run OpenBIOS /
SCPH-1001, and whether OpenBIOS is selected) — online on ready, LAN on JOIN.
At Start the host freezes one session BIOS (`openbios` or `scph1001`) via
`match_caps.session_bios` (online) or the `MOTK1 START` line (LAN). The session
uses OpenBIOS unless every seated peer can run SCPH-1001 and nobody selected
OpenBIOS. Peers that cannot apply a settled SCPH-1001 abort instead of falling
back. That choice boots the match only — it does not change each peer’s saved
BIOS preference. See `docs/BIOS_SELECTION.md` (Netplay lobby settle).

---

## Disc identity for multi-track titles

Netplay is **dump-strict** for titles that declare it. Peers must run the
**same playable image geometry**, not “any USA ISO.”

Typical `[netplay]` in `game.toml` (MotK example):

```toml
[netplay]
require_cue = true
required_tracks = 17
# optional: required_disc_fp = "…"   # TOC fingerprint
```

| Rule | Why |
|------|-----|
| Prefer / require **`.cue` + sibling `.bin` track files** | Multi-track Redump layout (data + XA/audio) |
| Exact **track count** when `required_tracks > 0` | Track-01-only dumps desync CDDA/XA vs full cues |
| Reject bare incomplete mounts when `require_cue` | Cue→bin fallback cannot invent missing tracks |
| Optional **TOC fingerprint** (`required_disc_fp`) | Same track count still wrong dump |

Generate & rebuild / prepare flows should point at the **`.cue`**, not a lone
`.bin`. See [`config_schema.md`](config_schema.md) and the release checklist in
[`GAME_PROJECT_SETUP.md`](GAME_PROJECT_SETUP.md).

---

## Related product pieces

- **Lobby UI** (recomp-ui): host/join, room settings, rollback toggles, FORCE
  TURN, player names — only when `PSX_NETPLAY` is on and the title advertises
  netplay.
- **VERSION / lobby match pin:** peers should run the same release pin so
  generated code and protocol stay compatible.
- **Mods:** disabled for all netplay sessions (lobby / LAN / direct / rematch).
  Launcher `commit_netplay` and the runtime clear the in-session plan without
  touching the user's offline mod selection. Synced mod plans are deferred.

---

## Enabling for a new title

1. Vendor or submodule `recomp-net` under `psxrecomp/lib/recomp-net` (or set
   `RECOMP_NET_ROOT`).
2. Before `include(runtime.cmake)`: `set(PSX_NETPLAY ON CACHE BOOL … FORCE)`.
3. `psxrecomp_add_game_runtime(… ENABLE_NETPLAY_IF_PRESENT …)` with
   `MAX_PLAYERS` / `game.toml` `players` set correctly.
   ICE (libjuice) defaults ON with `PSX_NETPLAY`; override with
   `-DRNET_ENABLE_ICE=OFF` if needed.
4. Fill `[netplay]` disc gates for multi-track games.
5. Test LAN 2P, then online lobby; soak rollback + FMV if the title uses media.

This document will grow as N-way rollback confirmation, SFU soak on 5P titles,
and further ICE/SFU policy land.

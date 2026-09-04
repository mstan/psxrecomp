# Netplay topology (locked decisions)

| Decision | Choice |
|----------|--------|
| Online transport | Lobby UDP **SFU star** (`recomp-net-server` input relay) |
| Mesh | **No** — peers never dial each other online |
| Sim authority | Pad **slot 0** = session host (`START`, state xfer) |
| Seat moves | Guests rearrange among seats **1..N−1**; host stays at 0 |
| Seat ceiling | **8** (`RNET_MAX_SLOTS`, lobby `MAX_SLOTS`, PSX dual multitap) |

## Paths

```text
Online (WebSocket lobby)
  every peer ──UDP──► lobby SFU ──fan-out──► other seats
  WS still carries lobby JSON + ICE signaling (ICE unused for match data)

LAN / Direct IP (no WS start)
  2P: P2P UDP
  3+: host-as-relay LAN hub (local star; game host fans out)
```

## Per-title players

- Framework / UI / net library ceiling: **8**
- `game.toml` `players` / `MAX_PLAYERS` may be lower (MotK=2, Bomberman=5)
- `PSX_MAX_PLAYERS == 8` enables dual SCPH-1070 (ports 1+2 → pads 0–7)
- Single multitap (5–7) keeps `multitap_port` (Bomberman Port 2)

## Follow-ups (not in this pass)

- N-way `hash_confirm` / FRAME_COMMIT agreement for rollback at N>2
- Validate 5P Bomberman over SFU + rollback host facade parity with MotK

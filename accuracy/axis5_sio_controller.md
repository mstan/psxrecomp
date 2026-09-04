# Axis 5 — SIO / Controller / Memory-Card Accuracy

Cross-reference of `runtime/src/sio.c` (+ `main.cpp` pad driver, `memcard.c`)
against the in-tree Beetle/Mednafen oracle
(`beetle-psx/mednafen/psx/`:
`frontio.cpp`, `input/dualshock.cpp`, `input/gamepad.cpp`, `input/memcard.cpp`)
and nocash psx-spx "Controllers and Memory Cards".

Historical read-only research; citations use the line numbers from that audit.

> **Status 2026-07-26:** Tomba now offers explicit Analog / D-Pad modes instead
> of Hybrid. The Tomba-only `g_pad_legacy_cfg` protocol fork, its game-config
> key, debug command, and helper script have been removed. References to them
> below document the retired implementation and are not descriptions of the
> current runtime.

---

## 1. What our implementation does

### 1.1 Bus / transfer model (`sio.c`)

- SIO0 MMIO at `0x1F801040..0x1F80104E` handled in `sio_read`/`sio_write`
  (`sio.c:1306`, `sio.c:1361`). A byte is "transferred" when the guest writes
  `SIO_TX_DATA` (`0x1F801040`) with `TX_EN` set; we process it synchronously via
  `sio_process_byte` (`sio.c:1131`) and arm a delayed ACK→IRQ7.
- Two timing models behind `SIO_MODEL_CYCLE_PACED` (default 1, `sio.c:1767`):
  - **Access-paced legacy path** — `sio_irq_countdown` decrements on every SIO
    register access and on `sio_tick`; fires IRQ7 + sets `STAT.ACK`/`STAT.IRQ`
    (`sio.c:1983`). Delays: `SIO_IRQ_DELAY_PAD=4`, `SIO_IRQ_DELAY_CARD=8`
    (`sio.c:490`,`498`).
  - **Cycle-paced shifter** — `sio_shift_*` + `sio_pending_ack` + `sio_advance`
    (`sio.c:1882`) modelling `SIO_BAUD_CYCLES_DEFAULT=1088` shift + `170`-cycle
    ACK. *However* the **pad fast-path** (`sio.c:1386`) bypasses the shifter
    entirely: any pad byte (`active_device==DEV_PAD`, or `bus_owner==PAD`, or
    `tx==0x01`) is processed immediately and uses the access-paced
    `SIO_IRQ_DELAY_PAD` countdown. So in practice **pad timing is still
    access-paced, not cycle-paced.**
- `active_device` (`NONE/PAD/MEMCARD`, `sio.c:173`) routes a byte to
  `pad_process_byte` or `mc_process_byte`. First byte selects: `0x01`→pad,
  `0x81`→card (`sio.c:1148`).

### 1.2 Pad state machine (`pad_process_byte`, `sio.c:727`)

A 3-state FSM (`PAD_IDLE → PAD_WAIT_ACCESS → PAD_SEND_RESPONSE`, `sio.c:55`),
**byte-indexed not bit-indexed**. Per-slot state:

- `pad_buttons[2]` (active-low), `pad_analog[2]` (0=digital id `0x41`, 1=analog
  id `0x73`), `pad_stick[2][4]`, `pad_connected`.
- `pad_in_config[2]` — DualShock config-mode latch (report `0xF3`).
- `pad_supports_config[2]` — 1 = DualShock (answers config cmds), 0 = plain
  digital pad (ignores them) (`sio.c:87`).
- `pad_type_req[2]` — deferred host type-change request, applied only at
  `PAD_IDLE && !in_config` (`sio.c:100`, applied `sio.c:733`).
- `g_pad_legacy_cfg` — per-game flag (Tomba only). When set, `0x43` always
  answers `0xF3` and the `0x45/0x46/0x47/0x4C/0x4D` config queries answer
  unconditionally with no enter/exit tracking (`sio.c:718`, `sio.c:793`,
  `sio.c:802`).

Command handling at `PAD_WAIT_ACCESS` (`sio.c:752`):
- `0x42` poll: emits `[id, 0x5A, btnlo, btnhi]` (4 bytes), or 8 bytes with the
  four stick axes when `pad_analog || pad_in_config` (`sio.c:767`).
- `0x43` (modern): emits `[cur_id, 0x5A, 0,0,0,0,0,0]` (8 bytes), latches
  enter/exit from the data byte at response index 2 (`sio.c:862`).
- `0x44` (modern): mode-set; latches analog/digital from data byte at index 2
  (`sio.c:870`).
- `0x45/0x46/0x47/0x4C/0x4D/0x4F` (modern): canned responses, **only while
  `pad_in_config`** (`sio.c:823`).
- Anything else, or a config cmd outside config: hi-z (`0xFF`), end txn
  (`sio.c:845`).

### 1.3 Host pad driver (`main.cpp`)

`sample_pad_into_sio` (`main.cpp:1240`) runs once (twice w/ low-latency) per
frame: computes `eff_analog` from mode (DIGITAL/ANALOG/HYBRID, `main.cpp:1261`),
pushes sticks via `sio_set_pad_sticks`, and requests the type via
`sio_request_pad_type` (`main.cpp:1288`). HYBRID auto-flips analog on stick
deflection / digital on d-pad (`main.cpp:1267`).

### 1.4 Memory card FSM (`mc_process_byte`, `sio.c:906`)

Per-slot `mc_slots[2]` saved/restored across device switches (`sio.c:176`).
Read (`0x52`), write (`0x57`), get-id (`0x53`); FLAG byte `0x08`→`0x00` after
first access (`sio.c:934`); sector echo, checksum, `0x47`/`0x4E`/`0xFF` end
codes. This is broadly faithful to `memcard.cpp` and is not the focus here.

---

## 2. Discrepancies vs Beetle + psx-spx

### D1 — **Pad protocol is byte-framed, the hardware/Beetle is bit-clocked** (model)
Beetle clocks **one bit per `Clock()` call**, 8 bits per byte
(`dualshock.cpp:402-418`, `gamepad.cpp:143-157`), with DSR pulses scheduled per
byte (`frontio.cpp:756`, `dualshock.cpp:1077`). We process a whole byte per
`SIO_TX_DATA` write and fake one ACK per byte. This is an acceptable abstraction
*for the data exchanged* (the guest only sees whole RX bytes + ACK IRQs), but it
means our ACK/IRQ **timing** is a hand-tuned constant (`DELAY_PAD=4` accesses)
rather than `baud<<scaleshift` cycles (`frontio.cpp:520,783`). Low risk for
correctness, real risk for any timing-sensitive pad probe. Logged for the cycle
axis; not the hybrid bug.

### D2 — **`0x42` returns analog (8-byte) frame whenever `pad_in_config` is set, even with analog OFF** (semantic, WRONG)
Ours: `if (pad_analog || pad_in_config) → 8-byte stick frame` (`sio.c:775`).
Beetle: `0x4200` returns the 6-byte analog payload `if(analog_mode || mad_munchkins)`
(`dualshock.cpp:592`) — i.e. analog frame in config mode is correct **but** see
D3 for the framing-count mismatch. The conceptual match is OK; the danger is the
**byte count** (D3) and that we tie stick reporting to *our* `pad_in_config`
which can be set/cleared on a different schedule than Beetle's `mad_munchkins`.

### D3 — **Config-mode poll byte counts differ from a real DualShock** (semantic)
Real DualShock / Beetle digital `0x42` = ID `0x41` + `0x5A` + **2** button
bytes = the device transmits `0x41,0x5A,btn0,btn1` framed as id then `transmit_count=2`
buttons (`dualshock.cpp:604-606`); analog `0x42` = id `0x73` + `0x5A` + 2 btn +
**4 axis** bytes (`dualshock.cpp:594-600`). Our digital frame is 4 bytes
(`id,5A,lo,hi`) and analog is 8 bytes (`id,5A,lo,hi,rx,ry,lx,ly`) — counts agree
with hardware. **But** note our **stick axis order** is `rx,ry,lx,ly`
(`sio.c:776-779`) whereas a real pad reports **right stick first only in the
config layout**; the standard analog `0x42` order is `RX,RY,LX,LY` — this
matches. No defect here, but it is the opposite of Beetle's internal
`axes[0]=left, axes[1]=right` ordering, so a naive trace diff will look
"swapped" — call it out in validation so it isn't chased as a bug.

### D4 — **`0x43` semantics are fundamentally wrong vs a real DualShock** (semantic, the core bug — see §3)
On a **real DualShock NOT in config mode**, command `0x43` is **a poll that
*also* arms config entry**: it returns the *button/analog data frame*
(`dualshock.cpp:471-490` — `0x43` in `command_phase==1` transmits buttons+axes,
identical framing to `0x42`), and only the **second data byte == 0x01** sent
during that same transaction flips `mad_munchkins=true` afterwards
(`dualshock.cpp:503-515`). Ours instead returns an **all-zero 8-byte frame**
`[cur_id,5A,0,0,0,0,0,0]` for `0x43` (`sio.c:793-798`). So during the
enter-config handshake we feed the driver **zeros where the real pad sends live
button/stick data.** A driver that reads those bytes as input (many do, since
`0x43`-with-data is indistinguishable from a poll on the wire) sees "all
pressed" / centered-stick garbage. **This is the phantom-input mechanism.**

### D5 — **Digital `InputDevice_Gamepad` does NOT implement config at all** (confirms our `pad_supports_config=0` path is correct)
Beetle's plain gamepad only handles `0x42`; any other command sets
`command_phase=-1` and transmits nothing (`gamepad.cpp:202-209`). Our
`pad_supports_config==0` else-branch returns hi-z for config commands
(`sio.c:845`) — **faithful.** Good. The Tomba2 digital-pad fix (memory:
`tomba2_menu_input_fix`) is correct and matches hardware.

### D6 — **No analog-mode lock, no `mad_munchkins` gating of `0x44`** (semantic, minor)
Beetle: `0x44` only takes effect inside MAD MUNCHKINS (config) mode
(`dualshock.cpp:679,689-712`), and `0x4402` sets `analog_mode_locked`
(`dualshock.cpp:714-725`); a locked pad ignores the physical analog button
(`dualshock.cpp:203`). Ours honours `0x44` set-mode (`sio.c:870`) but has **no
lock state**, so a game that locks analog (common: forces DualShock and locks)
will still see our HYBRID auto-flip yank it back to digital — exactly the kind
of "type flips underneath the game" desync the deferred-request machinery was
built to avoid but cannot prevent, because we never learned the pad is locked.

### D7 — **`0x4D` rumble-map negotiation and host output** (resolved)
Resolved: `0x4D` now echoes the previous six-byte motor map while latching the
new map. Mapped motor bytes from later `0x42` polls are retained per slot and
forwarded to SDL's low/high-frequency rumble channels by the frontend. The
protocol path is pinned by `sio_dualshock_rumble_test`; legacy savestates remain
loadable and receive the conventional `{0x00,0x01,0xFF,...}` map with stopped
motors.

### D8 — **`0x45` analog-status byte is hard-coded `0x03` (analog on)** (semantic)
Beetle `0x4501` reports `transmit_buffer[1] = analog_mode ? 0x01 : 0x00`
(`dualshock.cpp:743`) — i.e. the *current* analog state. Ours always answers
`{0xF3,0x5A,0x03,0x02,0x01,0x02,0x01,0x00}` (`sio.c:808`/`831`), reporting analog
ON unconditionally. A driver polling `0x45` to discover the live mode reads
"analog" even when we are presenting digital → mode-confusion, another route to
the hybrid desync.

### D9 — **Config enter/exit latches on the wrong byte index** (semantic, subtle)
Beetle enters MAD MUNCHKINS only when, during the `0x43` transaction,
`transmit_pos==2 && receive_buffer==0x01` (`dualshock.cpp:503`) — i.e. the
**third** byte of the transaction (idx 2, the byte after id+0x5A). We latch
`pad_in_config` when `pad_current_cmd==0x43 && pad_response_idx==2`
(`sio.c:862`). Because our `pad_response_idx` starts at 1 after the cmd byte and
increments per response byte, idx 2 corresponds to the 3rd exchanged byte —
which **does** line up. No defect, but it is fragile and undocumented relative to
Beetle's `transmit_pos` convention; verify with a trace before trusting it.

### D10 — **DTR/SELECT-deassert does not run the analog-button evaluation** (model)
On real hardware the analog toggle is evaluated when DTR goes inactive
(`dualshock.cpp:383-388`, `CheckManualAnaModeChange`). We have no DTR-edge hook
into pad type; our type change is host-driven via `pad_type_req`. This is *by
design* (the host emulates the analog button) but means our "analog button" can
fire at a **different bus phase** than hardware would, which is the seam the
phantom bug lives in.

---

## 3. HYBRID PAD BUG — root-cause analysis (priority)

**Symptom:** in HYBRID mode the pad's reported type flips between digital
(`0x41`) and analog (`0x73`) as the player moves between d-pad and stick; under
the *modern* config SM this produces phantom "all-pressed"/disconnect input in
Tomba (and a phantom dash in MMX6). Tomba is shipped on the **legacy** flag
(`g_pad_legacy_cfg=1`) to dodge it; that flag is explicitly marked LEGACY/
remove-me (`sio.c:665-717`).

**Root cause — there are two compounding faults, both in `0x43` handling:**

### (a) `0x43` returns zeros instead of the live data frame (D4 — the real bug)
A real DualShock treats `0x43` **exactly like `0x42`** for the bytes it *sends*
— it transmits the current button + (in analog mode) axis frame
(`dualshock.cpp:471-490`) — and merely *additionally* uses the host's 0x01/0x00
data byte to toggle config on/off **after** the transaction
(`dualshock.cpp:503-515`). Tomba's libpad, like most, issues the config
enter/poll with `0x43` and **reads the returned frame as controller state**.
Our `0x43` returns `[id,0x5A,0,0,0,0,0,0]` (`sio.c:793`). Active-low PSX buttons:
`0x00` = **all buttons pressed**, sticks `0x00` = hard up-left. So every frame
the driver runs a `0x43` (which it does to manage analog mode) it reads a
phantom "everything pressed." That is the phantom input, independent of any type
flip. The legacy flag hides it only because Tomba's legacy driver path happens
not to consume the `0x43` frame the same way — it is luck, not correctness.

### (b) The type flip changes the ID byte across a transaction boundary the game treats as continuous (D8/D10)
Even with (a) fixed, our HYBRID flip changes `pad_analog` (hence the reported
id 0x41↔0x73 and the 4↔8 byte frame length) at an idle boundary
(`sio.c:733`). libpad's findpad caches the pad type; when the id changes it
re-runs detection, and the modern SM manufactures a one-frame
"id changed / re-detect" that the game reads as unplug→replug (memory:
`tomba_phantom_input_v050`). Beetle never has this problem because the type only
changes via the **physical analog button evaluated at DTR-inactive**
(`dualshock.cpp:171-213`), and crucially Beetle's `0x45` reports the *live*
analog state (`dualshock.cpp:743`) and `0x44` can lock it — so a well-written
driver always knows the current mode and never mis-reads the frame length. Our
`0x45` lies (always `0x03`/analog-on, D8), so the driver's cached mode and the
actual frame length we send can disagree → it parses an 8-byte analog frame as a
4-byte digital one (or vice-versa), shifting every subsequent byte → garbage
buttons.

**Why the per-poll flip diverges from a real DualShock:** a real pad's type is
*stable within and across* polls and only changes on a deliberate analog-button
press (debounced, DTR-gated) or an explicit `0x44` from the game; the game is
always told the truth by `0x45`. Our hybrid mode flips the type from an
*external* signal (host stick/d-pad heuristic) on a schedule the game's pad
driver never sanctioned, and we then **mis-report the current mode (`0x45`) and
send zero frames for `0x43`** so the driver cannot recover. The deferred
`pad_type_req` machinery (`sio.c:642`) correctly prevents a flip *mid-transaction*,
but it cannot fix that the *content* of the `0x43` frame is wrong and that `0x45`
lies — those are the actual desync sources.

**Bottom line:** The hybrid bug is **not** primarily a "flip races the
handshake" timing bug (that part is already mitigated). It is that our **`0x43`
sends a zero data frame instead of the live poll frame** (§3a / D4) and our
**`0x45` reports a fixed analog-on status instead of the live mode** (§3b / D8).
Fix those two to match `dualshock.cpp` and the HYBRID flip becomes benign for
every title — which is exactly what the LEGACY comment predicted, letting the
`g_pad_legacy_cfg` opt-in be deleted.

---

## 4. Prioritized fix list (player-visible impact first)

> **STATUS 2026-07-26:** Fixes 1, 2, 3 are implemented. Fix 4 is also complete:
> Tomba no longer uses Hybrid, and the legacy pad-config branch has been removed.
>
> **Prior status 2026-06-27 (branch wt/tomba2-axis5-controller):** Fixes 1, 2, 3 IMPLEMENTED
> in sio.c (0x43 live frame; 0x45 live analog byte; 0x44 analog-mode-lock + hybrid-flip
> lock gate), transcribed from dualshock.cpp. Validated no-regression on Tomba 2
> (digital pad, pad 0xFFFF at rest, boots). These are gated to the MODERN SM and leave
> Tomba 1's legacy path untouched, so they're safe to ship; MMX6 (modern SM) gains the
> phantom-dash fix. **Fix 4 (retire `g_pad_legacy_cfg`) is GATED on a behavioral
> DualShock-game validation** (MMX6 phantom-dash gone / Tomba 1 benign hybrid on the
> modern SM) — that needs a Tomba 1/MMX6 build against THIS framework worktree (a full
> regen, since the runtime ABI now carries the cycle-axis CPUState fields), so it's a
> separate validated pass, not a blind delete. Fixes 5/6 not yet done.

1. **[P0] Make `0x43` transmit the live poll frame, not zeros** (D4 / §3a).
   In `pad_process_byte` `0x43` branch (`sio.c:787`), populate
   `pad_response[2..]` with the same button (+axis when analog) bytes as the
   `0x42` branch instead of `0x00`. Mirror `dualshock.cpp:471-490`. This is the
   single highest-impact fix: removes the phantom "all-pressed" at its source
   and is the prerequisite for retiring `g_pad_legacy_cfg`.

2. **[P0] Make `0x45` report the live analog state** (D8 / §3b). Set
   `pad_response[3] = pad_analog[slot] ? 0x01 : 0x00` in both the modern and
   legacy `0x45` answers (`sio.c:808`,`831`), matching `dualshock.cpp:743`. Lets
   the driver track frame length across a hybrid flip and stops the
   off-by-frame-length garbage reads.

3. **[P1] Implement analog-mode lock (`0x44`/`0x4402`)** (D6). Track
   `analog_mode_locked` per slot; when locked, `sio_request_pad_type` /
   `pad_type_req` must be ignored (the HYBRID auto-flip must not override a
   game-locked mode) — mirror `dualshock.cpp:203,714-725`. Prevents the hybrid
   heuristic from fighting games that pin DualShock.

4. **[DONE 2026-07-26] Retire `g_pad_legacy_cfg`.** Tomba now exposes only
   explicit Analog / D-Pad modes, so there is no live type flip to accommodate.
   The flag, legacy SIO branches, config key, debug command, helper script, and
   Tomba opt-in were removed. All titles use the modern DualShock state machine.

5. **[DONE 2026-08-06] Echo-back `0x4D` rumble map and forward motors.** The
   SIO model now routes negotiated weak/strong values to the SDL frontend.

6. **[P2] Move pad ACK/IRQ timing onto the cycle-paced shifter** (D1). Remove
   the pad fast-path bypass (`sio.c:1386`) so pad bytes use
   `baud<<scaleshift` timing like the card path and like `frontio.cpp:520,783`.
   Cross-listed with the cycle axis. Needed for any title that times pad probes.

7. **[P3] Document/verify the config-latch byte index and stick-order
   conventions** (D3, D9) so trace diffs aren't mistaken for bugs.

---

## 5. Validation method (native port 4500 vs Beetle oracle port 4382)

Tomba2 dev runtime debug port is **4500** (memory: `tomba2_bringup`); Beetle
oracle is the `psx-beetle.exe` JSON server (CLAUDE.md §16 names 4380; this task
specifies **4382** — use whichever port the running beetle binary bound). Both
expose the identical wire protocol, including `pad_status` (`debug_server.c:5713`)
and `sio_trace` (`debug_server.c:9963`), and Beetle fires a per-byte SIO trace
callback `sio_trace_cb(TX, RX, Control)` (`frontio.cpp:772-774`).

Per fix, free-run BOTH processes to the same screen (do NOT pause/step — query
the always-on rings), drive the same input, then diff:

- **Fix 1 (`0x43` frame):** `sio_trace` on both. Filter to transactions whose
  2nd byte is `0x43`. **Native (current):** RX = `..,id,5A,00,00,..`.
  **Beetle:** RX = `..,id,5A,<btn0^FF>,<btn1^FF>,<axes..>`. After the fix the
  native RX bytes for `0x43` must equal the `0x42` RX bytes captured in the same
  state. Confirm `pad_status` shows no spurious all-pressed (`pad:0xFFFF` at
  rest on both).

- **Fix 2 (`0x45` status):** drive HYBRID, toggle stick↔d-pad, capture `0x45`
  transactions. Native byte[3] must follow `pad_status.slotN.analog` (true→0x01,
  false→0x00) and equal Beetle's `0x4501` byte[1] in the matching analog state.

- **Fix 3 (lock):** issue a `0x44`/`0x4402` lock sequence (or drive a game that
  does), then deflect the stick. `pad_status.analog` must NOT change while
  locked; cross-check Beetle stays in its locked mode across the same input.

- **Fix 4 (retire legacy):** run Tomba in both explicit Analog and D-Pad modes
  on the modern SM. `pad_status` at rest = `0xFFFF` both slots, no disconnect blips; menu
  responsive; in-game d-pad↔stick transitions produce no phantom dash/unpause.
  Diff a 600-byte `sio_trace` window vs Beetle for the same navigation — RX
  streams should match transaction-for-transaction (allowing the D3 stick-order
  note).

- **Fix 6 (timing):** compare ACK→IRQ7 spacing. Use the SIO IRQ ring
  (`sio_get_irq_ring`, `sio.c:378`) for native `delay_applied`/byte_seq vs
  Beetle's `ClockDivider`/`dsr_pulse_delay` cadence (`frontio.cpp:756,783`).
  Native pad-byte IRQ spacing should land within the `baud<<scaleshift` window,
  not the fixed 4-access constant.

General invariant to assert on every diff: at rest, `pad_status` =
`{pad:0xFFFF, slot0.connected:true}` on BOTH; any deviation from `0xFFFF` while
no key is held is a phantom-input regression and must reproduce identically (or
better) than Beetle.

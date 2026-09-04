#!/usr/bin/env python3
"""cosim.py — first-divergence co-simulation coordinator. See COSIM_ORACLE.md.

Launches two clean psx-cosim instances (each its own complete deterministic PSX),
advances BOTH to the same retired-instruction-count (icount) checkpoints, and compares
their full-state chain hashes. Checkpoints are icount-keyed (stride / --max /
--start-cycle are all in retired instructions, where a branch+delay-slot retires as
ONE event), so both sides always hash the same architectural position; the guest-cycle
clock is inside the hash, so cycle-accounting drift is itself a detectable divergence.
The first checkpoint whose chains differ brackets the first divergence; the
per-subsystem `sub` hashes + the ring `window` say WHAT and WHERE.

The two instances are NOT interleaved on one timeline — they are two independent
deterministic runs, sampled at matched icounts. Validity rests ENTIRELY on
determinism, which is why you MUST pass the gates first:

  # GATE 1 — determinism/hashing: two of the SAME backend must NEVER diverge.
  python cosim.py --a compiled --b compiled --stride 65536 --max 1500000000
  python cosim.py --a interp   --b interp   --stride 65536 --max 1500000000
  # GATE 4 — injected fault must halt at the right field:
  python cosim.py --a compiled --b compiled --inject-at 200000000 --inject ram:100000:1

  # THE RUN (only after gates pass):
  python cosim.py --a compiled --b interp --stride 65536 --max 1500000000
"""
import socket, subprocess, os, sys, time, argparse

# Game-agnostic: env overrides let the same coordinator drive any game's psx-cosim
# build (e.g. a Tomba regression) without editing this file. Defaults = MMX6.
#   COSIM_EXE  = path to that game's build-cosim/psx-cosim.exe
#   COSIM_GAME = path to that game's game.toml
EXE = os.environ.get("COSIM_EXE", r"../MegaManX6Recomp\build-cosim\psx-cosim.exe")
CWD = os.environ.get("COSIM_CWD", os.path.dirname(EXE))
GAME = os.environ.get("COSIM_GAME", r"../MegaManX6Recomp\game.toml")
LOGDIR = os.path.join(CWD, "cosim-logs")

def tail_file(path, max_bytes=8192):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - max_bytes), os.SEEK_SET)
            return f.read().decode(errors="replace")
    except Exception as e:
        return f"<could not read {path}: {e}>"

def launch(mode, port, stride, start_cycle):
    os.makedirs(LOGDIR, exist_ok=True)
    # Two instances sharing one runtime dir race on mods/state.toml publish
    # (temp-file rename collision, ENOENT). The mods root is EXE-DIR-relative
    # (main.cpp: exe_dir_from_argv(argv[0]) / "mods"), NOT cwd-relative — so
    # instance B needs its OWN exe copy, not just its own cwd. COSIM_CWD_B
    # names B's runtime dir; B runs the exe found there (override: COSIM_EXE_B).
    cwd = CWD
    exe = EXE
    cwd_b = os.environ.get("COSIM_CWD_B", "")
    if cwd_b and port != 4600 and str(port).endswith("1"):
        cwd = cwd_b
        exe_b = os.environ.get("COSIM_EXE_B",
                               os.path.join(cwd_b, os.path.basename(EXE)))
        if not os.path.isfile(exe_b):
            raise RuntimeError(
                f"instance B exe not found: {exe_b} — copy the freshly built "
                f"exe into COSIM_CWD_B (its mods/ root is exe-dir-relative)")
        if os.path.getmtime(exe_b) < os.path.getmtime(EXE) - 1:
            raise RuntimeError(
                f"instance B exe is STALE: {exe_b} is older than {EXE} — "
                f"copy the freshly built exe into COSIM_CWD_B")
        exe = exe_b
    env = dict(os.environ)
    env["PSX_HEADLESS"] = "1"
    env["PSX_COSIM_PORT"] = str(port)
    env["PSX_COSIM_STRIDE"] = str(stride)
    # The starvation watchdog is driven by debug-server heartbeats, while cosim
    # can spend seconds fast-forwarding without polling that server. Treat it as
    # a GUI/debugger watchdog, not a cosim oracle signal.
    env.setdefault("PSX_STARVATION_TIMEOUT_US", "0")
    if start_cycle:
        env["PSX_COSIM_START_CYCLE"] = str(start_cycle)
    if mode == "interp":
        env["PSX_FORCE_INTERP"] = "1"
    log_path = os.path.join(LOGDIR, f"cosim_{mode}_{port}_{os.getpid()}.log")
    log_file = open(log_path, "wb")
    p = subprocess.Popen([exe, "--headless", "--no-launcher", "--game", GAME],
                         cwd=cwd, env=env,
                         stdout=log_file, stderr=subprocess.STDOUT,
                         creationflags=0x00000200)
    p._cosim_log_path = log_path
    p._cosim_log_file = log_file
    if os.environ.get("PSX_COSIM_BELOW_NORMAL") == "1":
        try:
            import ctypes
            h = ctypes.windll.kernel32.OpenProcess(0x0400, False, p.pid)
            ctypes.windll.kernel32.SetPriorityClass(h, 0x00004000)  # BELOW_NORMAL
        except Exception:
            pass
    return p

def connect(port, timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.socket(); s.settimeout(5); s.connect(("127.0.0.1", port))
            return s
        except Exception:
            time.sleep(0.5)
    raise RuntimeError(f"cosim port {port} never came up")

def cmd(s, line, timeout=600):
    s.settimeout(timeout)
    s.sendall((line + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d: break
        buf += d
    return buf.decode(errors="replace").strip()

def close_proc_log(p):
    try:
        p._cosim_log_file.close()
    except Exception:
        pass

def report_child_failure(label, proc, cp, exc):
    code = proc.poll()
    print(f"[{label} reset] at local cp {cp}: poll={code} error={exc}", flush=True)
    path = getattr(proc, "_cosim_log_path", "")
    if path:
        print(f"--- {label} log tail: {path} ---", flush=True)
        print(tail_file(path), flush=True)

def wait_parked(sa, sb, timeout=600):
    t0 = time.time()
    last = ("", "")
    while time.time() - t0 < timeout:
        ra = kv(cmd(sa, "status", timeout=120))
        rb = kv(cmd(sb, "status", timeout=120))
        last = (ra, rb)
        if ra.get("parked") == "1" and rb.get("parked") == "1":
            return ra, rb
        time.sleep(0.05)
    raise RuntimeError(f"cosim did not park before timeout: A={last[0]} B={last[1]}")

def parse_cpu(resp):
    """'pc XX hi XX ... r0 XX ... c0 XX ... mdts XX gtets XX ...' -> {field: val}"""
    t = resp.split()
    return {t[i]: t[i+1] for i in range(0, len(t)-1, 2)}

def kv(resp):
    """parse a reply into {key: next_token} for every token that is followed by
    another token. Robust to a leading bare status word (e.g. 'parked cp N cycle M
    chain HEX'): we scan ALL adjacent pairs, so 'chain' -> HEX is always captured
    regardless of the leading word's parity. (The earlier stride-2 parser misaligned
    on the leading 'parked'/'timeout' word and returned chain=None for BOTH sides,
    making every compare None==None == 'equal' — a silent blind spot. Gate 4 exists
    to catch exactly this.)"""
    t = resp.split()
    d = {}
    for i in range(len(t) - 1):
        d.setdefault(t[i], t[i+1])
    return d

def dump_window(s, n=24):
    s.settimeout(30); s.sendall(f"window {n}\n".encode()); buf = b""
    while b"\nend\n" not in buf and b"\nend" not in (buf[-6:] if len(buf)>=6 else buf):
        d = s.recv(65536)
        if not d: break
        buf += d
        if buf.endswith(b"end\n") or buf.endswith(b"end"): break
    return buf.decode(errors="replace")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="compiled", choices=["compiled", "interp"])
    ap.add_argument("--b", default="interp",   choices=["compiled", "interp"])
    ap.add_argument("--stride", type=int, default=65536)
    ap.add_argument("--max", type=int, default=1_500_000_000,
                    help="stop after this many retired instructions (icount units)")
    ap.add_argument("--porta", type=int, default=4600)
    ap.add_argument("--portb", type=int, default=4601)
    ap.add_argument("--inject-at", type=int, default=0)
    ap.add_argument("--inject", default="")   # e.g. ram:100000:1  or reg:2:1
    ap.add_argument("--start-cycle", type=int, default=0,
                    help="free-run to this absolute retired-instruction count "
                         "(icount) before checkpointing")
    ap.add_argument("--cpudiff-at-cp", type=int, default=0,
                    help="step both to this checkpoint and field-diff the CPU dump")
    args = ap.parse_args()

    if args.cpudiff_at_cp:
        print(f"launch A={args.a}:{args.porta}  B={args.b}:{args.portb}  "
              f"stride={args.stride} start={args.start_cycle} cpudiff={args.cpudiff_at_cp}",
              flush=True)
        pa = launch(args.a, args.porta, args.stride, args.start_cycle); pb = launch(args.b, args.portb, args.stride, args.start_cycle)
        try:
            sa = connect(args.porta); sb = connect(args.portb)
            wait_parked(sa, sb)
            n = args.cpudiff_at_cp - 1  # both start parked at cp 1
            for _ in range(max(0, n)):
                cmd(sa, "step 1", timeout=1200)
                cmd(sb, "step 1", timeout=1200)
            ra = cmd(sa, "cpu"); rb = cmd(sb, "cpu")
            da = parse_cpu(ra)
            db = parse_cpu(rb)
            print(f"A cpu dump: {ra}", flush=True)
            print(f"B cpu dump: {rb}", flush=True)
            print(f"CPU field-diff at cp {args.cpudiff_at_cp}  (A={args.a} B={args.b}):", flush=True)
            diffs = [k for k in da if da.get(k) != db.get(k)]
            if not diffs:
                print("  (no CPU field differs)", flush=True)
            for k in diffs:
                print(f"  cpu.{k}: A={da.get(k)}  B={db.get(k)}", flush=True)
            ga = parse_cpu(cmd(sa, "gte"))
            gb = parse_cpu(cmd(sb, "gte"))
            print("GTE field-diff:", flush=True)
            gdiffs = [k for k in ga if ga.get(k) != gb.get(k)]
            if not gdiffs:
                print("  (no GTE field differs)", flush=True)
            for k in gdiffs:
                print(f"  gte.{k}: A={ga.get(k)}  B={gb.get(k)}", flush=True)
            # device-timing fields (behind irqctl/gpu sub-hashes)
            va = parse_cpu(cmd(sa, "dev")); vb = parse_cpu(cmd(sb, "dev"))
            print("DEV field-diff (irqctl/gpu timing):", flush=True)
            ddiffs = [k for k in va if va.get(k) != vb.get(k)]
            if not ddiffs:
                print("  (no dev field differs)", flush=True)
            for k in ddiffs:
                print(f"  dev.{k}: A={va.get(k)}  B={vb.get(k)}", flush=True)
            xa = parse_cpu(cmd(sa, "gpu")); xb = parse_cpu(cmd(sb, "gpu"))
            print("GPU field-diff:", flush=True)
            xdiffs = [k for k in xa if xa.get(k) != xb.get(k)]
            if not xdiffs:
                print("  (no GPU field differs)", flush=True)
            for k in xdiffs:
                print(f"  gpu.{k}: A={xa.get(k)}  B={xb.get(k)}", flush=True)
            print("IRQ trace A:", flush=True)
            print("  " + cmd(sa, "irqtrace"), flush=True)
            print("IRQ trace B:", flush=True)
            print("  " + cmd(sb, "irqtrace"), flush=True)
        finally:
            for p in (pa, pb):
                try:
                    if p.poll() is None: p.terminate()
                except Exception: pass
                close_proc_log(p)
        return

    print(f"launch A={args.a}:{args.porta}  B={args.b}:{args.portb}  stride={args.stride} start={args.start_cycle}", flush=True)
    pa = launch(args.a, args.porta, args.stride, args.start_cycle); pb = launch(args.b, args.portb, args.stride, args.start_cycle)
    try:
        sa = connect(args.porta); sb = connect(args.portb)
        ia, ib = wait_parked(sa, sb)
        if args.max <= args.start_cycle:
            print(f"--max must be greater than --start-cycle ({args.start_cycle})", flush=True)
            return
        max_cp = (args.max - args.start_cycle) // args.stride
        print(f"both up; stepping {max_cp} checkpoints (stride {args.stride})", flush=True)
        print(f"initial park A cycle {ia.get('cycle')} cp {ia.get('cp')}  B cycle {ib.get('cycle')} cp {ib.get('cp')}", flush=True)

        # Both guests are parked at checkpoint 1 immediately after launch (deterministic
        # cycle boundary). We advance BOTH one checkpoint per iteration and compare the
        # cumulative chain hash. First mismatch = first divergence.
        injected = not args.inject
        cp = 0
        while cp < max_cp:
            # Gate-4 injection into B just before the checkpoint that crosses inject_at.
            if not injected and args.start_cycle + (cp + 1) * args.stride >= args.inject_at:
                kind, a, b = args.inject.split(":")
                print(cmd(sb, f"inject {kind} {int(a)} {int(b)}"), flush=True)
                print(f"[inject] {args.inject} into B before cp {cp+1}", flush=True)
                injected = True

            try:
                ra = kv(cmd(sa, "step 1"))
            except Exception as e:
                report_child_failure("A", pa, cp, e)
                return
            try:
                rb = kv(cmd(sb, "step 1"))
            except Exception as e:
                report_child_failure("B", pb, cp, e)
                return
            if pa.poll() is not None or pb.poll() is not None:
                print(f"[exit] a={pa.poll()} b={pb.poll()} at cp {cp}", flush=True); break
            ca, cb = ra.get("chain"), rb.get("chain")
            if ca is None or cb is None:
                print(f"[FATAL] could not parse chain — tool is BLIND, aborting.\n"
                      f"  A: {ra}\n  B: {rb}", flush=True); return
            cyc_a, cyc_b = ra.get("cycle"), rb.get("cycle")
            if cyc_a != cyc_b:
                print(f"[TIMING] cycle skew A={cyc_a} B={cyc_b} at cp {ra.get('cp')} "
                      f"icnt A={ra.get('icnt')} B={rb.get('icnt')} — checkpoints are "
                      f"icount-keyed, so BOTH sides hashed the state after the same "
                      f"retired-instruction count; a cycle difference here is a REAL "
                      f"cycle-accounting divergence between the backends (the clock is "
                      f"in the chain hash, so the chain should differ too).",
                      flush=True)
            if ca != cb:
                print(f"\n*** FIRST DIVERGENCE at checkpoint cp={ra.get('cp')} "
                      f"cycle~{cyc_a} (frame~{int(cyc_a)//566000}) ***", flush=True)
                print(f"  A chain={ca}  B chain={cb}", flush=True)
                print("  --- A subhash ---\n  " + cmd(sa, "sub"), flush=True)
                print("  --- B subhash ---\n  " + cmd(sb, "sub"), flush=True)
                print("  (the FIRST subsystem hash that differs is where it split)", flush=True)
                print("  --- A window ---\n" + dump_window(sa, 16), flush=True)
                print("  --- B window ---\n" + dump_window(sb, 16), flush=True)
                return
            cp += 1
            if cp % 256 == 0:
                print(f"  ok cp {cp} cycle {cyc_a} (frame~{int(cyc_a)//566000}) chain {ca}", flush=True)

        print("no divergence within --max (or a process exited).", flush=True)
        if args.inject:
            print("NOTE: injection run — a clean 'no divergence' means the tool MISSED the "
                  "fault (gate-4 FAIL); it should have stopped at the injected checkpoint.", flush=True)
    finally:
        for p in (pa, pb):
            try:
                if p.poll() is None: p.terminate()
            except Exception:
                pass
            close_proc_log(p)

if __name__ == "__main__":
    main()

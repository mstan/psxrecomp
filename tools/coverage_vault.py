#!/usr/bin/env python3
"""Additive coverage vault for overlay captures + compiled cache.

Overlay coverage — overlay_captures.json (raw disc-derived overlay bytes +
executed/entry PCs) and the gcc-compiled cache DLLs — normally lives ONLY in
gitignored build dirs, so a `cmake` clean or a fresh worktree wipes it, and it's
re-derivable only by replaying those game areas. This tool maintains a single,
safe, ADDITIVE vault OUTSIDE any build dir: every merge UNIONS new coverage in
and never drops what's already there.

  - captures: union by VARIANT (load_addr + hash of the captured bytes). Same
    variant => union its executed_pcs / dispatch_entry_pcs and preserve the
    extractor-only static_dispatch_entry_pcs provenance subset. Distinct variants
    (same address, different scene's overlay) are all kept.
  - cache: filenames are content-keyed (<addr>_<crc>.dll/.ranges), so a copy-if-
    absent (or newer) is a safe additive union.

It contains game-derived bytes, so the vault dir must stay gitignored / private
(same rule as overlay_captures.json). This script is pure tooling (no game data)
and is committed.

Usage:
  coverage_vault.py merge --vault DIR [--captures captures.json]
                          [--addendum captures.addendum.jsonl] [--cache CACHE_DIR]
  coverage_vault.py stats --vault DIR
  coverage_vault.py compact-addendum --addendum captures.addendum.jsonl
                                      --persist-dir IMMUTABLE_DIR
"""
import argparse, json, os, shutil, hashlib, sys, contextlib

CAP_NAME = "overlay_captures.json"
CACHE_SUB = "cache"

def _variant_key(region):
    b = region.get("bytes_b64", "") or ""
    return "%s:%s" % (region.get("load_addr"), hashlib.sha1(b.encode()).hexdigest())

def _load_list(path):
    """Load the legacy/latest manifest plus runtime additive contributions
    from a history directory <path>.d (each file a full JSON list snapshot,
    e.g. one per runtime process/session). Union by variant across every
    contribution so a fresh manifest write never drops an in-flight append."""
    paths = [path] if os.path.isfile(path) else []
    history = path + ".d"
    if os.path.isdir(history):
        paths = [os.path.join(history, n) for n in sorted(os.listdir(history))
                 if n.lower().endswith(".json")] + paths
    index = {}
    for source in paths:
        try:
            with open(source, encoding="utf-8") as f:
                v = json.load(f)
            if not isinstance(v, list):
                raise ValueError("root is not a list")
        except Exception as e:
            print("  warn: could not read %s (%s); skipping contribution" %
                  (source, e))
            continue
        for r in v:
            k = _variant_key(r)
            if k not in index:
                index[k] = dict(r)
            else:
                tgt = index[k]
                for fld in ("executed_pcs", "dispatch_entry_pcs",
                            "function_entry_pcs", "seeds"):
                    tgt[fld] = sorted(set(tgt.get(fld, [])) |
                                      set(r.get(fld, [])))
    return list(index.values())

def _load_addendum(path):
    """Load every valid append-only history record, ignoring a torn tail.

    Each line is an independent snapshot wrapper. A hard kill may truncate only
    the final line; earlier launches remain usable and a later runtime append
    quarantines the bad tail with a newline.
    """
    regions = []
    seen_refs = set()
    if not path or not os.path.exists(path):
        return regions
    with open(path, encoding="utf-8", errors="replace") as history:
        for lineno, line in enumerate(history, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except Exception as e:
                print("  warn: ignoring invalid addendum line %d in %s (%s)" %
                      (lineno, path, e))
                continue
            if not isinstance(record, dict):
                print("  warn: ignoring unknown addendum record on line %d in %s" %
                      (lineno, path))
                continue
            schema = record.get("schema")
            if schema == "psxrecomp overlay capture addendum v1":
                captures = record.get("captures", [])
                if isinstance(captures, list):
                    regions.extend(r for r in captures if isinstance(r, dict))
                continue
            if schema == "psxrecomp overlay capture addendum v2":
                snapshot = record.get("snapshot")
                expected = str(record.get("fnv64", "")).upper()
                if not isinstance(snapshot, str) or not snapshot:
                    print("  warn: v2 addendum line %d has no snapshot" % lineno)
                    continue
                if not os.path.isabs(snapshot):
                    snapshot = os.path.join(os.path.dirname(path), snapshot)
                ref_key = (os.path.normcase(os.path.abspath(snapshot)), expected)
                if ref_key in seen_refs:
                    continue
                seen_refs.add(ref_key)
                if not os.path.exists(snapshot):
                    print("  warn: v2 snapshot missing on line %d: %s" %
                          (lineno, snapshot))
                    continue
                actual = _fnv64_file(snapshot)
                if expected and expected != "%016X" % actual:
                    print("  warn: v2 snapshot signature mismatch on line %d: %s" %
                          (lineno, snapshot))
                    continue
                regions.extend(r for r in _load_list(snapshot)
                               if isinstance(r, dict))
                continue
            print("  warn: ignoring unknown addendum record on line %d in %s" %
                  (lineno, path))
    return regions

def _fnv64_file(path):
    value = 1469598103934665603
    with open(path, "rb") as source:
        while True:
            chunk = source.read(65536)
            if not chunk:
                break
            for byte in chunk:
                value ^= byte
                value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value

def _verified_v2_record(record, addendum, lineno):
    snapshot = record.get("snapshot")
    expected = str(record.get("fnv64", "")).upper()
    if not isinstance(snapshot, str) or not snapshot:
        raise ValueError("v2 addendum line %d has no snapshot" % lineno)
    if not os.path.isabs(snapshot):
        snapshot = os.path.join(os.path.dirname(addendum), snapshot)
    snapshot = os.path.abspath(snapshot)
    if not os.path.isfile(snapshot):
        raise ValueError("v2 snapshot missing on line %d: %s" %
                         (lineno, snapshot))
    actual = "%016X" % _fnv64_file(snapshot)
    if not expected or expected != actual:
        raise ValueError("v2 snapshot signature mismatch on line %d: %s" %
                         (lineno, snapshot))
    result = dict(record)
    result["fnv64"] = actual
    result["snapshot"] = snapshot
    return result

def compact_addendum(addendum, persist_dir):
    """Atomically replace embedded v1 snapshots with verified v2 references.

    Immutable snapshots are the authority during this operation. Every valid
    v1 record must have its exact runtime-named file and matching FNV signature;
    otherwise the original addendum is left byte-for-byte untouched.
    """
    if not addendum or not os.path.isfile(addendum):
        raise ValueError("addendum does not exist: %s" % addendum)
    if not persist_dir or not os.path.isdir(persist_dir):
        raise ValueError("persist directory does not exist: %s" % persist_dir)
    addendum = os.path.abspath(addendum)
    persist_dir = os.path.abspath(persist_dir)
    old_bytes = os.path.getsize(addendum)
    records = []
    seen_refs = set()
    v1_count = v2_count = invalid_count = duplicate_count = 0
    with open(addendum, encoding="utf-8", errors="replace") as history:
        for lineno, line in enumerate(history, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except Exception as e:
                print("  warn: dropping invalid addendum line %d (%s)" %
                      (lineno, e))
                invalid_count += 1
                continue
            if not isinstance(record, dict):
                raise ValueError("unknown addendum record on line %d" % lineno)
            schema = record.get("schema")
            if schema == "psxrecomp overlay capture addendum v1":
                game = record.get("game")
                session = record.get("session")
                sequence = record.get("sequence")
                expected = str(record.get("fnv64", "")).upper()
                if (not isinstance(game, str) or not game or
                        not isinstance(session, str) or not session or
                        not isinstance(sequence, int) or sequence < 0 or
                        len(expected) != 16 or
                        any(c not in "0123456789ABCDEF" for c in expected)):
                    raise ValueError("malformed v1 metadata on line %d" % lineno)
                basename = "%s_%s_%04d_%s.json" % (
                    game, session, sequence, expected)
                snapshot = os.path.join(persist_dir, basename)
                converted = dict(record)
                converted.pop("captures", None)
                converted["schema"] = "psxrecomp overlay capture addendum v2"
                converted["snapshot"] = snapshot
                converted = _verified_v2_record(converted, addendum, lineno)
                v1_count += 1
            elif schema == "psxrecomp overlay capture addendum v2":
                converted = _verified_v2_record(record, addendum, lineno)
                v2_count += 1
            else:
                raise ValueError("unknown addendum schema on line %d: %r" %
                                 (lineno, schema))
            ref_key = (os.path.normcase(converted["snapshot"]),
                       converted["fnv64"])
            if ref_key in seen_refs:
                duplicate_count += 1
                continue
            seen_refs.add(ref_key)
            records.append(converted)

    tmp = "%s.tmp-%d" % (addendum, os.getpid())
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as out:
            for record in records:
                out.write(json.dumps(record, separators=(",", ":")))
                out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, addendum)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    new_bytes = os.path.getsize(addendum)
    print("coverage_vault: compacted %s" % addendum)
    print("  records: %d v1 converted, %d v2 retained, %d duplicates and %d invalid lines dropped" %
          (v1_count, v2_count, duplicate_count, invalid_count))
    print("  bytes:   %d -> %d (saved %d)" %
          (old_bytes, new_bytes, old_bytes - new_bytes))
    return v1_count, v2_count, duplicate_count, invalid_count

@contextlib.contextmanager
def _exclusive_lock(path):
    """Cross-process lock for the read/union/replace capture transaction."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path + ".lock", "a+b") as lock:
        lock.seek(0)
        if os.name == "nt":
            import msvcrt
            if os.path.getsize(path + ".lock") == 0:
                lock.write(b"0")
                lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == "nt":
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

def _atomic_write_json(path, records):
    """Write the vault manifest via tmp+replace with best-effort durability
    (data fsync + POSIX directory fsync so a crash right after replace()
    cannot leave a torn/missing manifest)."""
    tmp = "%s.%d.tmp" % (path, os.getpid())
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(records, f, indent=1)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        if os.name != "nt":
            dfd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
            try:
                os.fsync(dfd)
            finally:
                os.close(dfd)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)

def merge_capture_regions(vault_json, src):
    """Union `src` region records into the vault manifest at `vault_json`,
    under the cross-process lock, refusing to silently clobber a corrupt
    existing vault. Shared by both the whole-manifest merge (merge_captures)
    and the append-only addendum merge (merge_addendum)."""
    if not src:
        return 0, 0
    with _exclusive_lock(vault_json):
        # Keep a corrupt existing union visible instead of silently replacing
        # it as an empty vault. Immutable contributions remain usable for repair.
        if os.path.isfile(vault_json):
            try:
                with open(vault_json, encoding="utf-8") as f:
                    if not isinstance(json.load(f), list):
                        raise ValueError("root is not a list")
            except Exception as exc:
                raise RuntimeError("refusing to overwrite corrupt vault %s: %s" %
                                   (vault_json, exc))
        index = { _variant_key(r): r for r in _load_list(vault_json) }
        new_variants = new_pcs = 0
        for r in src:
            k = _variant_key(r)
            if k not in index:
                index[k] = dict(r)
                new_variants += 1
                new_pcs += len(r.get("executed_pcs", []))
            else:
                tgt = index[k]
                for fld in ("executed_pcs", "dispatch_entry_pcs",
                            "static_dispatch_entry_pcs",
                            "function_entry_pcs", "seeds"):
                    cur = set(tgt.get(fld, []))
                    add = set(r.get(fld, []))
                    if fld == "executed_pcs":
                        new_pcs += len(add - cur)
                    tgt[fld] = sorted(cur | add)
        _atomic_write_json(vault_json, list(index.values()))
    return new_variants, new_pcs

def merge_captures(vault_json, src_json):
    if not src_json or not (os.path.exists(src_json) or
                            os.path.isdir(src_json + ".d")):
        return 0, 0
    return merge_capture_regions(vault_json, _load_list(src_json))

def merge_addendum(vault_json, addendum):
    return merge_capture_regions(vault_json, _load_addendum(addendum))

def merge_cache(vault_cache, src_cache):
    """Mirror compiled DLLs/.ranges/.resident into the vault, preserving relative
    layout (<compiler>/<arch-abi>/cg<N>_<hash>/file). The layout is load-
    bearing: the same content-keyed filename exists under different cg dirs
    with DIFFERENT compiled bytes (per-emitter generations), so a flat copy
    would mix generations. The original flat listdir() also simply never
    matched anything — the cache has always been nested — so the vault's DLL
    mirror sat empty (found 2026-07-03)."""
    if not src_cache or not os.path.isdir(src_cache):
        return 0
    added = 0
    for root, _dirs, files in os.walk(src_cache):
        rel = os.path.relpath(root, src_cache)
        for fn in files:
            if not (fn.endswith(".dll") or fn.endswith(".ranges") or
                    fn.endswith(".resident")):
                continue
            src = os.path.join(root, fn)
            dstdir = os.path.join(vault_cache, rel) if rel != "." else vault_cache
            dst = os.path.join(dstdir, fn)
            os.makedirs(dstdir, exist_ok=True)
            if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst) + 1:
                shutil.copy2(src, dst)
                if fn.endswith(".dll"):
                    added += 1
    return added

def cmd_stats(vault):
    cj = os.path.join(vault, CAP_NAME)
    regs = _load_list(cj)
    pcs = sum(len(r.get("executed_pcs", [])) for r in regs)
    ndll = 0
    vc = os.path.join(vault, CACHE_SUB)
    if os.path.isdir(vc):
        for _root, _dirs, files in os.walk(vc):
            ndll += sum(1 for f in files if f.endswith(".dll"))
    print("vault: %s" % vault)
    print("  captures: %d variant(s), %d executed PC(s)" % (len(regs), pcs))
    print("  cache:    %d DLL(s)" % ndll)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["merge", "stats", "compact-addendum"])
    ap.add_argument("--vault", help="vault directory (kept gitignored/private)")
    ap.add_argument("--captures", help="source overlay_captures.json to merge in")
    ap.add_argument("--addendum", help="append-only overlay capture history (.jsonl)")
    ap.add_argument("--cache", help="source cache dir (e.g. build/cache/<game_id>) to merge in")
    ap.add_argument("--persist-dir", help="immutable capture snapshot directory")
    a = ap.parse_args()
    if a.cmd == "compact-addendum":
        if not a.addendum or not a.persist_dir:
            ap.error("compact-addendum requires --addendum and --persist-dir")
        compact_addendum(a.addendum, a.persist_dir)
        return 0
    if not a.vault:
        ap.error("%s requires --vault" % a.cmd)
    if a.cmd == "stats":
        cmd_stats(a.vault)
        return 0
    vj = os.path.join(a.vault, CAP_NAME)
    vc = os.path.join(a.vault, CACHE_SUB)
    nv = np_ = 0
    if a.captures:
        add_v, add_p = merge_captures(vj, a.captures)
        nv += add_v; np_ += add_p
    if a.addendum:
        add_v, add_p = merge_addendum(vj, a.addendum)
        nv += add_v; np_ += add_p
    nd = merge_cache(vc, a.cache) if a.cache else 0
    print("coverage_vault: +%d new variant(s), +%d new PC(s), +%d new DLL(s) -> %s" % (nv, np_, nd, a.vault))
    return 0

if __name__ == "__main__":
    sys.exit(main())

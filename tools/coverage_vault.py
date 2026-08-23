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
  coverage_vault.py compact --vault DIR --output compacted.json
  coverage_vault.py compact --vault DIR --apply
                            [--prune-cache] [--prune-stale-temp]
                            [--drop-invalid-evidence]
  coverage_vault.py compact-addendum --addendum captures.addendum.jsonl
                                      --persist-dir IMMUTABLE_DIR
"""
import argparse, base64, json, os, shutil, hashlib, sys, contextlib, re, time

CAP_NAME = "overlay_captures.json"
CACHE_SUB = "cache"
EVIDENCE_FIELDS = ("executed_pcs", "dispatch_entry_pcs",
                   "static_dispatch_entry_pcs", "function_entry_pcs", "seeds")

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

def _iter_json_array(path, chunk_size=1024 * 1024):
    """Stream objects from a top-level JSON array without loading the file.

    Legacy Tomba 2 manifests exceeded 1 GiB and contain tens of millions of PC
    strings. json.load() expands those strings several-fold in memory, which is
    exactly why interrupted merges left multi-gigabyte temporary files behind.
    """
    decoder = json.JSONDecoder()
    with open(path, encoding="utf-8") as source:
        buffer = ""
        eof = False

        def fill():
            nonlocal buffer, eof
            chunk = source.read(chunk_size)
            if chunk:
                buffer += chunk
            else:
                eof = True

        fill()
        while not buffer.strip() and not eof:
            fill()
        start = len(buffer) - len(buffer.lstrip())
        if start >= len(buffer) or buffer[start] != "[":
            raise ValueError("root is not a JSON array: %s" % path)
        buffer = buffer[start + 1:]
        expect_value = True
        while True:
            while True:
                stripped = buffer.lstrip()
                buffer = stripped
                if buffer or eof:
                    break
                fill()
            if not buffer:
                raise ValueError("unterminated JSON array: %s" % path)
            if buffer[0] == "]":
                if buffer[1:].strip() or not eof:
                    tail = buffer[1:] + source.read()
                    if tail.strip():
                        raise ValueError("trailing data after JSON array: %s" % path)
                return
            if not expect_value:
                if buffer[0] != ",":
                    raise ValueError("expected ',' between array items: %s" % path)
                buffer = buffer[1:]
                expect_value = True
                continue
            while True:
                try:
                    value, end = decoder.raw_decode(buffer)
                    break
                except json.JSONDecodeError:
                    if eof:
                        raise ValueError("invalid or truncated JSON array: %s" % path)
                    fill()
            if not isinstance(value, dict):
                raise ValueError("capture array item is not an object: %s" % path)
            yield value
            buffer = buffer[end:]
            expect_value = False

def _address(value, field):
    try:
        return int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError):
        raise ValueError("invalid %s address: %r" % (field, value))

def _compact_region(region, drop_invalid_evidence=False):
    """Split one legacy broad capture into execution-evidence page runs.

    The four-byte tail matches write_json_window(): a branch in the last word
    of a retained page needs the next page's delay-slot instruction. Every
    evidence address must lie in the original byte image; malformed input fails
    closed rather than silently losing coverage.
    """
    load = _address(region.get("load_addr"), "load_addr")
    size = region.get("size")
    if not isinstance(size, int) or size <= 0:
        raise ValueError("invalid capture size at 0x%08X: %r" % (load, size))
    try:
        image = base64.b64decode(region.get("bytes_b64", ""), validate=True)
    except Exception as exc:
        raise ValueError("invalid bytes_b64 at 0x%08X: %s" % (load, exc))
    if len(image) != size:
        raise ValueError("capture size mismatch at 0x%08X: size=%d bytes=%d" %
                         (load, size, len(image)))
    # 8 MB dev-RAM aware: titles running the 8MB enhancement stream code into
    # the extended banks (0x200000..0x7FFFFF); the old 2 MB mask corrupted
    # their capture addresses and the bound rejected legitimate regions.
    phys_lo = load & 0x7FFFFF
    phys_hi = phys_lo + size
    if phys_hi > 8 * 1024 * 1024:
        raise ValueError("capture exceeds PSX RAM at 0x%08X" % load)

    parsed = {}
    pages = set()
    evidence_entries = 0
    invalid_evidence = 0
    for field in EVIDENCE_FIELDS:
        values = region.get(field, []) or []
        if not isinstance(values, list):
            raise ValueError("%s is not a list at 0x%08X" % (field, load))
        entries = []
        for value in values:
            address = _address(value, field)
            phys = address & 0x7FFFFF
            if phys < phys_lo or phys >= phys_hi or (phys & 3):
                if not drop_invalid_evidence:
                    raise ValueError(
                        "%s address 0x%08X is outside/alignment-invalid for "
                        "capture 0x%08X+%d (use --drop-invalid-evidence only "
                        "for a known-corrupt legacy vault)" %
                        (field, address, load, size))
                invalid_evidence += 1
                continue
            entries.append((phys, "0x%08X" % address))
            pages.add(phys >> 12)
            evidence_entries += 1
        parsed[field] = entries
    if not pages:
        return [], evidence_entries, invalid_evidence

    ordered = sorted(pages)
    runs = []
    first = previous = ordered[0]
    for page in ordered[1:] + [None]:
        if page is not None and page == previous + 1:
            previous = page
            continue
        run_lo = max(phys_lo, first << 12)
        run_hi = min(phys_hi, ((previous + 1) << 12) + 4)
        compact = {k: v for k, v in region.items()
                   if k not in EVIDENCE_FIELDS and
                      k not in ("load_addr", "size", "bytes_b64")}
        compact["load_addr"] = "0x%08X" % (load + run_lo - phys_lo)
        compact["size"] = run_hi - run_lo
        compact["bytes_b64"] = base64.b64encode(
            image[run_lo - phys_lo:run_hi - phys_lo]).decode("ascii")
        for field in EVIDENCE_FIELDS:
            compact[field] = sorted({text for phys, text in parsed[field]
                                     if run_lo <= phys < run_hi})
        runs.append(compact)
        if page is None:
            break
        first = previous = page
    return runs, evidence_entries, invalid_evidence

def compact_capture_manifest(source, output=None, drop_invalid_evidence=False):
    """Stream, crop, and content-deduplicate a legacy capture manifest."""
    index = {}
    stats = {"source_regions": 0, "source_bytes": 0,
             "source_evidence_entries": 0, "invalid_evidence_entries": 0,
             "dropped_data_regions": 0}
    for region in _iter_json_array(source):
        stats["source_regions"] += 1
        stats["source_bytes"] += int(region.get("size", 0) or 0)
        compacted, evidence_entries, invalid_evidence = _compact_region(
            region, drop_invalid_evidence)
        stats["source_evidence_entries"] += evidence_entries
        stats["invalid_evidence_entries"] += invalid_evidence
        if not compacted:
            stats["dropped_data_regions"] += 1
        for record in compacted:
            key = _variant_key(record)
            if key not in index:
                index[key] = record
                continue
            target = index[key]
            for field in EVIDENCE_FIELDS:
                target[field] = sorted(set(target.get(field, [])) |
                                       set(record.get(field, [])))
    records = sorted(index.values(), key=lambda r: (
        _address(r.get("load_addr"), "load_addr"), _variant_key(r)))
    stats["output_variants"] = len(records)
    stats["output_bytes"] = sum(r["size"] for r in records)
    stats["output_evidence_entries"] = sum(
        len(r.get(field, [])) for r in records for field in EVIDENCE_FIELDS)
    stats["max_output_region"] = max((r["size"] for r in records), default=0)
    if stats["source_evidence_entries"] and not records:
        raise RuntimeError("compaction lost all execution evidence")
    if output:
        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        _atomic_write_json(output, records)
    return records, stats

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

def _print_compact_stats(source, stats):
    print("coverage_vault: compacted analysis for %s" % source)
    print("  regions: %d -> %d (%d data-only dropped)" %
          (stats["source_regions"], stats["output_variants"],
           stats["dropped_data_regions"]))
    print("  bytes:   %d -> %d (saved %d; max output region %d)" %
          (stats["source_bytes"], stats["output_bytes"],
           stats["source_bytes"] - stats["output_bytes"],
           stats["max_output_region"]))
    print("  evidence entries: %d source -> %d deduplicated" %
          (stats["source_evidence_entries"],
           stats["output_evidence_entries"]))
    if stats["invalid_evidence_entries"]:
        print("  invalid legacy evidence dropped: %d" %
              stats["invalid_evidence_entries"])

def _prune_stale_manifest_temps(vault_json, minimum_age_hours=24):
    directory = os.path.dirname(vault_json) or "."
    basename = os.path.basename(vault_json)
    pattern = re.compile(re.escape(basename) + r"\.\d+\.tmp$")
    cutoff = time.time() - minimum_age_hours * 3600
    removed = []
    for name in os.listdir(directory):
        path = os.path.join(directory, name)
        if (pattern.fullmatch(name) and os.path.isfile(path) and
                os.path.getmtime(path) <= cutoff):
            size = os.path.getsize(path)
            os.unlink(path)
            removed.append((path, size))
    return removed

def cmd_compact(vault, output=None, apply=False, prune_cache=False,
                prune_stale_temp=False, stale_temp_hours=24,
                drop_invalid_evidence=False):
    vault = os.path.abspath(vault)
    source = os.path.join(vault, CAP_NAME)
    if not os.path.isfile(source):
        raise ValueError("vault manifest does not exist: %s" % source)
    if apply and output:
        raise ValueError("compact accepts either --apply or --output, not both")
    if not apply and not output:
        raise ValueError("compact requires --output for a preview or --apply")
    if (prune_cache or prune_stale_temp) and not apply:
        raise ValueError("pruning requires --apply")

    if apply:
        with _exclusive_lock(source):
            records, stats = compact_capture_manifest(
                source, drop_invalid_evidence=drop_invalid_evidence)
            _atomic_write_json(source, records)
            removed_temps = (_prune_stale_manifest_temps(
                source, stale_temp_hours) if prune_stale_temp else [])
            cache = os.path.join(vault, CACHE_SUB)
            cache_bytes = 0
            cache_files = 0
            if prune_cache and os.path.isdir(cache):
                for root, _dirs, files in os.walk(cache):
                    for name in files:
                        path = os.path.join(root, name)
                        cache_files += 1
                        cache_bytes += os.path.getsize(path)
                shutil.rmtree(cache)
    else:
        records, stats = compact_capture_manifest(
            source, output, drop_invalid_evidence)
        removed_temps = []
        cache_bytes = cache_files = 0

    _print_compact_stats(source, stats)
    if output:
        print("  preview: %s" % os.path.abspath(output))
    if removed_temps:
        print("  stale temp files removed: %d (%d bytes)" %
              (len(removed_temps), sum(size for _path, size in removed_temps)))
    if cache_files:
        print("  obsolete cache files removed: %d (%d bytes)" %
              (cache_files, cache_bytes))
    return stats

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["merge", "stats", "compact",
                                    "compact-addendum"])
    ap.add_argument("--vault", help="vault directory (kept gitignored/private)")
    ap.add_argument("--captures", help="source overlay_captures.json to merge in")
    ap.add_argument("--addendum", help="append-only overlay capture history (.jsonl)")
    ap.add_argument("--cache", help="source cache dir (e.g. build/cache/<game_id>) to merge in")
    ap.add_argument("--persist-dir", help="immutable capture snapshot directory")
    ap.add_argument("--output", help="write a compacted preview here; leaves vault unchanged")
    ap.add_argument("--apply", action="store_true",
                    help="atomically replace the vault manifest with compacted evidence")
    ap.add_argument("--prune-cache", action="store_true",
                    help="with --apply, remove old content-keyed cache generations")
    ap.add_argument("--prune-stale-temp", action="store_true",
                    help="with --apply, remove old <manifest>.<pid>.tmp files")
    ap.add_argument("--stale-temp-hours", type=float, default=24,
                    help="minimum age for --prune-stale-temp (default: 24)")
    ap.add_argument("--drop-invalid-evidence", action="store_true",
                    help="drop impossible unaligned/out-of-region PCs from a known-corrupt legacy vault")
    a = ap.parse_args()
    if a.cmd == "compact-addendum":
        if not a.addendum or not a.persist_dir:
            ap.error("compact-addendum requires --addendum and --persist-dir")
        compact_addendum(a.addendum, a.persist_dir)
        return 0
    if not a.vault:
        ap.error("%s requires --vault" % a.cmd)
    if a.cmd == "compact":
        try:
            cmd_compact(a.vault, a.output, a.apply, a.prune_cache,
                        a.prune_stale_temp, a.stale_temp_hours,
                        a.drop_invalid_evidence)
        except (ValueError, RuntimeError) as exc:
            ap.error(str(exc))
        return 0
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

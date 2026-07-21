#!/usr/bin/env python3
"""Generic play-free overlay extractor (AOT spike, v2 — full-discovery producers).

Given a game.toml (disc + load_address + text_size), enumerate the disc and emit
a synthetic overlay_captures.json for compile_overlays.py. Producer types:

 1. PS-X EXE files (magic "PS-X EXE") — the load address is self-described in the
    header. Run the recompiler in NORMAL mode (full discovery: jr-$ra boundary
    scan finds FRAMELESS functions overlay-mode's prologue-scan misses) to get the
    seed set, then split the body into overlay-floor-clamped window regions.
    This is the ROBUST, high-coverage producer (Tomba2 MAIN.EXE: 62%->69% shards,
    4.6x more functions than prologue-only overlay mode).

 2. Header-table files ({count:u32, ptr[count]:u32} with in-range pointers) —
    prologue-scan seeds at a delta-swept base. LESS robust (base recovery
    unreliable when header ptrs aren't clean function starts); see plan doc.

 3. Strict 0x800-aligned {id,size} member archives — multiple members must
    independently vote the same link base; mixed members use direct-call roots.

 4. Companion HED/DAT/BNS archives — contiguous packed-sector runs address a
    logical namespace across payload siblings, with the same consensus gate.

Usage:
  extract_generic.py --game-toml <path> --recompiler <psxrecomp-game.exe>
                     --out <captures.json> [--tmp <dir>]
Requires extract_overlays.py (same dir chain) for the DiscReader/ISO9660 walker.
"""
import argparse, os, sys, json, base64, struct, subprocess, tempfile, re, binascii, hashlib
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import extract_overlays as eo
import compile_overlays as co
try:
    import tomllib
except ImportError:
    import tomli as tomllib

def parse_cue_datatrack(cue_path):
    """Return (bin_path, is_raw) for the FIRST data (MODE) track — the ISO9660
    filesystem lives there. eo.parse_cue picks the LAST FILE, which breaks
    multi-bin cues (separate audio-track .bin files after the data track)."""
    cue_dir=os.path.dirname(os.path.abspath(cue_path))
    cur_bin=None
    with open(cue_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m=re.match(r'\s*FILE\s+"([^"]+)"\s+BINARY', line, re.IGNORECASE)
            if m: cur_bin=os.path.join(cue_dir, m.group(1))
            t=re.match(r'\s*TRACK\s+\d+\s+MODE(\d)/(\d+)', line, re.IGNORECASE)
            if t and cur_bin:
                return cur_bin, (int(t.group(2))==2352)
    return eo.parse_cue(cue_path)   # fallback

def prologues(data, base):
    """Return framed function entries, including a proven pre-frame prelude.

    Psy-Q commonly schedules ``lui R`` / ``load ...,off(R)`` before the usual
    ``addiu sp,sp,-N``.  A raw prologue scan consequently starts those functions
    eight bytes late and leaves the real dispatch entry uncovered.  Recover the
    earlier entry only for that exact register-linked pair immediately following
    a previous ``jr ra`` + delay-slot boundary.  Replace (rather than supplement)
    the late prologue root so exact-entry compilation cannot split one function
    into overlapping siblings.

    This deliberately does not scan arbitrary instructions before a prologue:
    the return boundary, LUI/load opcode pair, and matching address register are
    all required data-as-code gates.
    """
    entries=[]
    for off in prologue_offsets(data):
        entry=off
        if off >= 16 and _word(data, off-16) == 0x03E00008:
            lui=_word(data, off-8)
            load=_word(data, off-4)
            lui_rt=(lui >> 16) & 0x1F
            if (lui >> 26) == 0x0F and lui_rt != 0 and \
                    0x20 <= (load >> 26) <= 0x26 and \
                    ((load >> 21) & 0x1F) == lui_rt:
                entry=off-8
        entries.append(base+entry)
    return entries

def prologue_offsets(data):
    """File offsets whose word is an `addiu $sp,$sp,-N` stack-frame prologue."""
    return [off for off in range(0,len(data)-4,4)
            if (struct.unpack_from('<I',data,off)[0]>>16)==0x27BD
            and (struct.unpack_from('<I',data,off)[0]&0x8000)]

def _word(data, off):
    return struct.unpack_from('<I', data, off)[0] if 0 <= off <= len(data)-4 else None

def _dense_local_pointer_table(data, base, off, count=3):
    """True when ``off`` begins a run of position-fixed in-image pointers.

    A word such as ``0x80118CC4`` is technically a decodable MIPS ``lb`` when
    viewed as an instruction, but three adjacent aligned values in the loaded
    image's address range are substantially stronger evidence for a data table.
    In particular, a JAL-shaped word in a dispatcher must not turn such a table
    into a frameless function root.
    """
    hi = base + len(data)
    for index in range(count):
        value = _word(data, off + index * 4)
        if value is None or (value & 3) or not (base <= value < hi):
            return False
    return True

def _callable_boundary(data, off):
    """Strong local evidence that an in-file pointer names a callable entry."""
    w = _word(data, off)
    if w is None or w == 0:
        return False
    if (w >> 16) == 0x27BD and (w & 0x8000):
        return True
    # A return, arbitrary architectural delay slot, and up to six alignment
    # NOPs is the same Psy-Q boundary accepted by overlay exact-entry mode.
    for padding in range(7):
        jr_off = off - 8 - padding*4
        if _word(data, jr_off) != 0x03E00008:
            continue
        if all(_word(data, p) == 0 for p in range(jr_off+8, off, 4)):
            return True
    return False

def pointer_table_targets(data, base):
    """Callable targets from dense in-file function-pointer tables.

    A run needs at least three adjacent in-range pointers, and each promoted
    target independently needs a prologue/return boundary. This deliberately
    refuses isolated pointer-shaped data and mid-function dispatch labels.
    """
    lo, hi = base, base + len(data)
    words = [_word(data, off) for off in range(0, len(data)-3, 4)]
    in_range = lambda value: value is not None and (value & 3) == 0 and lo <= value < hi
    targets = set()
    start = 0
    while start < len(words):
        if not in_range(words[start]):
            start += 1
            continue
        end = start + 1
        while end < len(words) and in_range(words[end]):
            end += 1
        if end - start >= 3:
            for value in words[start:end]:
                if (_callable_boundary(data, value-base) and
                        not _dense_local_pointer_table(data,base,value-base)):
                    targets.add(value)
        start = end
    return targets

def supplemental_callable_seeds(data, base):
    return pointer_table_targets(data, base)

def frameless_leaf_entries(data, base):
    """Return independently bounded functions immediately following returns.

    A previous ``jr ra`` plus its architectural delay slot establishes the
    start boundary (allowing the same six alignment NOPs as exact-entry mode).
    The shared bounded-CFG verifier then requires the candidate itself to reach
    a return without invalid instructions or sequential runway. This recovers
    unreferenced callback leaves without promoting arbitrary return-adjacent
    data; dense local-pointer tables fail that verifier explicitly.
    """
    entries=set()
    for jr_off in range(0,len(data)-12,4):
        if _word(data,jr_off) != 0x03E00008:
            continue
        entry_off=jr_off+8
        padding=0
        while padding<6 and _word(data,entry_off)==0:
            entry_off+=4; padding+=1
        if entry_off>len(data)-4:
            continue
        entry=base+entry_off
        if co.plausible_callable_target(
                data,base,len(data),entry,base+len(data)):
            entries.add(entry)
    return entries

def jal_targets(data):
    """Distinct absolute jal call targets. These are BASE-INDEPENDENT: the target
    is 0x80000000 | (imm26<<2) regardless of where the file loads, because all
    PSX game code lives in the 0x800xxxxx region (PC[31:28] is always 0x8).

    Unconditional `j` targets are deliberately excluded: they are intra-function
    branch destinations, not callable-boundary evidence. On small overlays they
    can outvote the true base by making a nearby instruction resemble a prologue.
    """
    ts=set()
    for off in range(0,len(data)-4,4):
        w=struct.unpack_from('<I',data,off)[0]
        if (w>>26) == 0x03:                        # jal only
            ts.add(0x80000000 | ((w & 0x03FFFFFF) << 2))
    return ts

def direct_jal_roots(data, base):
    """Locally bounded call targets with independent boundary evidence."""
    lo,hi=base,base+len(data)
    return sorted(t for t in jal_targets(data)
                  if lo <= t < hi and (t&3)==0 and
                  not _dense_local_pointer_table(data,base,t-base) and
                  (_callable_boundary(data,t-base) or
                   ((_word(data,t-base) or 0)>>16)==0x27BD))

def bounded_dispatch_fallback(data, base, entries):
    """Bound authoritative mid-function entries to their nearest hosts.

    Header-table exports are real dispatch targets but are commonly switch
    cases rather than function starts.  For a conservative retry, pair each
    with only its nearest preceding prologue and stop that host at the very
    next prologue.  A host without a provable upper boundary is omitted.
    """
    starts=prologues(data,base)
    if len(starts)<2:
        return None
    hosts=set(); dispatch=set(); ranges=set()
    for entry in sorted(set(entries)):
        before=[start for start in starts if start<=entry]
        if not before:
            continue
        host=before[-1]
        end=next((start for start in starts if start>host),None)
        if end is None or not (host<=entry<end):
            continue
        hosts.add(host); dispatch.add(entry); ranges.add((host,end))
    if not hosts or not dispatch:
        return None
    return {
        'function_entry_pcs':[f"0x{addr:08X}" for addr in sorted(hosts)],
        # The prologue hosts and the exported interior targets are both
        # statically established dispatch candidates. Keep the ordinary dispatch
        # field as the complete authority set so the more-specific static
        # provenance below remains a strict subset by construction.
        'dispatch_entry_pcs':[
            f"0x{addr:08X}" for addr in sorted(hosts | dispatch)],
        'static_dispatch_entry_pcs':[
            f"0x{addr:08X}" for addr in sorted(hosts | dispatch)],
        'static_discovery_entry_pcs':[],
        'producer_ranges':[
            {'start':f"0x{lo:08X}",'end':f"0x{hi:08X}"}
            for lo,hi in sorted(ranges)],
        'strict_producer_ranges':True,
    }

def _select_base_vote(hist, trusted_bases=()):
    """Select a sharp vote peak, or an exact independently trusted near-tie.

    A trusted match is useful when two position-fixed files share a load base:
    one call-dense file proves it sharply, while a small sibling has the same
    exact base among its candidates but too few calls to clear the ratio test.
    Require one unique trusted match with at least four votes; otherwise fail
    closed.
    """
    if not hist:
        return None
    top=hist.most_common(2)
    base,score=top[0]
    second=top[1][1] if len(top)>1 else 0
    if score >= 4 and score >= second*1.5:
        return base, score
    trusted=[(base, hist[base]) for base in set(trusted_bases)
             if hist.get(base, 0) >= 4]
    return trusted[0] if len(trusted) == 1 else None

def recover_base(data, ptrs, trusted_bases=()):
    """Recover the verbatim load base of a position-fixed header-table overlay.

    The export-table pointers point at mid-function DISPATCH entries, not
    prologues, so matching them against 0x27BD is useless (0 hits at the true
    base — measured). Instead use jal-target self-consistency: jal targets are
    base-independent, and at the TRUE base the maximum number of intra-overlay
    jal call targets land on a real 0x27BD prologue. This peaks uniquely and
    sharply on the correct base (~2x margin over runner-up on Tomba 1's 22
    overlays). Small call-sparse siblings may additionally match one exact base
    independently proved by another file; ambiguous/no-consensus cases still
    fail closed.

    Returns (base, score) or None when the signal is too weak to trust (safe:
    a skipped overlay is coverage loss, never wrong execution — the runtime's
    per-function code_crc dispatch guard rejects any mis-based shard anyway)."""
    import bisect
    from collections import Counter
    nonnull=[p for p in ptrs if p]
    if not nonnull: return None
    n=len(data)
    # Containment: every export pointer must map to a valid in-file offset when
    # the file loads verbatim at `base`  =>  0 <= ptr-base <= n-4.
    win_hi=min(nonnull)                 # base <= min(ptr)
    win_lo=max(nonnull)-(n-4)           # base >= max(ptr)-(n-4)
    if win_hi < win_lo: return None
    P=prologue_offsets(data)
    if not P: return None
    # For each jal target t and prologue offset o, base (t-o) makes t hit a
    # prologue. Tally votes over bases inside the containment window; the peak
    # base is the one the most distinct jal targets agree on.
    hist=Counter()
    for t in jal_targets(data):
        lo_i=bisect.bisect_left(P, t-win_hi)
        hi_i=bisect.bisect_right(P, t-win_lo)
        for o in P[lo_i:hi_i]:
            hist[t-o]+=1
    if not hist: return None
    return _select_base_vote(hist, trusted_bases)

def recover_raw_base(data, base_lo, base_hi=0x80200000):
    """Recover a position-fixed raw-code blob without an export-pointer table.

    This uses the same base-independent jal-target/prologue self-consistency as
    recover_base(), but has no pointer-containment window. Compensate with a
    deliberately stricter proof: at least eight distinct call targets, a 2x
    margin over the runner-up, and at least 25% of all apparent prologues must
    agree on one RAM-fitting base. This finds call-dense raw siblings such as
    Tomba 2 CRD.BIN/SOP.BIN while rejecting ordinary data files. A false positive
    remains fail-safe at dispatch because every compiled function is code-CRC
    guarded, but the strict gate also avoids wasting shards on data-as-code.
    """
    import bisect
    from collections import Counter
    P=prologue_offsets(data)
    T=jal_targets(data)
    if not P or not T: return None
    hist=Counter()
    max_base=base_hi-len(data)
    for t in T:
        lo_i=bisect.bisect_left(P,t-max_base)
        hi_i=bisect.bisect_right(P,t-base_lo)
        for o in P[lo_i:hi_i]:
            base=t-o
            if (base&3)==0:
                hist[base]+=1
    if not hist: return None
    top=hist.most_common(2)
    base,score=top[0]
    second=top[1][1] if len(top)>1 else 0
    if score < 8 or score < second*2 or score*4 < len(P):
        return None
    return base,score,second

def raw_base_votes(data, base_lo, base_hi=0x80200000):
    """Return jal/prologue base votes for a position-fixed code blob."""
    import bisect
    from collections import Counter
    P=prologue_offsets(data)
    T=jal_targets(data)
    hist=Counter()
    if not P or not T:
        return hist
    max_base=base_hi-len(data)
    for t in T:
        lo_i=bisect.bisect_left(P,t-max_base)
        hi_i=bisect.bisect_right(P,t-base_lo)
        for o in P[lo_i:hi_i]:
            base=t-o
            if (base&3)==0:
                hist[base]+=1
    return hist

def split_indexed_archive(data, alignment=0x800):
    """Recognize a strict ``{id,size}[]`` + aligned-payload archive.

    Mega Man X6's ROCK_X6.BIN uses one monotonically increasing table in the
    first sector.  Payload members follow in table order, each rounded up to a
    0x800-byte boundary.  Require the complete layout to account for the file
    (apart from at most one format trailer sector) so ordinary data cannot
    be mistaken for this container merely because its first words look small.
    """
    if len(data) < alignment*2 or len(data) % alignment:
        return None
    entries=[]
    for off in range(0, alignment, 8):
        ident,size=struct.unpack_from('<II',data,off)
        if ident == 0 and size == 0:
            break
        if ident == 0 or size < 4 or size > 2_000_000:
            return None
        if entries and ident <= entries[-1][0]:
            return None
        entries.append((ident,size))
    if len(entries) < 4:
        return None
    pos=alignment
    members=[]
    for ident,size in entries:
        if pos+size > len(data):
            return None
        members.append((ident,pos,data[pos:pos+size]))
        pos=(pos+size+alignment-1)&~(alignment-1)
    if pos > len(data) or len(data)-pos > alignment:
        return None
    return members

def return_adjacent_framed_entries(data, base):
    """Strong framed roots immediately following a completed function.

    Mixed archive members may contain prologue-shaped data, so a stack-frame
    instruction alone is insufficient. Require a preceding ``jr ra`` and its
    arbitrary architectural delay slot, at most six alignment NOPs, plus an
    in-frame save of ``ra`` before the new function's first control transfer.
    Every stack-relative word save observed in that prefix must fit the frame.
    """
    entries=set()
    for off in prologue_offsets(data):
        word=_word(data,off)
        frame_imm=word&0xFFFF
        frame_size=0x10000-frame_imm
        if not (8 <= frame_size <= 0x400 and (frame_size&3)==0):
            continue
        boundary=False
        for padding in range(7):
            jr_off=off-8-padding*4
            if _word(data,jr_off)!=0x03E00008:
                continue
            if all(_word(data,p)==0 for p in range(jr_off+8,off,4)):
                boundary=True
                break
        if not boundary:
            continue
        saved_ra=False
        consistent=True
        for pc in range(off+4,min(len(data),off+4+12*4),4):
            candidate=_word(data,pc)
            if candidate is None:
                consistent=False
                break
            if _is_control_flow_word(candidate):
                break
            op=(candidate>>26)&0x3F
            rs=(candidate>>21)&0x1F
            rt=(candidate>>16)&0x1F
            if op==0x2B and rs==29:
                displacement=candidate&0xFFFF
                if displacement&0x8000:
                    displacement-=0x10000
                if displacement<0 or displacement+4>frame_size or displacement&3:
                    consistent=False
                    break
                if rt==31:
                    saved_ra=True
        if consistent and saved_ra:
            entries.add(base+off)
    return sorted(entries)

def _is_control_flow_word(word):
    """Use the shared overlay decoder without exposing another public API."""
    return co._is_control_flow(word)

def reachable_static_dispatches(data,base,roots):
    """Dispatch-only PCs plus inspectable indirect-jump provenance."""
    roots=sorted({root for root in roots
                  if base <= root < base+len(data) and (root&3)==0})
    targets=set()
    continuations=set()
    proofs=[]
    producer=((base,base+len(data)),)
    for index,entry in enumerate(roots):
        hard_cap=roots[index+1] if index+1<len(roots) else base+len(data)
        walk=co._walk_overlay_function(
            data,base,len(data),entry,hard_cap,producer,False)
        targets.update(walk['jump_table_targets'])
        continuations.update(walk['call_continuations'])
        proofs.extend(walk['jump_table_proofs'])
    return {
        'dispatch_pcs':sorted(targets|continuations),
        'jump_table_proofs':proofs,
        'call_continuation_pcs':sorted(continuations),
    }

def recover_consensus_members(members, base_lo, base_hi=0x80200000):
    """Recover linked code from members that independently vote shared bases.

    An anchor member needs >=8 agreeing jal/prologue pairs and a 2x vote peak.
    A base becomes trusted only when at least two independent members anchor it.
    Other members may reuse that exact consensus with >=2 agreeing pairs.
    Locally callable direct-JAL targets and return-adjacent framed functions are
    roots. Computed-switch destinations are retained separately as dispatch-only
    evidence; generic pointer tables and arbitrary prologues remain insufficient
    inside a mixed code/data archive.
    """
    from collections import Counter
    if not members:
        return []
    votes=[]
    anchor_support=Counter()
    for ident,off,body in members:
        hist=raw_base_votes(body,base_lo,base_hi)
        votes.append((ident,off,body,hist))
        top=hist.most_common(2)
        if top:
            base,score=top[0]
            second=top[1][1] if len(top)>1 else 0
            if score >= 8 and score >= second*2:
                anchor_support[base]+=1
    trusted={base for base,count in anchor_support.items() if count >= 2}
    if not trusted:
        return []

    recovered=[]
    for ident,off,body,hist in votes:
        ranked=sorted(((hist.get(base,0),base) for base in trusted),reverse=True)
        score,base=ranked[0]
        runner=ranked[1][0] if len(ranked)>1 else 0
        if score < 2 or score < runner*2:
            continue
        direct=direct_jal_roots(body,base)
        if len(direct) < 2:
            continue
        framed=return_adjacent_framed_entries(body,base)
        seeds=sorted(set(direct)|set(framed))
        static_dispatch=reachable_static_dispatches(body,base,seeds)
        recovered.append({
            'id':ident, 'file_offset':off, 'data':body, 'base':base,
            'score':score, 'runner':runner, 'direct_seeds':direct,
            'return_framed_seeds':framed,
            'static_dispatch_pcs':static_dispatch['dispatch_pcs'],
            'jump_table_proofs':static_dispatch['jump_table_proofs'],
            'call_continuation_pcs':static_dispatch['call_continuation_pcs'],
            'seeds':seeds,
        })
    return recovered

def recover_indexed_archive(data, base_lo, base_hi=0x80200000):
    """Recover independently linked code members from a strict archive."""
    members=split_indexed_archive(data)
    return recover_consensus_members(members or (),base_lo,base_hi)

def hed_companion_members(disc, files):
    """Return strict sector-table members from sibling HED/DAT/BNS files.

    Ape Escape stores 32-bit descriptors as ``size_sectors:12 | lba:20``.
    Descriptor runs are contiguous, while the payload namespace concatenates
    same-stem DAT then BNS companions.  Require runs of at least four entries,
    exact sector continuity, sector-aligned companions, and no cross-file member.
    """
    by_upper={p.upper():(p,l,s) for p,l,s in files}
    groups=[]
    for hed_path,hed_lba,hed_size in files:
        if not hed_path.upper().endswith('.HED') or hed_size > 0x10000:
            continue
        stem=hed_path[:-4]
        companions=[]
        for ext in ('.DAT','.BNS'):
            item=by_upper.get((stem+ext).upper())
            if item:
                companions.append(item)
        if not companions or any(size%0x800 for _,_,size in companions):
            continue
        hed=disc.read_file_bytes(hed_lba,hed_size)
        words=[struct.unpack_from('<I',hed,off)[0]
               for off in range(0,len(hed)-3,4)]
        runs=[]; i=0
        while i < len(words):
            word=words[i]; sector=word&0xFFFFF; count=word>>20
            if not count:
                i+=1; continue
            run=[]; expected=sector; j=i
            while j < len(words):
                value=words[j]; at=value&0xFFFFF; span=value>>20
                if not span or at != expected:
                    break
                run.append((j,at,span)); expected+=span; j+=1
            if len(run) >= 4:
                runs.extend(run); i=j
            else:
                i+=1
        if not runs:
            continue

        logical=[]; cursor=0
        for path,lba,size in companions:
            body=disc.read_file_bytes(lba,size)
            logical.append((cursor,cursor+size//0x800,path,body))
            cursor+=size//0x800
        members=[]; seen=set()
        for table_index,sector,count in runs:
            key=(sector,count)
            if key in seen:
                continue
            seen.add(key)
            owner=next((x for x in logical
                        if x[0] <= sector and sector+count <= x[1]),None)
            if owner is None:
                continue
            lo,hi,path,body=owner
            off=(sector-lo)*0x800
            member=body[off:off+count*0x800]
            members.append((table_index,sector*0x800,member))
        if members:
            groups.append({
                'hed_path':hed_path,
                'members':members,
                'consumed':{hed_path.upper(),
                            *(path.upper() for path,_,_ in companions)},
            })
    return groups

def page_aligned_region(base, data):
    """Build a runtime-keyed region and discard only proven all-zero lead pages."""
    page_base=base&~0xFFF
    region=b'\x00'*(base-page_base)+data
    while len(region)>0x1000 and not any(region[:0x1000]):
        page_base+=0x1000
        region=region[0x1000:]
    return page_base,region

def is_psx_exe(data):
    return len(data) >= 0x20 and data[:8] == b'PS-X EXE'

def make_psx_exe(body, load_addr, entry_pc):
    """Wrap positioned bytes for normal-mode discovery only.

    The wrapper is never emitted or loaded by the runtime. Its header merely
    gives the recompiler the independently recovered link base and one locally
    proven direct-call entry while preserving the member bytes verbatim.
    """
    header=bytearray(0x800)
    header[:8]=b'PS-X EXE'
    struct.pack_into('<I',header,0x10,entry_pc)
    struct.pack_into('<I',header,0x18,load_addr)
    struct.pack_into('<I',header,0x1C,len(body))
    return bytes(header)+body

def is_header_table(data):
    if len(data) < 8: return None
    cnt = struct.unpack_from('<I', data, 0)[0]
    if not (1 <= cnt <= 4096) or 4+cnt*4 > len(data): return None
    ptrs = struct.unpack_from(f'<{cnt}I', data, 4)
    inr = sum(1 for p in ptrs if 0x80010000 <= p < 0x80200000 and (p&3)==0)
    nul = sum(1 for p in ptrs if p == 0)
    return cnt if (inr+nul == cnt and inr >= 1) else None

def parse_full_discovery_ranges(lines):
    """Return entries and overlapping alias recipes from normal-mode ranges."""
    seeds=set(); aliases=[]; pending=None
    for line in lines:
        m=re.match(r'F ([0-9A-Fa-f]+)',line)
        if m:
            pending=int(m.group(1),16)
            seeds.add(pending if pending>=0x80000000 else (0x80000000|pending))
            continue
        m=re.match(r'R ([0-9A-Fa-f]+) ([0-9A-Fa-f]+)',line)
        if m and pending is not None:
            lo=int(m.group(1),16); size=int(m.group(2),16)
            entry=pending if pending>=0x80000000 else (0x80000000|pending)
            lo=lo if lo>=0x80000000 else (0x80000000|lo)
            if size and entry != lo and lo <= entry < lo+size:
                aliases.append((entry,lo,lo+size))
            pending=None
    return sorted(seeds),sorted(set(aliases))

def full_discovery_seeds(data, recompiler, tmp):
    """Run NORMAL mode; return discovered entries plus exact alias recipes."""
    exe_path = os.path.join(tmp, 'producer.exe')
    open(exe_path,'wb').write(data)
    out = os.path.join(tmp, f'disc_out_{binascii.crc32(data)&0xFFFFFFFF:08X}')
    os.makedirs(out, exist_ok=True)
    try:
        # errors='replace': the recompiler prints non-ASCII (✓) that would raise
        # a decode error under text=True and lose the (already-written) ranges.
        subprocess.run([recompiler, exe_path, '--out-dir', out],
                       capture_output=True, text=True, errors='replace', timeout=300)
    except Exception as e:
        print(f"    full-discovery failed ({e}); falling back to prologue scan")
        return None,[]
    lines=[]
    for rf in [f for f in os.listdir(out) if f.endswith('.ranges')]:
        lines.extend(open(os.path.join(out,rf), errors='ignore'))
    seeds,aliases=parse_full_discovery_ranges(lines)
    return (seeds,aliases) if seeds else (None,[])

def full_discovery_output_audit_clean(data, tmp):
    """Fail closed when normal mode emitted any unsupported instruction TODO."""
    out=os.path.join(tmp,f'disc_out_{binascii.crc32(data)&0xFFFFFFFF:08X}')
    sources=[]
    if os.path.isdir(out):
        sources=[os.path.join(out,name) for name in os.listdir(out)
                 if name.endswith('.c') and '_full' in name]
    if not sources:
        return False
    for path in sources:
        with open(path,errors='ignore') as f:
            if re.search(r'TODO:[^\n]*?0x[0-9A-Fa-f]{8}:',f.read()):
                return False
    return True

def filter_full_discovery_seeds(body, base, candidates, declared_entry):
    """Quarantine normal mode's unproven image-body fallback entry.

    Normal mode derives interior entries from return/prologue/call/control-flow
    evidence and classifies their reachable extents.  Its one provenance-free
    fallback is the image load address: a backward return scan that reaches the
    beginning promotes that first body word.  Ape MINI2 begins with a pointer
    table there, while its PS-X header declares the real entry later.  Quarantine
    only that unproven body-start fallback; preserving the additive interior set
    is essential because removing selected roots can split otherwise broad
    functions and create native code-range holes.
    """
    return sorted({addr for addr in candidates
                   if addr != base or addr == declared_entry})

def enrich_positioned_member(member, recompiler, tmp):
    """Add normal-mode entries/aliases to a consensus-positioned member.

    Direct JAL targets established the member's link base. Those targets plus
    strict return-adjacent framed roots remain the hard-root fallback. Normal
    mode contributes decoder-classified candidates and exact overlapping alias
    recipes; the unproven image-start fallback is removed.
    Downstream overlay compilation still re-walks and audits every emitted byte.
    """
    body=member['data']; base=member['base']
    direct=member['direct_seeds']
    roots=member['seeds']
    if not direct:
        return member
    wrapped=make_psx_exe(body,base,direct[0])
    discovered,aliases=full_discovery_seeds(wrapped,recompiler,tmp)
    if discovered is None or not full_discovery_output_audit_clean(wrapped,tmp):
        out=dict(member)
        out['normal_rejected']='normal generated-C audit'
        return out
    discovered=filter_full_discovery_seeds(
        body,base,discovered,direct[0])
    hi=base+len(body)
    aliases=[alias for alias in aliases
             if base <= alias[0] < hi and
             base <= alias[1] < alias[2] <= hi and
             alias[0] != base and
             not any(alias[1] < root < alias[2] for root in roots)]
    out=dict(member)
    out['seeds']=sorted(set(discovered)|set(roots))
    out['static_alias_ranges']=sorted(set(aliases))
    out['normal_seed_count']=len(discovered)
    return out

def rec(load_addr, data, seeds, dispatch_extra=None, producer_ranges=None,
        static_discovery=None, static_alias_ranges=None,
        jump_table_proofs=None, call_continuations=None):
    # function_entry_pcs = prologue-scanned function starts (walk roots).
    # dispatch_entry_pcs = those PLUS any extra dispatch targets (e.g. the
    # overlay's own export table). static_dispatch_entry_pcs records this full
    # extractor-proven dispatch set (both scanner roots and table targets).
    # Keeping the provenance separate lets
    # compile_overlays nominate an address in sibling byte variants without
    # granting the same authority to a runtime-observed dispatch PC.
    # compile_overlays promotes clean starts to entries and mid-function
    # dispatch targets to DISPATCH_INTERIOR aliases (impossible_entry_start
    # guards garbage), so passing the game's authoritative dispatch table here
    # recovers indirect-only entries a prologue scan misses.
    static_dispatch = sorted(set(seeds) | set(dispatch_extra or ()))
    disp = static_dispatch
    out={"schema":"psxrecomp overlay capture v2","load_addr":f"0x{load_addr:08X}",
         "size":len(data),"bytes_b64":base64.b64encode(data).decode(),
         "executed_pcs":[],"dispatch_entry_pcs":[f"0x{a:08X}" for a in disp],
         "static_dispatch_entry_pcs":[
             f"0x{a:08X}" for a in static_dispatch],
         "function_entry_pcs":[f"0x{a:08X}" for a in seeds],
         "seeds":[f"0x{a:08X}" for a in seeds]}
    if producer_ranges:
        out["producer_ranges"]=[
            {"start":f"0x{lo:08X}","end":f"0x{hi:08X}"}
            for lo,hi in producer_ranges]
    if static_discovery:
        out["static_discovery_entry_pcs"]=[
            f"0x{addr:08X}" for addr in sorted(set(static_discovery))]
    if static_alias_ranges:
        out["static_alias_ranges"]=[
            {"entry":f"0x{entry:08X}", "start":f"0x{lo:08X}",
             "end":f"0x{hi:08X}"}
            for entry,lo,hi in sorted(set(static_alias_ranges))]
    if jump_table_proofs:
        out['static_jump_table_proofs']=[{
            'host_entry':f"0x{proof['host_entry']:08X}",
            'jr_pc':f"0x{proof['jr_pc']:08X}",
            'table_base':f"0x{proof['table_base']:08X}",
            'count':proof['count'],
            'targets':[f"0x{target:08X}" for target in proof['targets']],
        } for proof in jump_table_proofs]
    if call_continuations:
        out['static_call_continuation_pcs']=[
            f"0x{addr:08X}" for addr in sorted(set(call_continuations))]
    return out

def add_consensus_fallback(record,member):
    """Fail closed to base-proving direct calls if enrichment is rejected."""
    if (member.get('normal_seed_count') or
            member.get('return_framed_seeds') or
            member.get('static_dispatch_pcs')):
        record['optional_enrichment_fallback_entry_pcs'] = [
            f"0x{addr:08X}" for addr in member['direct_seeds']]
    return record

def bios_resident_records(bios_path=None, manifest_path=None):
    """Build guarded captures for BIOS-installed RAM code known by exact hash.

    Some BIOS helpers are assembled or patched into RAM during boot rather than
    copied as one contiguous ROM range.  They therefore cannot be emitted by
    the ordinary ROM->RAM relocation path.  The manifest records only verified
    code words and dispatch entries for an exact BIOS SHA-256.  Any gap in a
    declared image is inert zero fill, is outside ``producer_ranges``, and can never
    become a discovery root.  compile_overlays.py still emits its normal
    per-function live-byte CRC guards, so a different install image fails closed
    to the interpreter.
    """
    here=os.path.dirname(os.path.abspath(__file__))
    if manifest_path is None:
        manifest_path=os.path.join(here, 'bios_resident_code.json')
    if bios_path is None:
        bios_path=os.environ.get('PSXRECOMP_BIOS_ROM') or os.path.abspath(
            os.path.join(here, '..', '..', 'bios', 'SCPH1001.BIN'))
    if not os.path.isfile(manifest_path) or not os.path.isfile(bios_path):
        return []

    with open(bios_path, 'rb') as f:
        bios_sha=hashlib.sha256(f.read()).hexdigest().lower()
    with open(manifest_path, encoding='utf-8') as f:
        manifest=json.load(f)
    if manifest.get('schema') != 'psxrecomp bios resident code v1':
        raise ValueError(f'{manifest_path}: unsupported BIOS resident manifest schema')

    def number(value, label):
        try:
            return value if isinstance(value, int) else int(str(value), 0)
        except (TypeError, ValueError) as e:
            raise ValueError(f'{manifest_path}: invalid {label}: {value!r}') from e

    records=[]
    for image in manifest.get('images', []):
        if str(image.get('bios_sha256', '')).lower() != bios_sha:
            continue
        start=number(image.get('region_start'), 'region_start')
        size=number(image.get('region_size'), 'region_size')
        if (start & 3) or size <= 0 or size > 0x10000 or (size & 3):
            raise ValueError(f'{manifest_path}: resident region must be '
                             'word-aligned, word-sized, and at most 64 KiB')
        end=start+size
        data=bytearray(size)
        written={}
        producer_ranges=[]

        def put_word(addr, word, label):
            if (addr & 3) or not (start <= addr <= end-4):
                raise ValueError(f'{manifest_path}: {label} address 0x{addr:08X} '
                                 'is outside/alignment-invalid for its region')
            word &= 0xFFFFFFFF
            if addr in written and written[addr] != word:
                raise ValueError(f'{manifest_path}: conflicting resident word at '
                                 f'0x{addr:08X}')
            written[addr]=word
            struct.pack_into('<I', data, addr-start, word)

        for frag in image.get('code_fragments', []):
            addr=number(frag.get('address'), 'code fragment address')
            words=[number(w, 'code word') for w in frag.get('words', [])]
            if not words:
                raise ValueError(f'{manifest_path}: empty resident code fragment')
            for i,word in enumerate(words):
                put_word(addr+i*4, word, 'code fragment')
            producer_ranges.append((addr,addr+len(words)*4))
        for item in image.get('data_words', []):
            put_word(number(item.get('address'), 'data word address'),
                     number(item.get('word'), 'data word'), 'data word')

        entries=sorted({number(v, 'dispatch entry')
                        for v in image.get('dispatch_entry_pcs', [])})
        code_words={addr for lo,hi in producer_ranges for addr in range(lo,hi,4)}
        if not entries or any(entry not in code_words for entry in entries):
            raise ValueError(f'{manifest_path}: every resident dispatch entry must '
                             'name a listed code word')
        record=rec(start, bytes(data), [], dispatch_extra=entries,
                   producer_ranges=producer_ranges)
        record['producer']='bios_resident_manifest'
        record['bios_sha256']=bios_sha
        record['producer_name']=str(image.get('name', 'BIOS resident code'))
        records.append(record)
    return records

def enforce_bios_resident_policy(required, disabled, records):
    """Fail loudly when a release extraction requires resident coverage."""
    if required and disabled:
        raise ValueError('--require-bios-resident conflicts with '
                         '--no-bios-resident')
    if required and not records:
        raise ValueError('--require-bios-resident found no recipe matching the '
                         'resolved BIOS ROM (missing file or unsupported hash)')

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--game-toml')
    ap.add_argument('--recompiler')
    ap.add_argument('--out', required=True)
    ap.add_argument('--tmp', default=None)
    ap.add_argument('--bios', default=None,
                    help='BIOS ROM for exact-hash resident-code recipes (default: '
                         'PSXRECOMP_BIOS_ROM or framework bios/SCPH1001.BIN)')
    ap.add_argument('--bios-resident-manifest', default=None,
                    help='resident-code recipe JSON (default: bundled manifest)')
    ap.add_argument('--no-bios-resident', action='store_true',
                    help='omit exact-hash BIOS-installed RAM code captures')
    ap.add_argument('--require-bios-resident', action='store_true',
                    help='fail if no exact-hash BIOS resident recipe is emitted')
    ap.add_argument('--only-bios-resident', action='store_true',
                    help='emit only exact-hash BIOS-installed RAM code captures')
    a=ap.parse_args()
    if a.require_bios_resident and a.no_bios_resident:
        ap.error('--require-bios-resident conflicts with --no-bios-resident')
    if a.only_bios_resident and a.no_bios_resident:
        ap.error('--only-bios-resident conflicts with --no-bios-resident')
    resident_preflight=None
    if a.require_bios_resident:
        resident_preflight=bios_resident_records(
            a.bios, a.bios_resident_manifest)
    try:
        enforce_bios_resident_policy(
            a.require_bios_resident, a.no_bios_resident,
            resident_preflight or [])
    except ValueError as error:
        ap.error(str(error))
    if a.only_bios_resident:
        records=(resident_preflight if resident_preflight is not None else
                 bios_resident_records(a.bios, a.bios_resident_manifest))
        with open(a.out, 'w') as f:
            json.dump(records, f)
        print(f"BIOS resident: {len(records)} regions -> {a.out}")
        return
    if not a.game_toml or not a.recompiler:
        ap.error('--game-toml and --recompiler are required unless '
                 '--only-bios-resident is used')
    doc=tomllib.loads(open(a.game_toml, encoding='utf-8-sig').read())  # utf-8-sig strips BOM
    game=doc.get('game',doc)
    root=os.path.dirname(os.path.abspath(a.game_toml))
    disc_rel=game.get('disc') or doc.get('game',{}).get('disc')
    load_addr=int(str(game.get('load_address','0x80010000')),16)
    text_size=int(str(game.get('text_size','0x00080000')),16)
    floor = (load_addr + text_size) & 0x1FFFFFFF
    floor_page = (floor // 0x1000) * 0x1000        # page-align down
    disc=os.path.join(root, disc_rel)
    print(f"game={game.get('id')} disc={os.path.basename(disc)} load=0x{load_addr:08X} floor=0x{floor_page:08X}")
    bp,raw=parse_cue_datatrack(disc)   # multi-bin safe: pick track-1 data bin
    dr=eo.DiscReader(bp,raw=raw)
    files=list(eo.enumerate_files(dr))
    hed_groups=hed_companion_members(dr,files)
    hed_consumed=set().union(*(group['consumed'] for group in hed_groups)) \
        if hed_groups else set()
    tmp=a.tmp or tempfile.mkdtemp()
    os.makedirs(tmp, exist_ok=True)
    # The boot EXE (game.toml `exe`) is the STATIC-recompiled base, NOT an overlay.
    exe_base=os.path.basename(str(game.get('exe',''))).upper()
    records=[]; np=nh=nr=na=ne=nc=0; seen_crc=set(); header_files=[]; raw_files=[]
    positioned=[]
    for p,l,s in sorted(files):
        if p.upper() in hed_consumed:
            continue
        if s<8 or s>2_000_000: continue
        if exe_base and os.path.basename(p).upper()==exe_base:
            continue   # skip the statically-compiled base executable
        data=dr.read_file_bytes(l,s)
        # Position-fixed overlays are reused verbatim across AREA folders; dedup
        # by content so identical bytes -> one shard set, not N (same as hand tool).
        dcrc=binascii.crc32(data)&0xFFFFFFFF
        if dcrc in seen_crc: continue
        seen_crc.add(dcrc)
        if is_psx_exe(data):
            entry_pc=struct.unpack_from('<I',data,0x10)[0]
            t_addr=struct.unpack_from('<I',data,0x18)[0]
            body=data[2048:]
            seeds_all,aliases_all=full_discovery_seeds(data, a.recompiler, tmp)
            if seeds_all is None:
                seeds_all=prologues(body, t_addr)
            else:
                seeds_all=filter_full_discovery_seeds(
                    body,t_addr,seeds_all,entry_pc)
            # split body into floor-clamped window regions
            base=t_addr & 0x1FFFFFFF
            end=base+len(body)
            cut = floor_page if base < floor_page < end else None
            spans=[(base, cut)] if cut else [(base,end)]
            if cut: spans.append((cut,end))
            for lo,hi in spans:
                seg=body[lo-base:hi-base]
                va=0x80000000|lo
                sd=[x for x in seeds_all if lo<=(x&0x1FFFFFFF)<hi]
                # Normal-mode entries are already decoder/classifier output.
                # Leave them as ordinary captured candidates so overlay mode
                # can preserve overlapping alias groups. Marking them as hard
                # STATIC_DISCOVERY_ROOT call roots truncates broad owners and
                # makes a clean build lose native code-range coverage.
                span_hi=va+len(seg)
                aliases=[alias for alias in aliases_all
                         if va <= alias[0] < span_hi and
                         va <= alias[1] < alias[2] <= span_hi and
                         (alias[0] != t_addr or alias[0] == entry_pc)]
                records.append(rec(
                    va,seg,sd,static_alias_ranges=aliases))
            np+=1
            print(f"  [PS-X EXE] {p}: {len(body)}B @0x{0x80000000|base:08X}, full-disc seeds={len(seeds_all)}")
        else:
            cnt=is_header_table(data)
            if cnt:
                ptrs=struct.unpack_from(f'<{cnt}I',data,4)
                header_files.append((p, data, ptrs))
            else:
                raw_files.append((p, data))

    # Resolve call-dense files first. A small sibling that does not clear the
    # peak-margin test may then reuse one exact base independently proved by a
    # strong file, but only when that trusted candidate is unique.
    resolved=[]; weak=[]
    for p,data,ptrs in header_files:
        rb=recover_base(data, ptrs)
        if rb is None:
            weak.append((p,data,ptrs))
        else:
            resolved.append((p,data,ptrs,rb,'jal-fit'))
    trusted_bases={rb[0] for _,_,_,rb,_ in resolved}
    for p,data,ptrs in weak:
        rb=recover_base(data, ptrs, trusted_bases)
        if rb is None:
            print(f"  [header-table] {p}: base signal too weak, SKIPPED (safe)")
        else:
            resolved.append((p,data,ptrs,rb,'jal-fit+cross-file'))

    for p,data,ptrs,rb,fit_method in sorted(resolved):
        # jal-call-target self-consistency base recovery (robust; supersedes
        # the old prologue-density delta-sweep, which matched export pointers
        # against prologues they never point at -> wrong base by 2-16KB).
        base,score=rb
        prologue_seeds=prologues(data,base)
        frameless=frameless_leaf_entries(data,base)
        supplemental=supplemental_callable_seeds(data,base)
        seeds=sorted(set(prologue_seeds)|frameless|supplemental)
        # SUPPLEMENT the prologue seeds with the overlay's OWN export/dispatch
        # table (the header pointers) — the game's authoritative list of live
        # entry points, sitting on the disc (play-free). These are exactly the
        # indirect-dispatch-only / interior entries a prologue scan can't see;
        # compile_overlays emits them as DISPATCH_INTERIOR aliases as needed.
        hdr_entries=[p for p in ptrs
                     if p and (p&3)==0 and base <= p < base+len(data)]
        # The runtime keys shards by a PAGE-ALIGNED region_start (the first
        # dirty page of the overlay's contiguous run), and compile_overlays
        # takes the capture's load_addr verbatim as that key (phys = load_addr
        # & 0x1FFFFFFF, no re-align). So we must present a page-aligned base
        # with a FILL prefix bridging page_base -> file base, exactly as the
        # played-vault layout for these position-fixed overlays. Seeds are
        # absolute so they are unaffected; region bytes stay byte-identical.
        page_base = base & ~0xFFF
        fill = base - page_base
        region = b'\x00'*fill + data
        direct=direct_jal_roots(data,base)
        record=rec(page_base, region, seeds, dispatch_extra=hdr_entries,
                   static_discovery=direct)
        fallback=bounded_dispatch_fallback(data,base,hdr_entries)
        if fallback:
            # The on-disc export table is authoritative dispatch evidence but
            # its entries are commonly mid-function.  On audit rejection,
            # classify those entries from scratch without retaining broad
            # prologue/pointer guesses or trusting unrooted JAL-shaped data.
            record['optional_enrichment_fallback_entry_pcs'] = fallback
        records.append(record)
        positioned.append((p,data,base,seeds,hdr_entries))
        nh+=1
        extra=len(set(hdr_entries)-set(seeds))
        print(f"  [header-table] {p}: {len(data)}B file@0x{base:08X} "
              f"region@0x{page_base:08X}(+{fill}) "
              f"({fit_method} score={score}), prologue seeds={len(prologue_seeds)} "
              f"+{len(frameless-set(prologue_seeds))} bounded frameless leaves "
              f"+{len(supplemental-set(prologue_seeds))} pointer-table seeds "
              f"+{extra} export-table dispatch entries")

    # Some engines keep call-dense position-fixed code in raw siblings whose
    # leading table mixes pointers with strings/offsets, so the strict
    # {count,ptr[]} recognizer correctly refuses to call it an export table.
    # Recover only raw blobs with a much stronger unbounded jal-fit proof.
    raw_base_lo=0x80000000|floor_page
    for p,data in raw_files:
        rb=recover_raw_base(data, raw_base_lo)
        if rb is not None:
            base,score,second=rb
            prologue_seeds=prologues(data,base)
            frameless=frameless_leaf_entries(data,base)
            supplemental=supplemental_callable_seeds(data,base)
            seeds=sorted(set(prologue_seeds)|frameless|supplemental)
            page_base,region=page_aligned_region(base,data)
            direct=direct_jal_roots(data,base)
            record=rec(page_base,region,seeds,static_discovery=direct)
            if direct and set(seeds) != set(direct):
                record['optional_enrichment_fallback_entry_pcs'] = [
                    f"0x{addr:08X}" for addr in direct]
            records.append(record)
            positioned.append((p,data,base,seeds,()))
            nr+=1
            print(f"  [raw-code] {p}: {len(data)}B file@0x{base:08X} "
                  f"region@0x{page_base:08X} "
                  f"(strict jal-fit score={score}, runner-up={second}), "
                  f"prologue seeds={len(prologue_seeds)} "
                  f"+{len(frameless-set(prologue_seeds))} bounded frameless leaves "
                  f"+{len(supplemental-set(prologue_seeds))} pointer-table seeds")
            continue

        for member in recover_indexed_archive(data,raw_base_lo):
            member=enrich_positioned_member(member,a.recompiler,tmp)
            base=member['base']; body=member['data']
            page_base,region=page_aligned_region(base,body)
            producer_lo=max(base,page_base)
            record=rec(
                page_base,region,member['seeds'],
                dispatch_extra=member.get('static_dispatch_pcs'),
                producer_ranges=((producer_lo,base+len(body)),),
                static_discovery=member['direct_seeds'],
                static_alias_ranges=member.get('static_alias_ranges'),
                jump_table_proofs=member.get('jump_table_proofs'),
                call_continuations=member.get('call_continuation_pcs'))
            add_consensus_fallback(record,member)
            record['producer_name']=(
                f"{p}:member_{member['id']:04X}@{member['file_offset']:08X}")
            records.append(record)
            na+=1
            print(f"  [indexed-archive] {p} member 0x{member['id']:X}: "
                  f"{len(body)}B file+0x{member['file_offset']:X} "
                  f"@0x{base:08X}, region@0x{page_base:08X}, "
                  f"trusted-fit={member['score']}:{member['runner']}, "
                  f"direct seeds={len(member['direct_seeds'])}, "
                  f"framed roots={len(member.get('return_framed_seeds',()))}, "
                  f"static dispatch={len(member.get('static_dispatch_pcs',()))}, "
                  f"normal seeds={member.get('normal_seed_count',0)}, "
                  f"aliases={len(member.get('static_alias_ranges',()))}, "
                  f"normal reject={member.get('normal_rejected','none')}, "
                  f"total seeds={len(member['seeds'])}")

    for group in hed_groups:
        for member in recover_consensus_members(
                group['members'],raw_base_lo):
            base=member['base']; body=member['data']
            # Some HED archives mirror ordinary PS-X EXE files verbatim. The
            # self-describing producer above already emits their body at the
            # header-declared address; do not also emit the staging buffer that
            # includes the 0x800-byte executable header.
            if is_psx_exe(body):
                continue
            page_base,region=page_aligned_region(base,body)
            producer_lo=max(base,page_base)
            record=rec(
                page_base,region,member['seeds'],
                dispatch_extra=member.get('static_dispatch_pcs'),
                producer_ranges=((producer_lo,base+len(body)),),
                static_discovery=member['direct_seeds'],
                jump_table_proofs=member.get('jump_table_proofs'),
                call_continuations=member.get('call_continuation_pcs'))
            add_consensus_fallback(record,member)
            record['producer_name']=(
                f"{group['hed_path']}:entry_{member['id']:04X}"
                f"@logical_{member['file_offset']:08X}")
            records.append(record)
            ne+=1
            print(f"  [HED archive] {group['hed_path']} entry "
                  f"0x{member['id']:X}: {len(body)}B "
                  f"logical+0x{member['file_offset']:X} @0x{base:08X}, "
                  f"region@0x{page_base:08X}, "
                  f"trusted-fit={member['score']}:{member['runner']}, "
                  f"direct seeds={len(member['direct_seeds'])}, "
                  f"framed roots={len(member.get('return_framed_seeds',()))}, "
                  f"static dispatch={len(member.get('static_dispatch_pcs',()))}")

    # Runtime dirty-page capture coalesces directly adjacent producers into one
    # region keyed by the earlier page. Emit each provable two-file composition
    # as an additional variant so the later file's functions are registered
    # under that live region key. The bytes are an exact disc concatenation and
    # per-function code CRCs still guard every dispatch. Tomba 2's GAME.BIN ends
    # exactly at the common A*/SOP base; standalone records remain necessary for
    # sessions where either producer appears under its own page key.
    composite_keys=set()
    for lp,ldata,lbase,lseeds,ldispatch in positioned:
        for rp,rdata,rbase,rseeds,rdispatch in positioned:
            if lbase >= rbase or lbase+len(ldata) != rbase:
                continue
            page_base=lbase&~0xFFF
            region=b'\x00'*(lbase-page_base)+ldata+rdata
            key=(page_base,binascii.crc32(region)&0xFFFFFFFF)
            if key in composite_keys: continue
            composite_keys.add(key)
            seeds=sorted(set(lseeds)|set(rseeds))
            dispatch=sorted(set(ldispatch)|set(rdispatch))
            records.append(rec(
                page_base,region,seeds,dispatch_extra=dispatch,
                producer_ranges=((lbase,lbase+len(ldata)),
                                 (rbase,rbase+len(rdata)))))
            nc+=1
            print(f"  [composite] {lp} + {rp}: {len(region)}B "
                  f"region@0x{page_base:08X} (exact adjacent producers), "
                  f"seeds={len(seeds)}")
    nb=0
    if not a.no_bios_resident:
        resident=(resident_preflight if resident_preflight is not None else
                  bios_resident_records(a.bios, a.bios_resident_manifest))
        records.extend(resident)
        nb=len(resident)
        for record in resident:
            print(f"  [BIOS resident] {record['producer_name']}: "
                  f"{record['size']}B @0x{int(record['load_addr'],16):08X}, "
                  f"dispatch entries={len(record['dispatch_entry_pcs'])}, "
                  f"sha256={record['bios_sha256'][:12]}...")
    with open(a.out, 'w') as f:
        json.dump(records, f)
    print(f"producers: {np} PS-X EXE (full-discovery), {nh} header-table, "
          f"{nr} raw-code, {na} indexed-archive members, "
          f"{ne} HED-companion members, "
          f"{nc} adjacent composites, {nb} BIOS resident; "
          f"{len(records)} regions -> {a.out}")

if __name__=='__main__': main()

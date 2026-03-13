#!/usr/bin/env python3
"""
Find functions in tomba_full.c that are split pieces missing fallthrough calls.
V3: Filter OUT functions that end with "return; /* jr $ra */" since those are
legitimate function returns. The REAL broken split pieces have NO jr $ra and
just fall off the end of the function.

Key insight: On MIPS, a function return is "jr $ra" (jump to return address).
The recompiler emits this as: return;  /* jr $ra */
If a function has this, it's a complete function that properly returns.
If a function does NOT have this, it should be falling through to the next piece.
"""

import re
import sys

def parse_functions(filepath):
    """Parse all functions from the generated C file."""
    functions = []
    current_func = None
    current_lines = []
    brace_depth = 0
    in_function = False
    start_line = 0

    with open(filepath, 'r') as f:
        for line_no, line in enumerate(f, 1):
            m = re.match(r'^void (func_[0-9A-F]+)\(CPUState\* cpu\)', line)
            if m:
                if current_func and current_lines:
                    functions.append((current_func, current_lines[:], start_line))
                current_func = m.group(1)
                current_lines = [line]
                start_line = line_no
                brace_depth = 0
                in_function = False
                continue

            if current_func:
                current_lines.append(line)
                brace_depth += line.count('{') - line.count('}')
                if '{' in line and not in_function:
                    in_function = True
                if in_function and brace_depth == 0:
                    functions.append((current_func, current_lines[:], start_line))
                    current_func = None
                    current_lines = []
                    in_function = False

    if current_func and current_lines:
        functions.append((current_func, current_lines[:], start_line))

    return functions

def get_func_address(name):
    m = re.match(r'func_([0-9A-Fa-f]+)', name)
    if m:
        return int(m.group(1), 16)
    return None

def extract_metadata(lines):
    full = ''.join(lines[:10])
    size_match = re.search(r'Size:\s*(\d+)\s*bytes', full)
    blocks_match = re.search(r'Blocks:\s*(\d+)', full)
    size = int(size_match.group(1)) if size_match else None
    blocks = int(blocks_match.group(1)) if blocks_match else None
    return size, blocks

def has_jr_ra(lines):
    """Check if function has a 'jr $ra' return anywhere."""
    full = ''.join(lines)
    return 'jr $ra' in full

def ends_with_func_call(lines):
    """Check if the function body ends with a func_XXXXXXXX(cpu) call."""
    for line in reversed(lines):
        stripped = line.strip()
        if not stripped or stripped == '}' or stripped == '{':
            continue
        if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            continue
        if stripped == ';  /* label compatibility: C requires a statement after the last label */':
            continue
        if 'block_' in stripped and stripped.endswith(':'):
            continue
        return bool(re.search(r'func_[0-9A-F]+\(cpu\);', stripped))
    return False

def has_func_call_in_body(lines):
    """Check if there's any func_ call in the body (excluding override dispatch)."""
    for line in lines:
        stripped = line.strip()
        if 'psx_override_dispatch' in stripped:
            continue
        if re.search(r'\bfunc_[0-9A-F]+\(cpu\)', stripped):
            return True
    return False

def has_goto_in_body(lines):
    for line in lines:
        if re.search(r'\bgoto\b', line):
            return True
    return False

def get_body_statements(lines):
    stmts = []
    past_header = False
    for line in lines:
        stripped = line.strip()
        if stripped == '{':
            past_header = True
            continue
        if not past_header:
            continue
        if stripped == '}':
            continue
        if not stripped:
            continue
        if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            continue
        if 'psx_override_dispatch' in stripped:
            continue
        if stripped == ';  /* label compatibility: C requires a statement after the last label */':
            continue
        if stripped.startswith('block_') and stripped.endswith(':'):
            continue
        stmts.append(stripped)
    return stmts

def main():
    filepath = "F:/Projects/psxrecomp-v2/generated/tomba_full.c"

    print(f"Parsing {filepath}...")
    functions = parse_functions(filepath)
    print(f"Found {len(functions)} functions")

    # Build address-sorted lookup
    func_by_addr = {}
    for name, lines, start_line in functions:
        addr = get_func_address(name)
        if addr:
            func_by_addr[addr] = (name, lines, start_line)

    sorted_addrs = sorted(func_by_addr.keys())

    results = []

    for addr in sorted_addrs:
        name, lines, start_line = func_by_addr[addr]
        size, blocks = extract_metadata(lines)

        if size is None or blocks is None:
            continue

        # Check if next function exists at exactly addr + size
        next_expected_addr = addr + size
        if next_expected_addr not in func_by_addr:
            continue

        next_name = func_by_addr[next_expected_addr][0]

        # Skip if it already chains to next piece
        if ends_with_func_call(lines):
            continue

        # KEY FILTER: Skip if function has "jr $ra" (legitimate return)
        if has_jr_ra(lines):
            continue

        # At this point: function has a next piece at addr+size, does NOT call it,
        # and does NOT have a jr $ra return. This is a broken split piece.

        has_calls = has_func_call_in_body(lines)
        has_gotos = has_goto_in_body(lines)
        stmts = get_body_statements(lines)

        # Classify confidence
        if not has_calls and not has_gotos:
            confidence = 'HIGH'
        elif has_gotos and not has_calls:
            confidence = 'MEDIUM'
        elif has_calls:
            confidence = 'MEDIUM'
        else:
            confidence = 'LOW'

        results.append({
            'addr': addr,
            'name': name,
            'size': size,
            'blocks': blocks,
            'start_line': start_line,
            'next_func': next_name,
            'next_addr': next_expected_addr,
            'confidence': confidence,
            'has_calls': has_calls,
            'has_gotos': has_gotos,
            'num_stmts': len(stmts),
            'last_stmt': stmts[-1] if stmts else '(empty)',
        })

    # Sort by confidence then address
    results.sort(key=lambda x: (
        {'HIGH': 0, 'MEDIUM': 1, 'LOW': 2}[x['confidence']],
        x['addr']
    ))

    # Print results
    for conf in ['HIGH', 'MEDIUM', 'LOW']:
        group = [r for r in results if r['confidence'] == conf]
        if not group:
            continue

        print(f"\n{'='*80}")
        print(f"  {conf} CONFIDENCE - {len(group)} functions")
        print(f"  (No jr $ra, no fallthrough call, next func at addr+size)")
        print(f"{'='*80}\n")

        for r in group:
            print(f"  {r['name']} -> should call -> {r['next_func']}")
            print(f"    Size: {r['size']}B, Blocks: {r['blocks']}, Line: {r['start_line']}")
            print(f"    Stmts: {r['num_stmts']}, gotos: {r['has_gotos']}, calls: {r['has_calls']}")
            print(f"    Last: {r['last_stmt'][:120]}")
            print()

    total = len(results)
    high = len([r for r in results if r['confidence'] == 'HIGH'])
    med = len([r for r in results if r['confidence'] == 'MEDIUM'])
    low = len([r for r in results if r['confidence'] == 'LOW'])
    print(f"\nTotal: {total} ({high} HIGH, {med} MEDIUM, {low} LOW)")

    # Print compact HIGH+MEDIUM list
    print(f"\n{'='*80}")
    print("COMPACT LIST (HIGH + MEDIUM):")
    print(f"{'='*80}")
    for r in results:
        if r['confidence'] in ('HIGH', 'MEDIUM'):
            marker = '*' if r['confidence'] == 'HIGH' else ' '
            print(f"  {marker} {r['name']}  ->  {r['next_func']}   (line {r['start_line']}, {r['size']}B, blk={r['blocks']})")

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Find functions in tomba_full.c that are split pieces missing fallthrough calls.
V2: Better heuristics to distinguish split pieces from standalone functions.

Key insight: A function that is a "split piece" (part of a larger PS1 function)
will have the next function at exactly (address + size) bytes later. If the gap
between this function's address and the next function equals the Size, then they
are consecutive pieces of the same original function.

Additionally, a split piece that CORRECTLY chains will have a call like:
    func_XXXXXXXX(cpu);
at the end. A BROKEN split piece will NOT have this call.

We also need to check if the NEXT function is itself a split piece that could
be the continuation. If the next function's first instructions look like they
continue from the previous function's context (using same registers, etc.),
that's another signal.

The strongest signal: if function A at address X with size S has NO branch/call
at the end, and function B exists at address X+S, then A should have called B.
"""

import re
import sys

def parse_functions(filepath):
    """Parse all functions from the generated C file, extracting full body."""
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
    """Extract Size and Blocks from function header comment."""
    full = ''.join(lines[:10])  # Only check first 10 lines for metadata
    size_match = re.search(r'Size:\s*(\d+)\s*bytes', full)
    blocks_match = re.search(r'Blocks:\s*(\d+)', full)
    size = int(size_match.group(1)) if size_match else None
    blocks = int(blocks_match.group(1)) if blocks_match else None
    return size, blocks

def ends_with_func_call(lines):
    """Check if the function body ends with a func_XXXXXXXX(cpu) call."""
    # Look at the last few meaningful lines
    for line in reversed(lines):
        stripped = line.strip()
        if not stripped or stripped == '}' or stripped == '{':
            continue
        if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            continue
        if stripped == ';  /* label compatibility: C requires a statement after the last label */':
            continue
        # Found a meaningful line
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
    """Check if the function has any goto statements."""
    for line in lines:
        if re.search(r'\bgoto\b', line):
            return True
    return False

def has_return_in_body(lines):
    """Check for return statements NOT part of the override dispatch pattern."""
    in_override = False
    for line in lines:
        stripped = line.strip()
        if 'psx_override_dispatch' in stripped:
            in_override = True
            continue
        if in_override and 'return' in stripped:
            in_override = False
            continue
        if re.match(r'^\s*return\s*;', stripped):
            return True
    return False

def get_body_statements(lines):
    """Get non-trivial body statements (excluding boilerplate)."""
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
        if stripped == 'if (psx_override_dispatch(cpu, 0x':
            continue
        if stripped.startswith('return') and 'psx_override' not in stripped:
            # A non-override return
            pass
        if stripped == ';  /* label compatibility: C requires a statement after the last label */':
            continue
        if stripped.startswith('block_'):
            continue
        stmts.append(stripped)
    return stmts

def main():
    filepath = "F:/Projects/psxrecomp-v2/generated/tomba_full.c"

    print(f"Parsing {filepath}...")
    functions = parse_functions(filepath)
    print(f"Found {len(functions)} functions")

    # Build address-sorted list
    func_by_addr = {}
    for name, lines, start_line in functions:
        addr = get_func_address(name)
        if addr:
            func_by_addr[addr] = (name, lines, start_line)

    sorted_addrs = sorted(func_by_addr.keys())
    addr_index = {addr: i for i, addr in enumerate(sorted_addrs)}

    # For each function, check if it's a broken split piece
    results = []

    for addr in sorted_addrs:
        name, lines, start_line = func_by_addr[addr]
        size, blocks = extract_metadata(lines)

        if size is None or blocks is None:
            continue

        # Key check: does the next function start at exactly addr + size?
        next_expected_addr = addr + size

        if next_expected_addr not in func_by_addr:
            # No function at the expected continuation address
            # This function might be standalone, or the continuation is
            # at a different split boundary. Skip for now.
            continue

        next_name = func_by_addr[next_expected_addr][0]

        # The next function exists at exactly the continuation address.
        # Now check: does this function chain to it?
        if ends_with_func_call(lines):
            # Good - it already chains to the next piece
            continue

        if has_func_call_in_body(lines):
            # It calls SOME function but doesn't end with a call to the next piece.
            # This could be a legitimate function that calls helpers.
            # Still could be buggy if those calls are in the middle and it
            # should also fall through at the end.
            # Let's check more carefully...
            pass

        if has_goto_in_body(lines):
            # Has internal control flow. Could be complete, or could need
            # fallthrough after some branches.
            # These are more complex - mark as lower confidence.
            pass

        # Check for explicit return in body (not override)
        has_ret = has_return_in_body(lines)

        stmts = get_body_statements(lines)

        # Classify
        has_calls = has_func_call_in_body(lines)
        has_gotos = has_goto_in_body(lines)

        # Determine confidence
        if not has_calls and not has_gotos and not has_ret:
            # Pure linear code, no calls, no branches, no returns
            # Just falls off the end -> VERY likely a broken split
            confidence = 'HIGH'
        elif has_calls and not has_gotos:
            # Has some calls but no branching and falls off the end
            confidence = 'MEDIUM'
        elif has_gotos and not has_calls:
            # Has branches but no function calls - could be a complete
            # function with conditionals, or a broken split with jumps
            confidence = 'LOW'
        elif has_gotos and has_calls:
            # Has both branches and calls
            confidence = 'LOW'
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
            'has_return': has_ret,
            'num_stmts': len(stmts),
            'last_stmt': stmts[-1] if stmts else '(empty)',
        })

    # Sort by confidence then address
    results.sort(key=lambda x: (
        {'HIGH': 0, 'MEDIUM': 1, 'LOW': 2}[x['confidence']],
        x['addr']
    ))

    # Print results grouped by confidence
    for conf in ['HIGH', 'MEDIUM', 'LOW']:
        group = [r for r in results if r['confidence'] == conf]
        if not group:
            continue

        print(f"\n{'='*80}")
        print(f"  {conf} CONFIDENCE - {len(group)} functions")
        print(f"  (Next function exists at exactly addr+size, but no fallthrough call)")
        print(f"{'='*80}\n")

        for r in group:
            print(f"  {r['name']} -> {r['next_func']}")
            print(f"    Size: {r['size']}B, Blocks: {r['blocks']}, Line: {r['start_line']}")
            print(f"    Statements: {r['num_stmts']}, gotos: {r['has_gotos']}, calls: {r['has_calls']}, return: {r['has_return']}")
            print(f"    Last: {r['last_stmt'][:100]}")
            print()

    total = len(results)
    high = len([r for r in results if r['confidence'] == 'HIGH'])
    med = len([r for r in results if r['confidence'] == 'MEDIUM'])
    low = len([r for r in results if r['confidence'] == 'LOW'])
    print(f"\nTotal: {total} ({high} HIGH, {med} MEDIUM, {low} LOW)")

    # Print compact list of HIGH confidence for easy copy-paste
    print(f"\n{'='*80}")
    print("HIGH CONFIDENCE - compact list (function -> missing_call_to):")
    print(f"{'='*80}")
    for r in results:
        if r['confidence'] == 'HIGH':
            print(f"  {r['name']}  ->  {r['next_func']}   (line {r['start_line']}, {r['size']}B)")

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Find functions in tomba_full.c that are split pieces missing fallthrough calls.
V4: Also filter out BIOS call stubs (call_by_address + return) which are complete
    functions, not broken splits.
"""

import re
import sys

def parse_functions(filepath):
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
    full = ''.join(lines)
    return 'jr $ra' in full

def has_call_by_address(lines):
    """Check if function contains call_by_address (BIOS trap)."""
    for line in lines:
        if 'call_by_address' in line:
            return True
    return False

def ends_with_func_call(lines):
    for line in reversed(lines):
        stripped = line.strip()
        if not stripped or stripped == '}' or stripped == '{':
            continue
        if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            continue
        if stripped == ';  /* label compatibility: C requires a statement after the last label */':
            continue
        if stripped.startswith('block_') and stripped.endswith(':'):
            continue
        if stripped == '/* nop */  /* 0x':
            continue
        if '/* nop */' in stripped:
            continue
        return bool(re.search(r'func_[0-9A-F]+\(cpu\);', stripped))
    return False

def has_func_call_in_body(lines):
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

def get_last_meaningful_line(lines):
    """Get the last meaningful statement (not boilerplate)."""
    for line in reversed(lines):
        stripped = line.strip()
        if not stripped or stripped == '}' or stripped == '{':
            continue
        if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            continue
        if stripped == ';  /* label compatibility: C requires a statement after the last label */':
            continue
        if stripped.startswith('block_') and stripped.endswith(':'):
            continue
        if '/* nop */' in stripped:
            continue
        return stripped
    return '(empty)'

def main():
    filepath = "F:/Projects/psxrecomp-v2/generated/tomba_full.c"

    print(f"Parsing {filepath}...")
    functions = parse_functions(filepath)
    print(f"Found {len(functions)} functions")

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

        next_expected_addr = addr + size
        if next_expected_addr not in func_by_addr:
            continue

        next_name = func_by_addr[next_expected_addr][0]

        # Skip if already chains to next piece
        if ends_with_func_call(lines):
            continue

        # Skip if has jr $ra (legitimate return)
        if has_jr_ra(lines):
            continue

        # Skip if has call_by_address (BIOS trap stub - complete function)
        if has_call_by_address(lines):
            continue

        has_calls = has_func_call_in_body(lines)
        has_gotos = has_goto_in_body(lines)
        last_line = get_last_meaningful_line(lines)

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
            'last_line': last_line,
        })

    results.sort(key=lambda x: (
        {'HIGH': 0, 'MEDIUM': 1, 'LOW': 2}[x['confidence']],
        x['addr']
    ))

    for conf in ['HIGH', 'MEDIUM', 'LOW']:
        group = [r for r in results if r['confidence'] == conf]
        if not group:
            continue

        print(f"\n{'='*80}")
        print(f"  {conf} CONFIDENCE - {len(group)} functions")
        print(f"  (No jr $ra, no call_by_address, no fallthrough, next func at addr+size)")
        print(f"{'='*80}\n")

        for r in group:
            print(f"  {r['name']} -> should call -> {r['next_func']}")
            print(f"    Size: {r['size']}B, Blocks: {r['blocks']}, Line: {r['start_line']}")
            print(f"    gotos: {r['has_gotos']}, calls: {r['has_calls']}")
            print(f"    Last: {r['last_line'][:120]}")
            print()

    total = len(results)
    high = len([r for r in results if r['confidence'] == 'HIGH'])
    med = len([r for r in results if r['confidence'] == 'MEDIUM'])
    low = len([r for r in results if r['confidence'] == 'LOW'])
    print(f"\nTotal: {total} ({high} HIGH, {med} MEDIUM, {low} LOW)")

    # Compact list for use in fixes
    print(f"\n{'='*80}")
    print("COMPACT LIST — these functions need fallthrough calls added:")
    print(f"{'='*80}")
    for r in results:
        if r['confidence'] in ('HIGH', 'MEDIUM'):
            marker = 'H' if r['confidence'] == 'HIGH' else 'M'
            print(f"  [{marker}] {r['name']}  ->  {r['next_func']}   (line {r['start_line']}, {r['size']}B)")

    # Also identify chains (consecutive broken pieces that form a longer chain)
    print(f"\n{'='*80}")
    print("CHAINS — consecutive broken pieces of the same original function:")
    print(f"{'='*80}")

    result_names = set(r['name'] for r in results)
    visited = set()

    for r in results:
        if r['name'] in visited:
            continue
        chain = [r['name']]
        visited.add(r['name'])
        current = r['next_func']
        while current in result_names and current not in visited:
            chain.append(current)
            visited.add(current)
            # Find the result for current
            for r2 in results:
                if r2['name'] == current:
                    current = r2['next_func']
                    break
            else:
                break
        chain.append(current)  # Final target (the function they all should eventually reach)

        if len(chain) > 2:
            print(f"\n  Chain of {len(chain)-1} broken pieces:")
            for i, name in enumerate(chain):
                prefix = "  -> " if i > 0 else "     "
                is_broken = name in result_names
                marker = " [BROKEN]" if is_broken else " [TARGET]"
                print(f"    {prefix}{name}{marker}")
        else:
            print(f"  {chain[0]} -> {chain[1]}")

if __name__ == '__main__':
    main()

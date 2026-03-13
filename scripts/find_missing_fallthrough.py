#!/usr/bin/env python3
"""
Find functions in tomba_full.c that are split pieces missing fallthrough calls.

A "missing fallthrough" function:
1. Has "Blocks: 1" in its header comment (small, single-block function)
2. Does NOT end with a call to another func_XXXXXXXX (no fallthrough chain)
3. Does NOT end with a goto (no branch to another block)
4. Does NOT contain a proper return path (jr $ra equivalent)

These are recompiler split pieces where the first piece should chain into
the next piece but doesn't.
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

    with open(filepath, 'r') as f:
        for line_no, line in enumerate(f, 1):
            # Detect function start
            m = re.match(r'^void (func_[0-9A-F]+)\(CPUState\* cpu\)', line)
            if m:
                # Save previous function if any
                if current_func and current_lines:
                    functions.append((current_func, current_lines[:]))
                current_func = m.group(1)
                current_lines = [line]
                brace_depth = 0
                in_function = False
                continue

            if current_func:
                current_lines.append(line)
                brace_depth += line.count('{') - line.count('}')
                if '{' in line and not in_function:
                    in_function = True
                # Function ends when brace depth returns to 0
                if in_function and brace_depth == 0:
                    functions.append((current_func, current_lines[:]))
                    current_func = None
                    current_lines = []
                    in_function = False

    # Don't forget last function
    if current_func and current_lines:
        functions.append((current_func, current_lines[:]))

    return functions

def get_func_address(name):
    """Extract hex address from function name like func_8003B860."""
    m = re.match(r'func_([0-9A-Fa-f]+)', name)
    if m:
        return int(m.group(1), 16)
    return None

def analyze_function(name, lines):
    """Analyze a function for the missing-fallthrough bug.

    Returns a dict with analysis results, or None if not suspicious.
    """
    full_text = ''.join(lines)

    # Extract size and blocks from comment
    size_match = re.search(r'Size:\s*(\d+)\s*bytes', full_text)
    blocks_match = re.search(r'Blocks:\s*(\d+)', full_text)

    if not blocks_match:
        return None

    blocks = int(blocks_match.group(1))
    size = int(size_match.group(1)) if size_match else 0

    # We're looking for small functions (Blocks: 1 or very small size)
    # But also check Blocks: 2 with small size as they could be suspicious too
    if blocks > 2:
        return None
    if blocks == 2 and size > 64:
        return None
    if blocks == 1 and size > 128:
        return None

    # Check if function has psx_override_dispatch (indicates it's a real entry point
    # that COULD be a split piece)
    has_override = 'psx_override_dispatch' in full_text

    # Find the last meaningful statement before the closing brace
    # Strip out empty lines, comments, closing braces
    meaningful_lines = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        if stripped == '}':
            continue
        if stripped == '{':
            continue
        if stripped.startswith('//'):
            continue
        if stripped.startswith('/*'):
            continue
        if stripped.startswith('*'):
            continue
        if 'void func_' in stripped:
            continue
        meaningful_lines.append(stripped)

    if not meaningful_lines:
        return None

    last_line = meaningful_lines[-1]

    # Check if function ends with a call to another func_
    ends_with_func_call = bool(re.search(r'func_[0-9A-F]+\(cpu\);', last_line))

    # Check if there's ANY func_ call in the body (not counting the override dispatch)
    body_text = full_text
    # Remove the override dispatch line
    body_no_override = re.sub(r'.*psx_override_dispatch.*\n', '', body_text)
    has_any_func_call = bool(re.search(r'func_[0-9A-F]+\(cpu\)', body_no_override))

    # Check for goto statements (branch within function)
    has_goto = 'goto ' in full_text

    # Check for return statements (other than the override dispatch return)
    # Count returns that aren't part of the override pattern
    returns_in_body = []
    in_override = False
    for line in lines:
        stripped = line.strip()
        if 'psx_override_dispatch' in stripped:
            in_override = True
            continue
        if in_override and 'return' in stripped:
            in_override = False
            continue
        if 'return' in stripped:
            returns_in_body.append(stripped)

    # A function is suspicious if:
    # - It's small (Blocks: 1, or small size)
    # - It does NOT end with a func_ call
    # - It does NOT have gotos (meaning it's truly linear)
    # - The last statement is just a register assignment or memory read/write

    # Not suspicious: ends with a function call
    if ends_with_func_call:
        return None

    # Not suspicious: has gotos (it has internal control flow, likely complete)
    # Actually, we should still check these - a function with gotos but no
    # fallthrough to the next piece is still buggy. But let's focus on the
    # clear cases first.

    # Not suspicious: has return statements in the body (explicit returns)
    # Actually, returns could be from conditional branches. Let's check.

    # Check if last line looks like a register assignment or memory op
    is_assignment = bool(re.match(r'cpu->', last_line))
    is_semicolon_end = last_line.endswith(';')

    # Check if the function has any conditional returns (if (...) return;)
    # These might be legitimate early-return functions

    # For blocks:1 functions that just do assignments and fall off, flag them
    if blocks == 1 and not has_goto and not has_any_func_call:
        # This is the classic pattern - linear code that just falls off
        return {
            'name': name,
            'size': size,
            'blocks': blocks,
            'last_line': last_line,
            'has_goto': has_goto,
            'has_func_call': has_any_func_call,
            'has_override': has_override,
            'returns_in_body': returns_in_body,
            'confidence': 'HIGH' if is_assignment else 'MEDIUM',
            'category': 'linear_no_call'
        }

    # For blocks:1 with gotos but no func call at the end
    if blocks == 1 and has_goto and not has_any_func_call:
        # Has internal branches but never calls next piece
        # Could be a complete small function OR a broken split
        return {
            'name': name,
            'size': size,
            'blocks': blocks,
            'last_line': last_line,
            'has_goto': has_goto,
            'has_func_call': has_any_func_call,
            'has_override': has_override,
            'returns_in_body': returns_in_body,
            'confidence': 'LOW',
            'category': 'has_goto_no_call'
        }

    return None

def check_next_function_exists(func_name, all_func_names):
    """Check if there's a function at the next expected address."""
    addr = get_func_address(func_name)
    if addr is None:
        return None, None

    # The "next piece" would be at some nearby address
    # Check various offsets (typical split sizes are multiples of 4)
    for offset in range(4, 256, 4):
        next_addr = addr + offset
        next_name = f"func_{next_addr:08X}"
        if next_name in all_func_names:
            return next_name, offset

    return None, None

def main():
    filepath = "F:/Projects/psxrecomp-v2/generated/tomba_full.c"

    print(f"Parsing {filepath}...")
    functions = parse_functions(filepath)
    print(f"Found {len(functions)} functions")

    # Build set of all function names for cross-referencing
    all_func_names = set(name for name, _ in functions)

    # Build ordered list of function addresses
    func_addrs = []
    for name, _ in functions:
        addr = get_func_address(name)
        if addr:
            func_addrs.append((addr, name))
    func_addrs.sort()

    # Build map: func_name -> next function in address order
    addr_to_next = {}
    for i in range(len(func_addrs) - 1):
        addr_to_next[func_addrs[i][1]] = func_addrs[i+1]

    # Analyze each function
    suspicious = []
    for name, lines in functions:
        result = analyze_function(name, lines)
        if result:
            # Check if next function exists (suggesting this is a split piece)
            next_func, offset = check_next_function_exists(name, all_func_names)
            result['next_func'] = next_func
            result['next_offset'] = offset

            # Also check what the next function in address order is
            if name in addr_to_next:
                next_addr, next_name = addr_to_next[name]
                gap = next_addr - get_func_address(name)
                result['next_in_order'] = next_name
                result['gap_to_next'] = gap

            suspicious.append(result)

    # Sort by confidence then address
    suspicious.sort(key=lambda x: (
        0 if x['confidence'] == 'HIGH' else (1 if x['confidence'] == 'MEDIUM' else 2),
        get_func_address(x['name'])
    ))

    # Print results
    print(f"\n{'='*80}")
    print(f"SUSPICIOUS FUNCTIONS (potential missing fallthrough)")
    print(f"{'='*80}\n")

    high_count = 0
    medium_count = 0
    low_count = 0

    for r in suspicious:
        if r['confidence'] == 'HIGH':
            high_count += 1
        elif r['confidence'] == 'MEDIUM':
            medium_count += 1
        else:
            low_count += 1

        # Only print HIGH and MEDIUM for now
        if r['confidence'] == 'LOW':
            continue

        print(f"[{r['confidence']}] {r['name']} (Size: {r['size']}B, Blocks: {r['blocks']})")
        print(f"  Last line: {r['last_line']}")
        if r.get('next_func'):
            print(f"  Next piece: {r['next_func']} (offset +{r['next_offset']})")
        if r.get('next_in_order'):
            print(f"  Next in order: {r['next_in_order']} (gap: {r['gap_to_next']}B)")
        if r['returns_in_body']:
            print(f"  Returns in body: {r['returns_in_body']}")
        print()

    print(f"\nSummary: {high_count} HIGH, {medium_count} MEDIUM, {low_count} LOW confidence")
    print(f"Total suspicious: {len(suspicious)}")

    # Now print LOW confidence ones separately
    if low_count > 0:
        print(f"\n{'='*80}")
        print(f"LOW CONFIDENCE (have gotos, might be complete functions)")
        print(f"{'='*80}\n")
        for r in suspicious:
            if r['confidence'] != 'LOW':
                continue
            print(f"  {r['name']} (Size: {r['size']}B, Blocks: {r['blocks']})")
            if r.get('next_in_order'):
                print(f"    Next: {r['next_in_order']} (gap: {r['gap_to_next']}B)")

if __name__ == '__main__':
    main()

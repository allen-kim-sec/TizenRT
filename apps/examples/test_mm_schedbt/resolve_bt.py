#!/usr/bin/env python3
"""
resolve_bt.py - annotate test_mm_schedbt / test_mm_backtrace serial output
                with function names using arm-none-eabi-addr2line.

The firmware prints raw return addresses (bt[N] = 0x...., alloc_call_addr, etc.).
This reads that log and appends "<function>  <file:line>" to every line that
contains a code address, using the debug ELF's symbols.  Run it inside the
Docker container where the toolchain lives.

Usage:
    # from a saved log file
    python3 resolve_bt.py build/output/bin/common_dbg < serial.log

    # or live over the serial console
    cat /dev/ttyUSB1 | python3 resolve_bt.py build/output/bin/common_dbg

    # override the tool (default: arm-none-eabi-addr2line)
    ADDR2LINE=arm-none-eabi-addr2line python3 resolve_bt.py <elf> < serial.log
"""
import os
import re
import subprocess
import sys

# App text lives in the XIP flash window; ignore RAM/heap data addresses so we
# don't try to resolve ptr= values (heap) as code.
CODE_LO = 0x0e000000
CODE_HI = 0x0f000000

ADDR_RE = re.compile(r'0x?([0-9a-fA-F]{7,8})')


def resolve(elf, addrs, tool):
    """Batch-resolve a list of '0x...' address strings to 'func @ file:line'."""
    if not addrs:
        return {}
    try:
        out = subprocess.run(
            [tool, '-f', '-e', elf] + addrs,
            capture_output=True, text=True, check=True).stdout.splitlines()
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        sys.stderr.write("addr2line failed: %s\n" % e)
        return {}
    # addr2line -f emits 2 lines per address: function, then file:line
    result = {}
    for i, a in enumerate(addrs):
        fn = out[2 * i] if 2 * i < len(out) else '??'
        fl = out[2 * i + 1] if 2 * i + 1 < len(out) else '??:0'
        result[a] = "%s  %s" % (fn, fl)
    return result


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 1
    elf = sys.argv[1]
    tool = os.environ.get('ADDR2LINE', 'arm-none-eabi-addr2line')

    for line in sys.stdin:
        line = line.rstrip('\n')
        hits = []
        for m in ADDR_RE.finditer(line):
            val = int(m.group(1), 16)
            if CODE_LO <= val < CODE_HI:
                hits.append('0x%08x' % val)
        if not hits:
            print(line)
            continue
        names = resolve(elf, hits, tool)
        annot = '  |  '.join('%s=%s' % (a, names.get(a, '??')) for a in hits)
        print("%s    <-- %s" % (line, annot))
    return 0


if __name__ == '__main__':
    sys.exit(main())

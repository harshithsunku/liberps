#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Summarise gcov line coverage for the sources under a directory.

usage: covsum.py <build-dir> <src-dir> [min-percent]
Runs gcov on the objects of <src-dir>/*.c built in <build-dir>/obj/<src-dir>/ and prints
per-file and total line coverage. Exits 1 if the total is below min-percent (default 0).
"""
import glob
import os
import re
import subprocess
import sys
import tempfile


def main() -> int:
    build, src = sys.argv[1], sys.argv[2].rstrip("/")
    minimum = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    objdir = os.path.abspath(os.path.join(build, "obj", src))
    total_lines = total_hit = 0
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for c in sorted(glob.glob(os.path.join(src, "*.c"))):
            out = subprocess.run(
                ["gcov", "-n", "-o", objdir, os.path.abspath(c)],
                cwd=os.getcwd(), capture_output=True, text=True, check=False).stdout
            # gcov prints "File '<path>'\nLines executed:XX.XX% of N"
            for m in re.finditer(r"File '([^']+)'\nLines executed:([\d.]+)% of (\d+)", out):
                path, pct, n = m.group(1), float(m.group(2)), int(m.group(3))
                # gcov names the file as compiled (relative to the repository root)
                if os.path.normpath(path) != os.path.normpath(c) and \
                        os.path.abspath(path) != os.path.abspath(c):
                    continue
                hit = round(pct * n / 100.0)
                rows.append((c, pct, hit, n))
                total_lines += n
                total_hit += hit
    for c, pct, hit, n in rows:
        print(f"  {c:40s} {pct:6.2f}%  ({hit}/{n})")
    total = 100.0 * total_hit / total_lines if total_lines else 0.0
    print(f"  {'TOTAL ' + src:40s} {total:6.2f}%  ({total_hit}/{total_lines})")
    if total < minimum:
        print(f"coverage {total:.2f}% below required {minimum:.2f}%")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

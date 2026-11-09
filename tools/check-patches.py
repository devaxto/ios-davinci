#!/usr/bin/env python3
"""Validate xnu/patches/*.patch hunk headers against their real content.

`git apply` silently rejects a malformed patch ("corrupt patch at line N")
and our CI wrapper only *warns* and continues, so a bad hunk header costs a
full CI run and leaves the kernel at an older source state without any
visible failure. This checks every hunk's declared line counts.
"""
import pathlib
import re
import sys

HUNK = re.compile(rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def main(argv):
    root = pathlib.Path(argv[1] if len(argv) > 1 else "xnu/patches")
    bad = 0
    for p in sorted(root.glob("*.patch")):
        data = p.read_bytes()
        lines = data.split(b"\n")
        if lines and lines[-1] == b"":
            lines.pop()  # trailing newline, not a hunk line
        i = 0
        problems = []
        while i < len(lines):
            m = HUNK.match(lines[i])
            if not m:
                i += 1
                continue
            hdr = i + 1  # 1-based line number of the @@ line
            old_decl = int(m.group(2)) if m.group(2) is not None else 1
            new_decl = int(m.group(4)) if m.group(4) is not None else 1
            old_n = new_n = 0
            j = i + 1
            while j < len(lines) and (old_n < old_decl or new_n < new_decl):
                ln = lines[j]
                if ln.startswith(b"\\"):  # "\ No newline at end of file"
                    j += 1
                    continue
                if ln.startswith(b"-"):
                    old_n += 1
                elif ln.startswith(b"+"):
                    new_n += 1
                elif ln.startswith(b" ") or ln == b"" or ln.endswith(b"\r"):
                    # git treats a bare newline inside a hunk as an empty
                    # context line; only accept it while counts are unmet
                    old_n += 1
                    new_n += 1
                else:
                    break
                j += 1
            if (old_n, new_n) != (old_decl, new_decl):
                problems.append(
                    "%s: hunk @@ -%s,%s +%s,%s declares %d/%d, found %d/%d"
                    % (
                        p.name,
                        m.group(1).decode(),
                        old_decl,
                        m.group(3).decode(),
                        new_decl,
                        old_decl,
                        new_decl,
                        old_n,
                        new_n,
                    )
                )
                # resync at next hunk/file header
                while j < len(lines) and not (
                    HUNK.match(lines[j]) or lines[j].startswith(b"--- ")
                ):
                    j += 1
                i = j
                continue
            i = j
        for msg in problems:
            print(msg)
        bad += len(problems)
    if bad:
        print("\n%d corrupt hunk(s)" % bad)
        return 1
    print("all patch hunk headers OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

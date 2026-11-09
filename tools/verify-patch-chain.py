#!/usr/bin/env python3
"""Offline check that a patch chain really applies to the real sources.

`git apply` is atomic per file and our CI wrapper only warns on failure, so a
patch that does not apply costs a full CI run and leaves the kernel silently
at an old source state (this is exactly how 0099+0100 got skipped). This
replays a chain in a scratch tree seeded from git and diffs the result against
the expected files.

  python tools/verify-patch-chain.py 0099 0100

Seeds iokit/Kernel/arm/SweetFramebuffer.cpp from commit 11b0731 (last real
source edit before 0099) plus the matching SweetDisplay sources, applies each
patch with `git apply`, and compares each step to tools/gpu-src/*.<NNNN>.*
snapshots and finally to the live tools/gpu-src files.
"""
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
GPU = REPO / "tools" / "gpu-src"
SCRATCH = GPU / "_chain"
BASE_COMMIT = "11b0731"
REL = "iokit/Kernel/arm"
FILES = ("SweetFramebuffer.cpp", "SweetDisplay.cpp", "SweetDisplay.h",
         "SweetSurface.cpp", "SweetSurface.h")


def run(args, cwd):
    p = subprocess.run(args, cwd=str(cwd), capture_output=True)
    return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


def norm(data):
    return data.replace(b"\r\n", b"\n")


def git_show(path):
    p = subprocess.run(
        ["git", "show", "%s:tools/gpu-src/%s" % (BASE_COMMIT, path)],
        cwd=str(REPO),
        capture_output=True,
    )
    if p.returncode != 0:
        raise SystemExit("git show failed for %s: %s" % (path, p.stderr.decode()))
    return norm(p.stdout)


def main(argv):
    versions = [a.zfill(4) for a in argv[1:]]
    if not versions:
        raise SystemExit(__doc__)

    if SCRATCH.exists():
        shutil.rmtree(SCRATCH)
    (SCRATCH / REL).mkdir(parents=True)
    run(["git", "init", "-q", "."], SCRATCH)

    # seed the real pre-0099 sources (0098 is the last change to SweetDisplay)
    for f in FILES:
        (SCRATCH / REL / f).write_bytes(git_show(f))

    # the 0099 snapshots must equal the pre-0099 sources for the ones 0099
    # does not touch, otherwise 0100's old side is not what CI will see
    for f in ("SweetDisplay.cpp", "SweetDisplay.h"):
        snap = norm((GPU / ("SweetDisplay.0099." + f.split(".")[-1])).read_bytes())
        if snap != (SCRATCH / REL / f).read_bytes():
            print("MISMATCH: SweetDisplay.0099.%s != %s:%s" % (f.split(".")[-1], BASE_COMMIT, f))
            return 1
    print("snapshots for untouched files match %s" % BASE_COMMIT)

    for v in versions:
        patches = sorted((REPO / "xnu" / "patches").glob("%s-*.patch" % v))
        if len(patches) != 1:
            raise SystemExit("expected exactly one patch for %s, found %d" % (v, len(patches)))
        # the Windows worktree is CRLF (autocrlf); CI applies the LF blob, so
        # feed git apply the same LF text it will see on the runner
        local = SCRATCH / patches[0].name
        local.write_bytes(norm(patches[0].read_bytes()))
        rc, out, err = run(
            ["git", "-c", "core.autocrlf=false", "apply", "--whitespace=nowarn", "-v", local.name],
            SCRATCH,
        )
        if rc != 0:
            print("%s FAILED to apply: %s" % (patches[0].name, (err or out).strip()))
            return 1
        print("%s applied" % patches[0].name)

        # compare against the snapshot taken right after this version
        for f in FILES:
            snap = GPU / (pathlib.Path(f).stem + "." + v + pathlib.Path(f).suffix)
            if not snap.exists():
                continue
            want = norm(snap.read_bytes())
            got = norm((SCRATCH / REL / f).read_bytes())
            if want != got:
                print("MISMATCH: result of %s != %s" % (v, snap.name))
                return 1
            print("  %s matches %s" % (f, snap.name))

    # final result must equal the working tree sources
    for f in FILES:
        want = norm((GPU / f).read_bytes())
        got = norm((SCRATCH / REL / f).read_bytes())
        if want != got:
            print("MISMATCH: final result != tools/gpu-src/%s" % f)
            return 1
        print("final %s matches tools/gpu-src" % f)

    print("\nchain OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

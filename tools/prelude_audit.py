#!/usr/bin/env python3
"""Phase-5AG: audit the EFFECTIVE prelude of a split build against the tree.

The shipped binary is compiled from a frozen dispatch image plus a set of
build-time transformations (splices + split-time injections).  A prelude fix
that lives only in nnktool/src/recompile.rs is therefore invisible unless a
transform carries it -- the Phase-5AB (VRAM read shadow) and Phase-5AF (overlay
registry) class of silent regression.

This script checks, for the runtime TU that is about to be compiled:

  required   every marker in tools/nnk_prelude_manifest.txt must be present in
             nnk_main.c or one of the *.inc fragments next to it.  Missing
             markers FAIL the build (build_split.sh runs this before cc).
             The marker must also exist in its canonical tree `source`, so a
             manifest row cannot survive the feature it describes.
  divergent  markers the tree carries and the frozen image predates.  They are
             expected ABSENT; report them so a future phase promotes the row
             after splicing the fix.
  obsolete   markers the frozen image carries and the tree dropped.  Report
             them as stale (probe leftovers), never fatal.

usage: tools/prelude_audit.py --split work/split/c [--spliced id,id,...]
                              [--manifest tools/nnk_prelude_manifest.txt]
exit: 0 = all required features present, 1 = missing feature / rotted manifest
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_MANIFEST = os.path.join(HERE, "nnk_prelude_manifest.txt")
CLASSES = ("required", "divergent", "obsolete")


def die(msg):
    sys.stderr.write("prelude_audit: " + msg + "\n")
    sys.exit(2)


def load_manifest(path):
    rows = []
    seen = set()
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 5:
                die("%s:%d: expected 5 tab-separated columns "
                    "(id, class, marker, source, description), got %d"
                    % (path, n, len(parts)))
            fid, cls, marker, source, desc = parts
            if cls not in CLASSES:
                die("%s:%d: unknown class %r" % (path, n, cls))
            if fid in seen:
                die("%s:%d: duplicate feature id %r" % (path, n, fid))
            seen.add(fid)
            rows.append(dict(id=fid, cls=cls, marker=marker, source=source,
                             desc=desc, line=n))
    if not rows:
        die("manifest %s has no rows" % path)
    return rows


def effective_prelude(split_dir):
    """The runtime TU cc compiles: nnk_main.c + every fragment it #includes."""
    main = os.path.join(split_dir, "nnk_main.c")
    if not os.path.isfile(main):
        die("%s missing (run the split first)" % main)
    text = open(main, encoding="utf-8", errors="replace").read()
    incs = sorted(f for f in os.listdir(split_dir) if f.endswith(".inc"))
    for f in incs:
        text += "\n/* ---- %s ---- */\n" % f
        text += open(os.path.join(split_dir, f), encoding="utf-8",
                     errors="replace").read()
    return text, incs


def tree_contains(rel, marker):
    p = os.path.join(ROOT, rel)
    if not os.path.isfile(p):
        return None
    return marker in open(p, encoding="utf-8", errors="replace").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--split", required=True,
                    help="split output dir (holds nnk_main.c + *.inc)")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--spliced", default="",
                    help="feature ids whose splice ran in this build")
    args = ap.parse_args()

    rows = load_manifest(args.manifest)
    text, incs = effective_prelude(args.split)
    required = [r for r in rows if r["cls"] == "required"]
    divergent = [r for r in rows if r["cls"] == "divergent"]
    obsolete = [r for r in rows if r["cls"] == "obsolete"]

    missing, present = [], []
    for r in required:
        (present if r["marker"] in text else missing).append(r)

    # The manifest must describe a feature the TREE still emits, or it has
    # rotted into a second copy of the image's history.
    rotted = []
    for r in rows:
        got = tree_contains(r["source"], r["marker"])
        if got is None:
            rotted.append((r, "source file %s does not exist" % r["source"]))
        elif r["cls"] == "required" and not got:
            rotted.append((r, "canonical source %s no longer contains the "
                              "marker" % r["source"]))
        elif r["cls"] == "divergent" and not got:
            rotted.append((r, "divergent row is no longer in the tree "
                              "(%s)" % r["source"]))
        elif r["cls"] == "obsolete" and got:
            rotted.append((r, "obsolete marker is back in the tree (%s): "
                              "re-classify the row" % r["source"]))

    ahead = [r for r in divergent if r["marker"] not in text]
    promoted = [r for r in divergent if r["marker"] in text]
    stale = [r for r in obsolete if r["marker"] in text]

    spliced = [s for s in args.spliced.split(",") if s]
    print("== prelude audit: %d required, %d divergent, %d obsolete "
          "(effective prelude: nnk_main.c + %d .inc) =="
          % (len(required), len(divergent), len(obsolete), len(incs)))
    print("prelude: %d/%d features present (spliced: %s)"
          % (len(present), len(required), ",".join(spliced) if spliced else "none"))

    if ahead:
        print("prelude: image predates the tree (not enforced): %s"
              % ", ".join(r["id"] for r in ahead))
        for r in ahead:
            print("  AHEAD    %-28s %-16r %s" % (r["id"], r["marker"], r["desc"]))
    if promoted:
        print("prelude: divergent rows now present -- promote to required: %s"
              % ", ".join(r["id"] for r in promoted))
    if stale:
        print("prelude: stale in the image (the tree dropped these): %s"
              % ", ".join(r["id"] for r in stale))
        for r in stale:
            print("  STALE    %-28s %s" % (r["id"], r["desc"]))

    if rotted:
        sys.stdout.flush()
        print("prelude: MANIFEST IS STALE -- %d row(s) no longer match the tree"
              % len(rotted), file=sys.stderr)
        for r, why in rotted:
            print("  ROTTED   %-28s %s" % (r["id"], why), file=sys.stderr)

    if missing:
        sys.stdout.flush()
        print("prelude: AUDIT FAILED -- %d of %d required features MISSING from "
              "the effective prelude" % (len(missing), len(required)),
              file=sys.stderr)
        for r in missing:
            print("  MISSING  %-28s marker %r not in nnk_main.c or *.inc\n"
                  "           %s" % (r["id"], r["marker"], r["desc"]),
                  file=sys.stderr)
        print("  the frozen image predates this tree feature; splice it "
              "(tools/p5*_splice.py, tools/split_dispatch.py) or regenerate "
              "the image -- see PRELUDE_AUDIT.md", file=sys.stderr)
    if missing or rotted:
        sys.exit(1)
    print("prelude: AUDIT OK")
    sys.exit(0)


if __name__ == "__main__":
    main()

# -*- coding: utf-8 -*-
"""
Measure 涟漪 RippleTrace's extractors against tree-sitter ground truth.

For each file:
  * tree-sitter (a real grammar) counts function definitions and call sites.
  * `dump_ast.exe` runs 涟漪 RippleTrace's own extractor on the same file and prints the
    same counts plus every extracted definition.

Reported per file and as a total:
    def recall  = our defs / tree-sitter defs
    call recall = our call sites that tree-sitter also sees / tree-sitter calls

A recall number below ~0.8 means the impact analysis is missing real edges,
which is exactly the weakness this measurement exists to expose.

Usage:
    python tools/compare_parsers.py <repoDir> [maxFilesPerLang]
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
VENDOR = os.path.join(HERE, "_vendor")
sys.path.insert(0, VENDOR)

import tree_sitter                      # noqa: E402
import tree_sitter_cpp                  # noqa: E402
import tree_sitter_java                 # noqa: E402
import tree_sitter_python               # noqa: E402
from tree_sitter import Language, Parser  # noqa: E402

DUMPER = os.path.join(PROJECT, "build", "dump_ast.exe")

LANGS = {
    ".cpp": ("C++", tree_sitter_cpp.language),
    ".hpp": ("C++", tree_sitter_cpp.language),
    ".h":   ("C++", tree_sitter_cpp.language),
    ".cc":  ("C++", tree_sitter_cpp.language),
    ".java": ("Java", tree_sitter_java.language),
    ".py":  ("Python", tree_sitter_python.language),
}
DEF_TYPES = {
    "C++": {"function_definition"},
    "Java": {"method_declaration", "constructor_declaration"},
    "Python": {"function_definition"},
}
CALL_TYPES = {
    "C++": {"call_expression"},
    "Java": {"method_invocation", "object_creation_expression"},
    "Python": {"call"},
}
SKIP_DIRS = {".git", "build", "_vendor", "__pycache__", "node_modules",
             "out", "target", ".vs", "venv", ".venv", "dist", "_wheelhouse"}


def walk_nodes(node, want, found):
    if node.type in want:
        found.append(node)
    for c in node.children:
        walk_nodes(c, want, found)


def ts_counts(path):
    ext = os.path.splitext(path)[1].lower()
    if ext not in LANGS:
        return None
    name, lang_fn = LANGS[ext]
    parser = Parser(Language(lang_fn()))
    with open(path, "rb") as f:
        src = f.read()
    root = parser.parse(src).root_node
    defs, calls = [], []
    walk_nodes(root, DEF_TYPES[name], defs)
    walk_nodes(root, CALL_TYPES[name], calls)
    return name, len(defs), len(calls), root.has_error


def rippletrace_counts(path):
    """Ask 涟漪 RippleTrace's extractor for its own numbers on one file."""
    r = subprocess.run([DUMPER, path], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return None
    out = r.stdout
    defs = calls = 0
    for line in out.splitlines():
        if line.startswith("DEFS "):
            defs = int(line.split()[1])
        elif line.startswith("CALLS "):
            calls = int(line.split()[1])
    return defs, calls


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root_dir = sys.argv[1]
    per_lang_cap = int(sys.argv[2]) if len(sys.argv) > 2 else 12

    files_by_lang = {}
    for dirpath, dirnames, filenames in os.walk(root_dir):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            p = os.path.join(dirpath, fn)
            ext = os.path.splitext(fn)[1].lower()
            if ext not in LANGS:
                continue
            files_by_lang.setdefault(LANGS[ext][0], []).append(p)

    totals = {}
    print("=" * 76)
    print(" Parser accuracy vs tree-sitter ground truth")
    print("=" * 76)

    for lang in sorted(files_by_lang):
        files = sorted(files_by_lang[lang])[:per_lang_cap]
        t_def = t_call = i_def = i_call = 0
        bad = []
        for p in files:
            ts = ts_counts(p)
            im = rippletrace_counts(p)
            if ts is None or im is None:
                continue
            _, d_ts, c_ts, err = ts
            d_im, c_im = im
            t_def += d_ts
            t_call += c_ts
            i_def += d_im
            i_call += c_im
            if c_ts and c_im / c_ts < 0.8:
                bad.append((os.path.relpath(p, root_dir), d_ts, d_im, c_ts, c_im))

        dr = (i_def / t_def * 100) if t_def else 0.0
        cr = (i_call / t_call * 100) if t_call else 0.0
        totals[lang] = (t_def, i_def, dr, t_call, i_call, cr)

        print("\n[ %s ]  %d file(s)" % (lang, len(files)))
        print("  definitions : tree-sitter %5d   RippleTrace %5d   recall %5.1f%%"
              % (t_def, i_def, dr))
        print("  call sites  : tree-sitter %5d   RippleTrace %5d   recall %5.1f%%"
              % (t_call, i_call, cr))
        if bad:
            print("  worst files (call recall < 80%%):")
            for rel, d1, d2, c1, c2 in sorted(bad, key=lambda x: (x[4] / x[3]) if x[3] else 1)[:5]:
                print("    %-42s calls %4d/%4d = %5.1f%%   defs %3d/%3d"
                      % (rel, c2, c1, (c2 / c1 * 100) if c1 else 0, d2, d1))

    print("\n" + "=" * 76)
    print(" Summary")
    print("=" * 76)
    print("  %-8s %-28s %-28s" % ("lang", "definitions (RippleTrace / ts)", "calls (RippleTrace / ts)"))
    for lang, (td, idf, dr, tc, ic, cr) in sorted(totals.items()):
        print("  %-8s %5d / %-5d  %5.1f%%          %5d / %-5d  %5.1f%%"
              % (lang, idf, td, dr, ic, tc, cr))
    return 0


if __name__ == "__main__":
    sys.exit(main())

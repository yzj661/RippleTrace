# -*- coding: utf-8 -*-
"""
Ground-truth parser probe.

tree-sitter provides real grammars for C++, Java and Python. This script uses
them to count, for a given file, how many function definitions and call
expressions actually exist. Those counts are the reference against which
涟漪 RippleTrace's own extractors can be measured - a claim like "our parser handles
Java" is only meaningful if something independent checks it.
"""
import os
import sys

VENDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_vendor")
sys.path.insert(0, VENDOR)

import tree_sitter                      # noqa: E402
import tree_sitter_cpp                  # noqa: E402
import tree_sitter_java                 # noqa: E402
import tree_sitter_python               # noqa: E402
from tree_sitter import Language, Parser  # noqa: E402

LANGS = {
    ".cpp": ("C++", tree_sitter_cpp.language),
    ".hpp": ("C++", tree_sitter_cpp.language),
    ".h":   ("C++", tree_sitter_cpp.language),
    ".cc":  ("C++", tree_sitter_cpp.language),
    ".java": ("Java", tree_sitter_java.language),
    ".py":  ("Python", tree_sitter_python.language),
}

# Node types that introduce a callable body, per grammar.
DEF_TYPES = {
    "C++": {"function_definition"},
    "Java": {"method_declaration", "constructor_declaration"},
    "Python": {"function_definition"},
}

# Node types that represent a call.
CALL_TYPES = {
    "C++": {"call_expression"},
    "Java": {"method_invocation", "object_creation_expression"},
    "Python": {"call"},
}


def walk(node, want, found, depth=0):
    """Collect nodes whose type is in `want`."""
    if node.type in want:
        found.append(node)
    for child in node.children:
        walk(child, want, found, depth + 1)


def analyse(path):
    ext = os.path.splitext(path)[1].lower()
    if ext not in LANGS:
        print("  unsupported extension:", ext)
        return

    name, lang_fn = LANGS[ext]
    language = Language(lang_fn())
    parser = Parser(language)

    with open(path, "rb") as f:
        src = f.read()

    tree = parser.parse(src)
    root = tree.root_node

    defs, calls = [], []
    walk(root, DEF_TYPES[name], defs)
    walk(root, CALL_TYPES[name], calls)

    errors = []
    walk(root, {"ERROR"}, errors)

    print("  language        : %s" % name)
    print("  parsed          : %s" % ("yes" if not root.has_error else "yes (with ERROR nodes)"))
    print("  function defs   : %d" % len(defs))
    print("  call sites      : %d" % len(calls))
    print("  ERROR nodes     : %d" % len(errors))

    # Show the first few definitions with their names.
    shown = 0
    for d in defs:
        if shown >= 8:
            break
        text = src[d.start_byte:d.end_byte].decode("utf-8", "replace")
        head = text.split("{")[0].split(":")[0].strip().replace("\n", " ")
        if len(head) > 70:
            head = head[:67] + "..."
        print("    def  line %-5d %s" % (d.start_point[0] + 1, head))
        shown += 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: probe_treesitter.py <file> [file...]")
        sys.exit(2)
    for p in sys.argv[1:]:
        print("=== %s ===" % p)
        try:
            analyse(p)
        except Exception as e:
            print("  FAILED:", type(e).__name__, e)
        print()

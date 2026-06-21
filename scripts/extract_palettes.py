#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract the 4 hardcoded palettes from utils/styleconstants.h into themes/*.json.

Order is preserved (JSON objects keep insertion order) so the theme editor can
list tokens in the same order as the header. Verifies values are byte-exact.
"""
import json
import re
import sys
from pathlib import Path
from collections import OrderedDict

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "utils" / "styleconstants.h"
THEMES = ROOT / "themes"

# function name in header -> (theme id, display name)
FUNCS = OrderedDict([
    ("defaultPalette",      ("steam_dark",    "Steam Dark")),
    ("lightPalette",        ("light",         "Light")),
    ("midnightPalette",     ("midnight_blue", "Midnight Blue")),
    ("highContrastPalette", ("high_contrast", "High Contrast")),
])

PAIR_RE = re.compile(
    r'\{\s*QStringLiteral\("([^"]+)"\)\s*,\s*QStringLiteral\("([^"]+)"\)\s*\}'
)


def extract_block(text, func_name):
    # find "func_name()" then the enclosing static const QHash {...};
    m = re.search(re.escape(func_name) + r"\s*\(\s*\)", text)
    if not m:
        raise SystemExit(f"function {func_name} not found")
    brace_start = text.find("{", m.end())          # opening { of function body
    init_start = text.find("{", brace_start + 1)    # opening { of initializer list
    # walk to matching close of the initializer list
    depth = 0
    i = init_start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return text[init_start : i + 1]


def main():
    text = HEADER.read_text(encoding="utf-8")
    THEMES.mkdir(exist_ok=True)
    summary = {}
    for func, (theme_id, display) in FUNCS.items():
        block = extract_block(text, func)
        pairs = PAIR_RE.findall(block)
        od = OrderedDict()
        for k, v in pairs:
            if k in od:
                raise SystemExit(f"duplicate token {k} in {func}")
            od[k] = v
        out = OrderedDict()
        out["_name"] = display
        for k, v in od.items():
            out[k] = v
        path = THEMES / f"{theme_id}.json"
        path.write_text(json.dumps(out, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
        summary[theme_id] = (func, len(od))
        print(f"  {theme_id:14s} <- {func:22s} {len(od)} tokens -> {path.name}")

    # verify: re-read each JSON and compare back against the header block
    print("\nVerification (JSON == header values):")
    ok = True
    for func, (theme_id, _display) in FUNCS.items():
        block = extract_block(text, func)
        header_pairs = OrderedDict(PAIR_RE.findall(block))
        loaded = json.loads((THEMES / f"{theme_id}.json").read_text(encoding="utf-8"),
                            object_pairs_hook=OrderedDict)
        loaded.pop("_name", None)
        same_keys = list(header_pairs.keys()) == list(loaded.keys())
        same_vals = all(header_pairs[k] == loaded[k] for k in header_pairs)
        status = "OK" if (same_keys and same_vals) else "MISMATCH"
        if status != "OK":
            ok = False
        print(f"  {theme_id:14s} keys={same_keys} vals={same_vals} -> {status}")

    # token-set parity across palettes (each theme should cover the default set)
    base_keys = set(json.loads((THEMES / "steam_dark.json").read_text(encoding="utf-8")).keys())
    for theme_id in ("light", "midnight_blue", "high_contrast"):
        keys = set(json.loads((THEMES / f"{theme_id}.json").read_text(encoding="utf-8")).keys())
        missing = base_keys - keys
        extra = keys - base_keys
        print(f"  {theme_id:14s} missing_vs_default={sorted(missing)} extra={sorted(extra)}")

    print("\nDONE" if ok else "\nFAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

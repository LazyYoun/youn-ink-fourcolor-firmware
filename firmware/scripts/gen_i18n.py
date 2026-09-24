#!/usr/bin/env python3
"""Generate firmware/main/common/i18n_strings.{h,cc} from the i18n resource
files (firmware/assets/i18n/{zh-CN,en-US}.json).

Run after editing either JSON file:
    python firmware/scripts/gen_i18n.py

Use --check to verify the generated sources are already up to date (CI):
    python firmware/scripts/gen_i18n.py --check
"""
import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
ASSETS_DIR = REPO_ROOT / "firmware" / "assets" / "i18n"
ZH_PATH = ASSETS_DIR / "zh-CN.json"
EN_PATH = ASSETS_DIR / "en-US.json"
OUT_DIR = REPO_ROOT / "firmware" / "main" / "common"
HEADER_PATH = OUT_DIR / "i18n_strings.h"
SOURCE_PATH = OUT_DIR / "i18n_strings.cc"

SPECIFIER_RE = re.compile(r"%[-+ #0-9.]*[a-zA-Z%]")


def pascal_case(snake_id):
    return "".join(part.capitalize() for part in snake_id.split("_"))


def format_specifiers(s):
    # Treat "%%" as a literal percent, not a conversion.
    return [m for m in SPECIFIER_RE.findall(s) if m != "%%"]


def encode_c_string(value):
    out = []
    for c in value:
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        else:
            out.append(c)
    return '"' + "".join(out) + '"'


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def validate(zh, en):
    zh_keys, en_keys = list(zh.keys()), list(en.keys())
    if set(zh_keys) != set(en_keys):
        missing_en = set(zh_keys) - set(en_keys)
        missing_zh = set(en_keys) - set(zh_keys)
        msg = "zh-CN.json and en-US.json key sets differ."
        if missing_en:
            msg += f" Missing from en-US.json: {sorted(missing_en)}."
        if missing_zh:
            msg += f" Missing from zh-CN.json: {sorted(missing_zh)}."
        raise SystemExit(msg)
    if zh_keys != en_keys:
        raise SystemExit("zh-CN.json and en-US.json must list keys in the same order.")
    for key in zh_keys:
        zh_specs = format_specifiers(zh[key])
        en_specs = format_specifiers(en[key])
        if len(zh_specs) != len(en_specs):
            raise SystemExit(
                f"Format specifier count mismatch for '{key}': "
                f"zh-CN has {zh_specs}, en-US has {en_specs}."
            )


def generate(zh, en):
    keys = list(zh.keys())
    enum_lines = ",\n".join(f"    k{pascal_case(k)}" for k in keys)
    header = f"""/**
 * @file i18n_strings.h
 * @brief GENERATED FILE -- do not edit by hand.
 *
 * Regenerate with `python firmware/scripts/gen_i18n.py` after editing
 * firmware/assets/i18n/zh-CN.json or firmware/assets/i18n/en-US.json.
 */

#ifndef I18N_STRINGS_H
#define I18N_STRINGS_H

#include <cstdint>

namespace i18n {{

enum class StringId : uint16_t {{
{enum_lines}
}};

constexpr size_t kStringCount = {len(keys)};

}}  // namespace i18n

#endif  // I18N_STRINGS_H
"""

    zh_lines = ",\n".join(f"    {encode_c_string(zh[k])}" for k in keys)
    en_lines = ",\n".join(f"    {encode_c_string(en[k])}" for k in keys)
    source = f"""/**
 * @file i18n_strings.cc
 * @brief GENERATED FILE -- do not edit by hand.
 *
 * Regenerate with `python firmware/scripts/gen_i18n.py` after editing
 * firmware/assets/i18n/zh-CN.json or firmware/assets/i18n/en-US.json.
 */

#include "i18n_strings.h"

namespace i18n {{

// `extern` is required here: a const-qualified namespace-scope variable
// has internal linkage by default in C++ (unlike C), so without it this
// definition wouldn't satisfy the `extern` declaration in i18n.cc and the
// link would fail with "undefined reference to i18n::kStringsZhCN/EnUS".
extern const char* const kStringsZhCN[kStringCount] = {{
{zh_lines}
}};

extern const char* const kStringsEnUS[kStringCount] = {{
{en_lines}
}};

}}  // namespace i18n
"""
    return header, source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                         help="Verify generated sources are up to date; don't write.")
    args = parser.parse_args()

    zh, en = load(ZH_PATH), load(EN_PATH)
    validate(zh, en)
    header, source = generate(zh, en)

    if args.check:
        ok = True
        if not HEADER_PATH.exists() or HEADER_PATH.read_text(encoding="utf-8") != header:
            print(f"stale: {HEADER_PATH}")
            ok = False
        if not SOURCE_PATH.exists() or SOURCE_PATH.read_text(encoding="utf-8") != source:
            print(f"stale: {SOURCE_PATH}")
            ok = False
        if not ok:
            return 1
        print("i18n generated sources are up to date.")
        return 0

    HEADER_PATH.write_text(header, encoding="utf-8")
    SOURCE_PATH.write_text(source, encoding="utf-8")
    print(f"Generated {HEADER_PATH} and {SOURCE_PATH} ({len(zh)} strings).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

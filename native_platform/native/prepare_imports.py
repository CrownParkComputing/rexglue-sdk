#!/usr/bin/env python3
"""Emit explicit failures for imports not implemented by the native runtime.

These handlers never fabricate successful results. Exercised imports must be
implemented before a native port can pass its gameplay acceptance checks.
"""
import json
import re
import sys
from pathlib import Path

source, runtime, output = map(Path, sys.argv[1:])
required = set()
for header in source.glob("*/*_funcs*.h"):
    required.update(re.findall(r"DECLARE_REX_FUNC\((__imp__\w+)\)", header.read_text()))
provided = set()
for path in runtime.rglob("*.cpp"):
    text = path.read_text()
    provided.update(re.findall(r"XN_(?:IMPORT(?:_RAW)?|STUB(?:_RETURN)?)\((__imp__\w+)", text))
    provided.update("__imp__" + name for name in re.findall(r"^XN_UI_STUB\((\w+)\)", text, re.M))
    if path.name == "imports_audio.cpp":
        provided.update("__imp__" + name for name in re.findall(r"^\s+X\((\w+)\)", text, re.M))
missing = sorted(required - provided)
output.parent.mkdir(parents=True, exist_ok=True)
text = '#include "kernel/kernel.h"\n#include <rex/hook.h>\n'
for name in missing:
    text += f'XN_IMPORT_RAW({name}) {{ xn::Fatal(std::format("Unsupported native import {name} lr={{:#010x}} r3={{:#010x}} r4={{:#010x}} r5={{:#010x}} r6={{:#010x}}", uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32)); }}\n'
if not output.exists() or output.read_text() != text:
    output.write_text(text)
output.with_suffix(".json").write_text(json.dumps({"required": len(required), "provided": len(required & provided), "unsupported": missing}, indent=2) + "\n")
print(f"Native import audit: {len(required & provided)}/{len(required)} handlers; {len(missing)} explicit unsupported handlers")

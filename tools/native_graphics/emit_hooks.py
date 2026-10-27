#!/usr/bin/env python3
"""Emit guarded native graphics hooks from audited ReXGlue function metadata.

Generated title C++ is read locally for identity checks and is never copied
into the output. Unknown/changed bodies fail rather than silently binding an
address to a different ABI. The runtime experiment is disabled by default.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tomllib

PROFILES = {
    "forza4_float4": ("forza4_constant_setter.h", "TryForzaConstantSetter", False),
    "mcla_float4": ("constant_setter.h", "TryMclaConstantSetter", True),
    "grouped_float4_emitter": ("constant_emitter.h", "TryConstantEmitter", False),
}


def write_changed(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def function_hashes(generated, wanted):
    found = {}
    start_pattern = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)\s*\{", re.M)
    for path in sorted(generated.glob("*_recomp.*.cpp")):
        source = path.read_text()
        starts = list(start_pattern.finditer(source))
        for index, match in enumerate(starts):
            address = int(match[1], 16)
            if address not in wanted:
                continue
            if address in found:
                raise ValueError(f"Duplicate function {address:08X}")
            end = starts[index + 1].start() if index + 1 < len(starts) else len(source)
            found[address] = hashlib.sha256(source[match.start():end].strip().encode()).hexdigest()
    return found


def emit(generated, manifest_path, output):
    metadata = tomllib.loads(manifest_path.read_text())
    if metadata.get("version") != 1:
        raise ValueError("Expected audited native graphics metadata version = 1")
    hooks = metadata.get("hook", [])
    if not hooks:
        raise ValueError("No audited native graphics hooks")
    wanted = set()
    for hook in hooks:
        address, profile = hook.get("address"), hook.get("profile")
        if not isinstance(address, int) or not 0 < address <= 0xFFFFFFFF or address & 3:
            raise ValueError("Invalid hook address")
        if address in wanted or profile not in PROFILES:
            raise ValueError(f"Duplicate address or unknown profile: {address:08X}")
        wanted.add(address)
        if profile == "grouped_float4_emitter":
            if hook.get("entry_after_save_lr") != address + 8:
                raise ValueError("Unverified emitter entry LR")
        else:
            if (hook.get("constants_offset"), hook.get("dirty_offset")) not in ((1920, 0), (6016, 8)):
                raise ValueError("Unverified constant/dirty layout")
        if not isinstance(hook.get("min_count", 0), int) or not 0 <= hook.get("min_count", 0) <= 256:
            raise ValueError("Invalid minimum copy count")
        if not re.fullmatch(r"[0-9a-f]{64}", hook.get("source_sha256", "")):
            raise ValueError("Missing audited source SHA256")
    hashes = function_hashes(generated, wanted)
    for hook in hooks:
        address = hook["address"]
        if hashes.get(address) != hook["source_sha256"]:
            raise ValueError(f"Native graphics identity mismatch at {address:08X}; re-audit before binding")
    lines = ['// Generated guarded hooks; no title function bodies are embedded.',
             '#include <rex/ppc/context.h>', '#include <bit>', '#include "hook_support.h"']
    for header in sorted({PROFILES[hook["profile"]][0] for hook in hooks}):
        lines.append(f'#include "{header}"')
    lines += ['using namespace xn::gfx::adapters;', 'namespace {']
    for hook in hooks:
        lines.append(f'NativeGraphicsHookStats stats_{hook["address"]:08X};')
    lines.append('}')
    for hook in hooks:
        address = f'{hook["address"]:08X}'
        _, helper, spill = PROFILES[hook["profile"]]
        emitter = hook["profile"] == "grouped_float4_emitter"
        layout = (f'{{0x{hook["entry_after_save_lr"]:08X}, 48, 52}}' if emitter else
                  f'{{{hook["constants_offset"]}, {hook["dirty_offset"]}, 256, {str(spill).lower()}}}')
        count = 'uint32_t(std::popcount(ctx.r4.u64)) * 4' if emitter else 'ctx.r6.u32'
        lines += [f'REX_EXTERN(__imp__sub_{address});', f'REX_EXTERN(sub_{address}) {{',
                  f'  const uint32_t count = {count};',
                  f'  const bool attempted = NativeGraphicsEnabled() && count >= {hook.get("min_count", 0)};',
                  f'  const bool consumed = attempted && {helper}(ctx, base, UINT64_C(0x100000000), {layout});',
                  f'  RecordNativeGraphicsHook(stats_{address}, attempted, consumed, count);',
                  f'  if (!consumed) __imp__sub_{address}(ctx, base);', '}']
    lines.append('void xn::gfx::adapters::ReportNativeGraphicsHooks() {')
    for hook in hooks:
        address = f'{hook["address"]:08X}'
        lines.append(f'  ReportNativeGraphicsHook(0x{address}, stats_{address});')
    lines.append('}')
    write_changed(output / "native_graphics_hooks.cpp", "\n".join(lines) + "\n")
    summary = dict(version=1, default_enabled=False, hooks=[{
        key: hook[key] for key in ("address", "profile", "source_sha256")
    } for hook in hooks])
    write_changed(output / "native_graphics_hooks.json", json.dumps(summary, indent=2) + "\n")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, required=True, help="One original generated module directory")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        summary = emit(args.generated, args.manifest, args.output)
    except (ValueError, OSError, tomllib.TOMLDecodeError) as error:
        parser.exit(1, f"Native graphics hook generation failed: {error}\n")
    print(f"Audited {len(summary['hooks'])} native graphics hooks; runtime default disabled")


if __name__ == "__main__":
    main()

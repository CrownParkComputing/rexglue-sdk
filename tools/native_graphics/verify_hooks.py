#!/usr/bin/env python3
"""Compare audited native graphics helpers with local generated originals.

Only aggregate verification results belong in source control. Original title
bodies are extracted to the caller's ignored output directory for this test.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tomllib

from emit_hooks import emit


def local_output(output, generated):
    if output.is_relative_to(generated) or generated.is_relative_to(output):
        raise ValueError("Keep oracle output separate from original generated sources")
    ancestor = output
    while not ancestor.exists():
        ancestor = ancestor.parent
    result = subprocess.run(["git", "-C", str(ancestor), "rev-parse", "--show-toplevel"],
                            capture_output=True, text=True)
    if result.returncode == 0:
        ignored = subprocess.run(["git", "-C", result.stdout.strip(), "check-ignore", "-q", "--",
                                  str(output / "guest_oracle.cpp")], capture_output=True)
        if ignored.returncode:
            raise ValueError("Oracle output inside a Git worktree must be ignored; use out/ or /tmp")


def original_bodies(generated, wanted):
    result = {}
    pattern = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)\s*\{", re.M)
    for path in sorted(generated.glob("*_recomp.*.cpp")):
        source = path.read_text()
        starts = list(pattern.finditer(source))
        for index, match in enumerate(starts):
            address = int(match[1], 16)
            if address not in wanted:
                continue
            end = starts[index + 1].start() if index + 1 < len(starts) else len(source)
            body = source[match.start():end].strip()
            if address in result:
                raise ValueError(f"Duplicate original function {address:08X}")
            result[address] = body
    if result.keys() != wanted:
        raise ValueError("Missing audited original function")
    return result


def prepare_oracle(hooks, bodies, output):
    profiles = {"forza4_float4": "ForzaFloat4", "mcla_float4": "MclaFloat4",
                "grouped_float4_emitter": "GroupedEmitter"}
    metadata = ["// Local verification metadata; no original function bodies.",
                "#pragma once", "#include <rex/ppc/context.h>",
                "enum class OracleProfile { ForzaFloat4, MclaFloat4, GroupedEmitter };",
                "struct OracleHook { uint32_t address; OracleProfile profile;",
                "  uint32_t constants_offset, dirty_offset, entry_after_save_lr;",
                "  PPCFunc* original; };", ""]
    for hook in hooks:
        metadata.append(f'void Oracle_{hook["address"]:08X}(PPCContext&, uint8_t*);')
    metadata.append("inline const OracleHook kOracleHooks[] = {")
    for hook in hooks:
        address = hook["address"]
        metadata.append(f'  {{0x{address:08X}, OracleProfile::{profiles[hook["profile"]]}, '
                        f'{hook.get("constants_offset", 0)}, {hook.get("dirty_offset", 0)}, '
                        f'0x{hook.get("entry_after_save_lr", 0):08X}, Oracle_{address:08X}}},')
    metadata.append("};")
    (output / "oracle_metadata.h").write_text("\n".join(metadata) + "\n")

    originals = ["// LOCAL RETAIL-DERIVED ORACLE: do not commit or distribute this file.",
                 "#include <rex/ppc/context.h>", "#include <rex/ppc/intrinsics.h>",
                 "#include <cstdio>", "#include <cstdlib>",
                 '#include "oracle_metadata.h"',
                 "#define REX_FUNC_PROLOGUE() ((void)0)",
                 "#define REX_RAW_ADDR(x) (base + uint32_t(x))"]
    for bits in (8, 16, 32, 64):
        load = f"(*(volatile uint{bits}_t*)(base + uint32_t(x)))"
        value = f"uint{bits}_t(y)"
        if bits != 8:
            load = f"__builtin_bswap{bits}({load})"
            value = f"__builtin_bswap{bits}({value})"
        originals += [f"#define REX_LOAD_U{bits}(x) {load}",
                      f"#define REX_STORE_U{bits}(x,y) (*(volatile uint{bits}_t*)(base + uint32_t(x)) = {value})"]
    calls = set()
    for body in bodies.values():
        if "__imp__" in body or "REX_CALL_INDIRECT_FUNC" in body:
            raise ValueError("Audited oracle unexpectedly contains an imported or indirect call")
        calls.update(re.findall(r"^\s*(sub_[0-9A-Fa-f]{8})\(ctx, base\);", body, re.M))
    for name in sorted(calls):
        originals += [f"void {name}(PPCContext&, uint8_t*) {{",
                      f'  std::fputs("Unexpected original slow path {name} in an accepted oracle case\\n", stderr);',
                      "  std::abort();", "}"]
    for hook in hooks:
        address = hook["address"]
        body = re.sub(r"^DEFINE_REX_FUNC\(sub_[0-9A-Fa-f]{8}\)\s*\{",
                      f"void Oracle_{address:08X}(PPCContext& ctx, uint8_t* base) {{",
                      bodies[address], count=1)
        originals.append(body)
    (output / "guest_oracle.cpp").write_text("\n\n".join(originals) + "\n")


def verify(args):
    generated, manifest = args.generated.resolve(), args.manifest.resolve()
    output, platform = args.output.resolve(), args.platform_root.resolve()
    if sys.platform != "linux":
        raise ValueError("This oracle currently verifies the Linux x86-64 memory mapping contract")
    local_output(output, generated)
    # Use the same identity gate as production hook generation, before copying
    # any original body. A changed executable must be audited again.
    summary = emit(generated, manifest, output / "hooks")
    hooks = tomllib.loads(manifest.read_text())["hook"]
    bodies = original_bodies(generated, {hook["address"] for hook in hooks})
    output.mkdir(parents=True, exist_ok=True)
    prepare_oracle(hooks, bodies, output)
    executable = output / "oracle_probe"
    command = shlex.split(args.cxx) + ["-std=c++23", "-O2", "-march=x86-64-v3"]
    includes = [output, platform, platform / "graphics_adapters",
                platform / "runtime/ppc/include", platform / "thirdparty", *args.include_root]
    command += [f"-I{path.resolve()}" for path in includes]
    command += [str(Path(__file__).with_name("oracle_probe.cpp")),
                str(output / "guest_oracle.cpp"), "-o", str(executable)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    (output / "oracle.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError(f"Oracle failed ({result.returncode}); see {output / 'oracle.log'}\n"
                         + result.stderr[:3000])
    metrics = json.loads(result.stdout)
    report = dict(version=1, passed=True, title=tomllib.loads(manifest.read_text()).get("title"),
                  hooks=summary["hooks"], verification=metrics,
                  limitation="Synthetic helper equivalence; game visuals, hook reach and FPS require runtime tests.")
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(hooks)} audited hooks; {metrics['equivalence_cases']} full-context/memory cases; "
          f"{metrics['rejected_cases']} unsupported calls left unchanged")
    print(f"Aggregate results: {output / 'verification.json'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, required=True, help="Original generated/default module")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Ignored local directory, or outside a worktree")
    parser.add_argument("--platform-root", type=Path, required=True, help="Native platform directory containing graphics_adapters/")
    parser.add_argument("--include-root", type=Path, action="append", default=[], help="Additional SDK/header include root")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    args = parser.parse_args()
    try:
        verify(args)
    except (ValueError, OSError, subprocess.CalledProcessError, tomllib.TOMLDecodeError) as error:
        parser.exit(1, f"Native graphics verification failed: {error}\n")


if __name__ == "__main__":
    main()

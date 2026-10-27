#!/usr/bin/env python3
"""Adapt build copies of Forza's generated code to the standalone runtime."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


DB16CYC_DELAY_PAIR = re.compile(
    r"(?P<comment>^[ \t]*// db16cyc[ \t]*)(?P<eol>\r?\n)"
    r"(?P<indent>[ \t]*)(?:std::this_thread::yield\(\)|"
    r"rex::runtime::guest_delay_hint\(\));",
    re.MULTILINE,
)
DB16CYC_COMMENT = re.compile(r"^[ \t]*// db16cyc[ \t]*(?:\r?\n|$)", re.MULTILINE)
GUEST_DELAY_INCLUDE = re.compile(
    r'^[ \t]*#include[ \t]*[<"][^>"\r\n]*guest_delay_hint\.hpp[>"][ \t]*(?:\r?\n|$)',
    re.MULTILINE,
)
CANONICAL_GUEST_DELAY_INCLUDE = "#include <guest/guest_delay_hint.hpp>\n"


def rewrite_db16cyc_delay_hints(text):
    """Rewrite only exact generated db16cyc/yield pairs in a build copy."""
    matches = list(DB16CYC_DELAY_PAIR.finditer(text))
    comments = list(DB16CYC_COMMENT.finditer(text))
    if len(matches) != len(comments):
        raise RuntimeError("Unrecognized generated db16cyc delay-hint site")
    if not matches:
        return text, 0

    rewritten = DB16CYC_DELAY_PAIR.sub(
        lambda match: (match.group("comment") + match.group("eol") +
                       match.group("indent") + "rex::runtime::guest_delay_hint();"),
        text,
    )
    return rewritten, len(matches)


def canonicalize_guest_delay_include(preamble):
    """Canonicalize and deduplicate the optional helper include in a unit."""
    matches = list(GUEST_DELAY_INCLUDE.finditer(preamble))
    if not matches:
        return CANONICAL_GUEST_DELAY_INCLUDE + preamble
    first = matches[0]
    eol = "\r\n" if first.group().endswith("\r\n") else "\n"
    canonical = f"#include <guest/guest_delay_hint.hpp>{eol}"
    result = []
    cursor = 0
    for index, match in enumerate(matches):
        result.append(preamble[cursor:match.start()])
        if index == 0:
            result.append(canonical)
        cursor = match.end()
    result.append(preamble[cursor:])
    return "".join(result)


def remove_guest_delay_include(preamble):
    return GUEST_DELAY_INCLUDE.sub("", preamble)


def rewrite_mcla_tiling_hooks(body):
    """Attach audited MCLA hooks only to the title's native build copy."""
    count = 0
    if "DEFINE_REX_FUNC(sub_8217A470)" in body:
        anchor = "loc_8217A700:\n\t// addi r11,r7,159\n"
        if body.count(anchor) != 1:
            raise RuntimeError("Missing MCLA main tiling anchor at 8217A700")
        injection = ("loc_8217A700:\n"
            "\t{\n"
            "\t\textern bool MclaNativePrepareTile(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint8_t*);\n"
            "\t\tif (MclaNativePrepareTile(r17.u32,r28.u32,r25.u32,r18.u32,ctx.r7.u32,ctx.r8.u32,base)) {\n"
            "\t\t\tctx.r7.u64 = r28.u64; ctx.r8.u64 = r25.u64;\n"
            "\t\t}\n\t}\n\t// addi r11,r7,159\n")
        body = body.replace(anchor, injection, 1)
        count += 1
    if "DEFINE_REX_FUNC(sub_82410D70)" in body:
        anchor = ("\t// cmplwi cr6,r11,2048\n"
                  "\tctx.cr6.compare<uint32_t>(ctx.r11.u32, 2048, ctx.xer);\n"
                  "\t// ble cr6,0x82410e48\n")
        if body.count(anchor) != 1 or body.count("loc_82410E48:") != 1:
            raise RuntimeError("Missing MCLA surface limit anchor at 82410E34")
        body = body.replace(anchor,
            "\t{\n\t\textern bool MclaNativeAllowSurface(uint32_t,uint32_t,uint32_t);\n"
            "\t\tif (MclaNativeAllowSurface(ctx.r3.u32,r30.u32,ctx.r11.u32)) goto loc_82410E48;\n\t}\n" + anchor, 1)
        count += 1
    return body, count


def rewrite_forza_tiling_hooks(body):
    """Change the audited scene descriptor before any surfaces are created."""
    function = "DEFINE_REX_FUNC(sub_828837A0)"
    if function not in body:
        return body, 0
    anchor = function + " {\n\tREX_FUNC_PROLOGUE();\n"
    if body.count(anchor) != 1:
        raise RuntimeError("Missing Forza scene tiling entry at 828837A0")
    body = body.replace(anchor, anchor +
        "\t{\n\t\textern void Forza4NativePrepareTiling(uint32_t,uint8_t*);\n"
        "\t\tForza4NativePrepareTiling(ctx.r4.u32,base);\n\t}\n", 1)
    return body, 1


def prepare(source, output, functions_per_unit=128, title="forza4", modules=("default", "xmediafacade", "speechfacade")):
    fingerprint = hashlib.sha256(Path(__file__).read_bytes())
    fingerprint.update(json.dumps([title, modules, functions_per_unit]).encode())
    for path in sorted(source.glob("*/*")):
        if path.suffix in (".h", ".cpp"):
            fingerprint.update(str(path.relative_to(source)).encode())
            with path.open("rb") as stream:
                fingerprint.update(hashlib.file_digest(stream, "sha256").digest())
    digest = fingerprint.hexdigest()
    manifest = output / "manifest.json"
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        outputs = previous.get("outputs", [])
        if previous.get("input_sha256") == digest and outputs and all((output / path).is_file() for path in outputs):
            return previous["modules"]
    sources = []
    result_modules = []
    for module in modules:
        original = source / module
        target = output / module
        init = (original / f"{title}_init.cpp").read_text()
        names = re.findall(r"\{\s*0x[0-9A-Fa-f]+,\s*(\w+)\s*\}", init)
        replacements = {}
        if module != "default":
            for name in names:
                # Address-named functions are already unique across images.
                if not name.startswith(("sub_", "__imp__")):
                    replacements[name] = module + "_" + name
                    replacements["__imp__" + name] = "__imp__" + module + "_" + name
            replacements["PPCImageConfig"] = module + "_PPCImageConfig"
            replacements["PPCFuncMappings"] = module + "_PPCFuncMappings"
        pattern = re.compile(r"\b(" + "|".join(map(re.escape, sorted(replacements, key=len, reverse=True))) + r")\b") if replacements else None

        def adapt(text):
            if pattern:
                text = pattern.sub(lambda match: replacements[match.group()], text)
            return text

        for header in original.glob("*.h"):
            text = header.read_text()
            if header.name == f"{title}_pch.h":
                text = text.replace('#include <rex/ppc.h>', '#include <rex/ppc.h>\n#include "kernel/fibers.h"')
                text = text.replace('  static thread_local std::unordered_map<u32, jmp_buf> map;\n  return map;', '  return xn::CurrentJumpBuffers();')
            write(target / header.name, adapt(text))
        # Native module registration is implemented by the native loader.
        init = re.sub(r"^void RegisterRecompiledModules.*\n", "", init, flags=re.M)
        init = re.sub(r"^\s*\.register_modules = RegisterRecompiledModules,\n", "", init, flags=re.M)
        write(target / f"{title}_init.cpp", adapt(init))
        module_sources = [str(target / f"{title}_init.cpp")]
        unit_count = 0
        function_count = 0
        tiling_hooks = 0
        for original_unit in sorted(original.glob(f"{title}_recomp.*.cpp")):
            text = original_unit.read_text()
            starts = list(re.finditer(r"^DEFINE_REX_FUNC\(\w+\)\s*\{", text, re.M))
            if not starts:
                raise RuntimeError(f"No functions in {original_unit}")
            preamble = text[:starts[0].start()]
            for offset in range(0, len(starts), functions_per_unit):
                end = starts[offset + functions_per_unit].start() if offset + functions_per_unit < len(starts) else len(text)
                body = text[starts[offset].start():end]
                # Optional cache-repair tool: observe existing garage cars,
                # then defer their ordinary thumbnail job to an input poll.
                if title == "forza4" and module == "default" and "DEFINE_REX_FUNC(sub_823F3550)" in body:
                    anchor = "\tctx.lr = 0x823F35C8;\n\tsub_82820770(ctx, base);\n"
                    if body.count(anchor) != 1:
                        raise RuntimeError("Missing Forza garage-car observation anchor")
                    body = body.replace(anchor, anchor +
                        "\t{\n"
                        "\t\textern void Forza4NativeNoteThumbnailCar(uint32_t, uint8_t*);\n"
                        "\t\tForza4NativeNoteThumbnailCar(REX_LOAD_U32(r29.u32), base);\n"
                        "\t}\n", 1)
                if title == "forza4" and module == "default" and "DEFINE_REX_FUNC(sub_8240F980)" in body:
                    anchor = "DEFINE_REX_FUNC(sub_8240F980) {\n\tREX_FUNC_PROLOGUE();\n"
                    if body.count(anchor) != 1:
                        raise RuntimeError("Missing Forza thumbnail lookup anchor")
                    body = body.replace(anchor, anchor +
                        "\t{\n"
                        "\t\textern void Forza4NativeNoteThumbnailCar(uint32_t, uint8_t*);\n"
                        "\t\tForza4NativeNoteThumbnailCar(ctx.r4.u32, base);\n"
                        "\t}\n", 1)
                # A synchronized texture lock exposes GPU-written pixels to
                # CPU consumers such as D3DX image export / car postcards.
                # Keep the original title fence and lock bookkeeping intact.
                if title == "forza4" and module == "default" and "DEFINE_REX_FUNC(sub_82386C48)" in body:
                    anchor = "loc_82386FAC:\n"
                    if body.count(anchor) != 1:
                        raise RuntimeError("Missing Forza texture-lock synchronization anchor")
                    body = body.replace(anchor, anchor +
                        "\t{\n"
                        "\t\textern void Forza4NativeSynchronizeTextureLock(uint32_t, uint32_t, uint32_t, uint8_t*);\n"
                        "\t\tForza4NativeSynchronizeTextureLock(r26.u32, r20.u32, r25.u32, base);\n"
                        "\t}\n", 1)
                # Native compatibility control, after TrackSettings reads its
                # detail-blur peak speed. Keep retail sources/assets intact.
                if title == "forza4" and module == "default" and "DEFINE_REX_FUNC(sub_82914230)" in body:
                    anchor = "loc_82915914:\n"
                    if body.count(anchor) != 1:
                        raise RuntimeError("Missing Forza TrackSettings compatibility anchor")
                    body = body.replace(anchor, anchor +
                        "\t{\n"
                        "\t\textern void Forza4NativeConfigureTrackSettings(uint32_t, uint8_t*);\n"
                        "\t\tForza4NativeConfigureTrackSettings(r30.u32, base);\n"
                        "\t}\n", 1)
                # Carry the known codegen label repair only in this build copy.
                if title == "forza4" and "loc_825CAF50:" not in body and "goto loc_825CAF50" in body:
                    anchor = "\tsub_825C81B8(ctx, base);\n\t// beq cr6,0x825caf50"
                    if anchor not in body:
                        raise RuntimeError("Missing Forza codegen repair anchor")
                    body = body.replace(anchor, "\tsub_825C81B8(ctx, base);\nloc_825CAF50:\n\t// beq cr6,0x825caf50", 1)
                if title == "midnightclubla" and module == "default":
                    body, injected = rewrite_mcla_tiling_hooks(body)
                    tiling_hooks += injected
                if title == "forza4" and module == "default":
                    body, injected = rewrite_forza_tiling_hooks(body)
                    tiling_hooks += injected
                body, delay_hint_rewrites = rewrite_db16cyc_delay_hints(body)
                path = target / f"native_unit_{unit_count:04}.cpp"
                if delay_hint_rewrites:
                    unit_text = canonicalize_guest_delay_include(preamble) + body
                else:
                    unit_text = remove_guest_delay_include(preamble) + body
                write(path, adapt(unit_text))
                module_sources.append(str(path))
                unit_count += 1
            function_count += len(starts)
        sources += module_sources
        if title == "midnightclubla" and module == "default" and tiling_hooks != 2:
            raise RuntimeError("Expected both MCLA native tiling hook functions")
        if title == "forza4" and module == "default" and "sub_828837A0" in names and tiling_hooks != 1:
            raise RuntimeError("Expected the Forza native scene tiling hook")
        result_modules.append({"module": module, "functions": function_count, "units": unit_count})
        write(target / "sources.cmake", "set(" + title.upper() + "_" + module.upper() + "_SOURCES\n" + "".join(f'  "{path}"\n' for path in module_sources) + ")\n")
    outputs = [str(path.relative_to(output)) for path in output.glob("*/*") if path.is_file()]
    write(manifest, json.dumps({"modules": result_modules, "input_sha256": digest, "outputs": outputs}, indent=2) + "\n")
    return result_modules


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--title", default="forza4")
    parser.add_argument("--modules", nargs="+", default=["default", "xmediafacade", "speechfacade"])
    args = parser.parse_args()
    print(json.dumps(prepare(args.source.resolve(), args.output.resolve(), title=args.title, modules=args.modules)))

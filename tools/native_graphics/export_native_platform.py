#!/usr/bin/env python3
"""Export or verify the audited source-only standalone native platform.

Exports only Git-indexed paths in an explicit allowlist. Original game code,
assets, profiles, caches and compiled files are outside that allowlist.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess

FONT = "resources/fonts/LiberationSans-Regular.ttf"
FONT_SHA256 = "baccc64becc3eb7d104b7c84d99f5314a0a1f896e2b3ea6c2f22fc08d2003bee"
TREES = ("native/runtime/", "native/thirdparty/", "native/graphics_adapters/")
EXACT = {
    "native/platform.cmake", "native/prepare_codegen.py", "native/prepare_codegen_probe.py", "native/prepare_imports.py",
    "native/render_tools.cmake", "native/render_replay.cpp", "native/render_trace_probe.cpp",
    "native/draw_batch_profile_probe.cpp",
    "native/write_watch_trace_probe.cpp", "native/dma_index_cache_probe.cpp",
    "native/prepared_index_cache_probe.cpp",
    "native/resident_index_cache_probe.cpp", "native/draw_preparation_dirty_probe.cpp",
    "native/edram_layout_probe.cpp", "native/mcla_tiling_probe.cpp", "native/forza4_tiling_probe.cpp",
    "native/render_replay_semantics_probe.cpp", "native/render_capture_probe.cpp",
    "native/native_options.cpp", "native/native_options.h", "native/native_settings.cpp",
    "native/native_settings.h", "native/native_disc2.cpp", "native/native_disc2.h",
    "native/forza4_setup.cpp", "native/forza4_setup.h",
    "tools/setup_import.py", "tools/prepare_fmv.sh",
    "native/THIRD_PARTY_NOTICES.md", "native/UPSTREAM.json",
    "licenses/runtime-module-fixes.txt", "thirdparty/nuklear/nuklear.h",
    "thirdparty/nuklear/LICENSE.txt", "resources/fonts/LICENSE.txt", FONT,
    *("tools/native_graphics/" + name for name in (
        "emit_hooks.py", "verify_hooks.py", "oracle_probe.cpp", "test_emit_hooks.py", "bench_replay.py",
        "apply_guest_delay_hints.py", "guest_delay_hint_probe.cpp")),
}
BLOCKED = {".xex", ".iso", ".xiso", ".stfs", ".xdc", ".xsh", ".xpso", ".xnrt", ".spv",
           ".exe", ".dll", ".so", ".a", ".o", ".obj", ".pyc", ".zip", ".7z", ".rar",
           ".png", ".jpg", ".jpeg", ".bmp", ".wmv", ".wav", ".sav", ".sqlite", ".log"}
MAGIC = (b"XEX2", b"PIRS", b"LIVE", b"CON ", b"\x7fELF", b"MZ", b"PK\x03\x04", b"cxds", b"XNRTRC01")
SECRETS = re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[0-9A-Z]{16}")


def permitted(name):
    path = PurePosixPath(name)
    return (not path.is_absolute() and ".." not in path.parts and
            (name in EXACT or name.startswith(TREES)))


def audit(name, data):
    if not permitted(name):
        raise ValueError(f"Path is outside the source allowlist: {name}")
    if len(data) > 10 * 1024 * 1024 or SECRETS.search(data):
        raise ValueError(f"Oversized source or possible credential: {name}")
    if name == FONT:
        if hashlib.sha256(data).hexdigest() != FONT_SHA256:
            raise ValueError("The licensed font differs from its audited snapshot")
        return
    if Path(name).suffix.lower() in BLOCKED or data.startswith(MAGIC) or b"\0" in data:
        raise ValueError(f"Unexpected binary/game/build file in source package: {name}")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(f"Non-text file outside the licensed font exception: {name}") from error


def manifest_hash(files):
    return hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def verify(bundle, *, require_current=True):
    manifest = json.loads((bundle / "manifest.json").read_text())
    if manifest.get("format") != "rexglue-native-platform/1":
        raise ValueError("Unknown native platform package format")
    seen = set()
    for entry in manifest["files"]:
        name = entry["path"]
        if name in seen or not permitted(name) or entry["mode"] not in (0o644, 0o755):
            raise ValueError(f"Invalid/duplicate source manifest entry: {name}")
        seen.add(name)
        path = bundle / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing source or symlink in package: {name}")
        data = path.read_bytes()
        audit(name, data)
        if len(data) != entry["bytes"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError(f"Source hash mismatch: {name}")
        if stat.S_IMODE(path.stat().st_mode) != entry["mode"]:
            raise ValueError(f"Source mode mismatch: {name}")
    actual = {p.relative_to(bundle).as_posix() for p in bundle.rglob("*") if p.is_file() or p.is_symlink()}
    if actual != seen | {"manifest.json"}:
        raise ValueError("Bundle has unlisted or missing files; compiled/captured output is not allowed")
    if ((require_current and not EXACT.issubset(seen)) or manifest_hash(manifest["files"]) != manifest["content_sha256"] or
            manifest["total_bytes"] != sum(e["bytes"] for e in manifest["files"]) or
            manifest.get("licensed_binary") != FONT):
        raise ValueError("Incomplete native source package or manifest hash mismatch")
    return manifest


def export(source, output):
    source, output = source.resolve(), output.resolve()
    if output.exists():
        raise ValueError("Use a fresh export directory; existing bundles are never overwritten")
    if not (source / ".git").exists():
        # Worktree .git is a regular file; both forms are supported.
        result = subprocess.run(["git", "-C", str(source), "rev-parse", "--show-toplevel"],
                                capture_output=True, text=True)
        if result.returncode or Path(result.stdout.strip()).resolve() != source:
            raise ValueError("--source-root must identify the source Git worktree")
    if output.is_relative_to(source):
        ignored = subprocess.run(["git", "-C", str(source), "check-ignore", "-q", "--",
                                  str(output / "manifest.json")], capture_output=True)
        if ignored.returncode:
            raise ValueError("Export inside the worktree must be ignored; use out/ or an external directory")
    untracked = subprocess.check_output(
        ["git", "-C", str(source), "ls-files", "--others", "--exclude-standard", "-z"]).split(b"\0")
    unknown = [p.decode() for p in untracked if p and permitted(p.decode())]
    if unknown:
        raise ValueError("Stage/audit new platform source before export: " + ", ".join(unknown[:20]))
    indexed = subprocess.check_output(["git", "-C", str(source), "ls-files", "--stage", "-z"])
    payload, entries = {}, []
    for item in indexed.split(b"\0"):
        if not item:
            continue
        metadata, path_bytes = item.split(b"\t", 1)
        name = path_bytes.decode()
        if not permitted(name):
            continue
        mode, _oid, stage = metadata.decode().split()
        path = source / name
        if stage != "0" or mode not in ("100644", "100755") or path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
            raise ValueError(f"Source must be an ordinary indexed file without conflicts: {name}")
        data = path.read_bytes()
        audit(name, data)
        payload[name] = data
        entries.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                            mode=0o755 if mode == "100755" else 0o644))
    if not EXACT.issubset(payload):
        raise ValueError("Required common source is not indexed: " + ", ".join(sorted(EXACT - payload.keys())))
    manifest = dict(format="rexglue-native-platform/1",
                    source_commit=subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
                    snapshot="Git-indexed paths; audited working-tree source bytes",
                    files=entries, total_bytes=sum(e["bytes"] for e in entries),
                    licensed_binary=FONT, content_sha256=manifest_hash(entries))
    for entry in entries:
        path = output / entry["path"]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload[entry["path"]])
        path.chmod(entry["mode"])
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    verify(output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify-bundle", type=Path)
    args = parser.parse_args()
    try:
        if args.verify_bundle:
            result = verify(args.verify_bundle.resolve())
        elif args.source_root and args.output:
            result = export(args.source_root, args.output)
        else:
            parser.error("Use --source-root/--output or --verify-bundle")
        print(f"PASS: {len(result['files'])} audited source files, {result['total_bytes']} bytes; "
              f"only licensed font is binary; {result['content_sha256']}")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Native platform export rejected: {error}\n")


if __name__ == "__main__":
    main()

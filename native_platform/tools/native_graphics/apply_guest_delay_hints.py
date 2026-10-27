#!/usr/bin/env python3
"""Plan or apply the opt-in native db16cyc delay-hint experiment.

Only exact generated pairs of ``// db16cyc`` followed by
``std::this_thread::yield();`` are changed. Every changed source is backed up
byte-for-byte before writing; a JSON manifest records the original SHA-256.
No generated source is modified unless ``--apply`` is supplied.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile


OLD_CALL = re.compile(r"^(?P<indent>[ \t]*)std::this_thread::yield\(\);(?P<tail>\r?)$")
HELPER_CALL = re.compile(
    r"^(?P<indent>[ \t]*)rex::runtime::guest_delay_hint\(\);(?P<tail>\r?)$"
)
DB16_COMMENT = re.compile(r"^[ \t]*// db16cyc[ \t]*(?:\r?)$")
YIELD_TEXT = "std::this_thread::yield()"
HELPER_NAME = "rex::runtime::guest_delay_hint"
HELPER_TEXT = HELPER_NAME + "()"
MANIFEST_NAME = "guest_delay_hint_backups.json"


class Refusal(RuntimeError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def relative_key(project: Path, source: Path) -> str:
    return source.relative_to(project).as_posix()


def transform(source: bytes, include_path: str, file_path: Path) -> tuple[bytes, int, int]:
    try:
        text = source.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise Refusal(f"not UTF-8 generated source: {file_path}") from exc
    lines = text.splitlines(keepends=True)
    result: list[str] = []
    old_count = 0
    new_count = 0

    index = 0
    while index < len(lines):
        line = lines[index]
        bare = line.rstrip("\n")
        if DB16_COMMENT.fullmatch(bare):
            if index + 1 >= len(lines):
                raise Refusal(f"db16cyc comment at EOF: {file_path}:{index + 1}")
            next_line = lines[index + 1].rstrip("\n")
            old_match = OLD_CALL.fullmatch(next_line)
            helper_match = HELPER_CALL.fullmatch(next_line)
            if old_match:
                indent = old_match.group("indent")
                tail = old_match.group("tail")
                result.append(line)
                result.append(f"{indent}{HELPER_TEXT};{tail}\n" if line.endswith("\n") else f"{indent}{HELPER_TEXT};{tail}")
                old_count += 1
                index += 2
                continue
            if helper_match:
                result.append(line)
                result.append(lines[index + 1])
                new_count += 1
                index += 2
                continue
            raise Refusal(
                f"unrecognized instruction after db16cyc comment: {file_path}:{index + 2}"
            )

        if YIELD_TEXT in line and (index == 0 or not DB16_COMMENT.fullmatch(lines[index - 1].rstrip("\n"))):
            raise Refusal(f"unpaired scheduler yield in generated source: {file_path}:{index + 1}")
        if HELPER_TEXT in line and (index == 0 or not DB16_COMMENT.fullmatch(lines[index - 1].rstrip("\n"))):
            raise Refusal(f"unpaired guest delay helper in generated source: {file_path}:{index + 1}")
        result.append(line)
        index += 1

    transformed = "".join(result).encode("utf-8")
    if old_count + new_count == 0:
        return source, 0, 0

    include_line = f'#include "{include_path}"'
    include_matches = [i for i, line in enumerate(result) if line.rstrip("\r\n") == include_line]
    if len(include_matches) > 1:
        raise Refusal(f"duplicate delay helper include in {file_path}")
    if include_matches:
        if old_count:
            raise Refusal(f"old db16cyc yield remains with helper include in {file_path}")
    else:
        if new_count and not old_count:
            raise Refusal(f"converted db16cyc site lacks expected helper include in {file_path}")
        # Place next to the generated module's primary funcs header. This keeps
        # the include relative to the source file and independent of build flags.
        primary = next(
            (i for i, line in enumerate(result) if line.startswith('#include "') and "_funcs" in line),
            None,
        )
        if primary is None:
            raise Refusal(f"cannot find generated funcs include in {file_path}")
        result.insert(primary + 1, include_line + "\n")
        transformed = "".join(result).encode("utf-8")
    return transformed, old_count, new_count


def load_manifest(backup_root: Path) -> dict:
    path = backup_root / MANIFEST_NAME
    if not path.exists():
        return {"version": 1, "files": {}}
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise Refusal(f"cannot read backup manifest {path}: {exc}") from exc
    if manifest.get("version") != 1 or not isinstance(manifest.get("files"), dict):
        raise Refusal(f"unrecognized backup manifest format: {path}")
    return manifest


def atomic_write(path: Path, data: bytes, mode: int | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        if mode is not None:
            os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path, help="project root containing generated/ and native/")
    parser.add_argument("--backup", required=True, type=Path, help="directory for exact pre-edit backups and SHA-256 manifest")
    parser.add_argument("--platform-root", type=Path, help="native platform root containing runtime/guest/guest_delay_hint.hpp (default: PROJECT/native)")
    parser.add_argument("--apply", action="store_true", help="write converted generated sources after backing them up")
    args = parser.parse_args()

    project = args.project.resolve()
    backup_root = args.backup.resolve()
    generated = project / "generated"
    try:
        platform_root = (args.platform_root or (project / "native")).resolve(strict=True)
    except OSError as exc:
        parser.error(f"cannot resolve platform root: {exc}")
    if not platform_root.is_dir():
        parser.error(f"platform root is not a directory: {platform_root}")
    try:
        helper = (platform_root / "runtime/guest/guest_delay_hint.hpp").resolve(strict=True)
    except OSError as exc:
        parser.error(f"cannot resolve delay helper under platform root {platform_root}: {exc}")
    if not generated.is_dir():
        parser.error(f"project has no generated/ directory: {project}")
    if not helper.is_file():
        parser.error(f"delay helper is not a regular file: {helper}")

    manifest = load_manifest(backup_root)
    sources = sorted(generated.rglob("*.cpp"))
    plans: list[tuple[Path, bytes, bytes, int, int]] = []
    for source_path in sources:
        before = source_path.read_bytes()
        include_path = os.path.relpath(helper, source_path.parent).replace(os.sep, "/")
        key = relative_key(project, source_path)
        prior = manifest["files"].get(key)
        if prior:
            if prior.get("include_path") != include_path:
                raise Refusal(f"platform include path differs from recorded conversion for {key}")
        after, old_count, converted_count = transform(before, include_path, source_path)
        if prior:
            backup_path = backup_root / prior.get("backup", "")
            if not backup_path.is_file():
                raise Refusal(f"recorded backup is missing for {key}: {backup_path}")
            original = backup_path.read_bytes()
            if sha256(original) != prior.get("sha256"):
                raise Refusal(f"recorded backup hash mismatch for {key}")
            expected, _, _ = transform(original, include_path, source_path)
            if before not in (original, expected):
                raise Refusal(f"source differs from recorded original and conversion: {key}")
            if before == expected or after == before:
                continue
        elif after == before:
            continue
        plans.append((source_path, before, after, old_count, converted_count))

    old_total = sum(item[3] for item in plans)
    converted_total = sum(item[4] for item in plans)
    action = "APPLY" if args.apply else "PLAN"
    print(f"{action}: {len(plans)} source file(s); {old_total} yield site(s) to convert; {converted_total} already converted among changed files")
    for source_path, _before, _after, old_count, converted_count in plans:
        print(f"  {relative_key(project, source_path)}: {old_count} to convert, {converted_count} already converted")
    if not args.apply or not plans:
        print("No files written." if not args.apply else "Already applied; no files written.")
        return 0

    backup_root.mkdir(parents=True, exist_ok=True)
    changed_manifest = json.loads(json.dumps(manifest))
    for source_path, before, _after, _old_count, _converted_count in plans:
        key = relative_key(project, source_path)
        record = changed_manifest["files"].get(key)
        if not record:
            backup_rel = Path("sources") / key
            backup_path = backup_root / backup_rel
            if backup_path.exists():
                exact = backup_path.read_bytes()
                if exact != before:
                    raise Refusal(f"unrecorded backup differs from source: {backup_path}")
            else:
                backup_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source_path, backup_path)
            exact = backup_path.read_bytes()
            if exact != before:
                raise Refusal(f"backup verification failed for {key}")
            include_path = os.path.relpath(
                helper, source_path.parent
            ).replace(os.sep, "/")
            record = {
                "backup": backup_rel.as_posix(),
                "sha256": sha256(before),
                "include_path": include_path,
            }
            changed_manifest["files"][key] = record
        else:
            backup_path = backup_root / record["backup"]
            original = backup_path.read_bytes()
            if sha256(original) != record.get("sha256"):
                raise Refusal(f"recorded backup hash mismatch for {key}")
            include_path = os.path.relpath(
                helper, source_path.parent
            ).replace(os.sep, "/")
            if record.get("include_path") != include_path:
                raise Refusal(f"platform include path differs from recorded conversion for {key}")
            expected, _, _ = transform(original, include_path, source_path)
            if before not in (original, expected):
                raise Refusal(f"source differs from recorded original and conversion: {key}")

    manifest_data = (json.dumps(changed_manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    atomic_write(backup_root / MANIFEST_NAME, manifest_data)
    for source_path, _before, after, _old_count, _converted_count in plans:
        atomic_write(source_path, after, source_path.stat().st_mode & 0o777)
    print(f"Backups and SHA-256 manifest written under {backup_root}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refusal as exc:
        print(f"refusing: {exc}", file=sys.stderr)
        raise SystemExit(2)

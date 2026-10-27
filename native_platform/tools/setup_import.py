#!/usr/bin/env python3
"""Import a user-supplied FM4 folder, ISO, or archive into a private game root.

Output is one absolute game-root path. No network access is used.
"""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile

EXPECTED = "3618e85af83e972ddcc1b0d0dd808e78e29ab4ab3890a15b31c210a6a3e93e09"
ARCHIVES = {".rar", ".zip", ".7z", ".iso", ".xiso"}
PACKAGES = {f"4d530910{i:08x}" for i in range(1, 5)}


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def valid_game(root):
    xex = root / "default.xex"
    if not xex.is_file() or not (root / "Media").is_dir():
        return False
    h = hashlib.sha256()
    with xex.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest() == EXPECTED


def find_game(root):
    if valid_game(root):
        return root
    for xex in root.rglob("default.xex"):
        if len(xex.relative_to(root).parts) <= 8 and valid_game(xex.parent):
            return xex.parent
    raise ValueError("No matching Forza Motorsport 4 Disc 1 was found (default.xex SHA-256 or Media/ is wrong).")


def safe_name(name):
    name = name.replace("\\", "/")
    path = PurePosixPath(name)
    if (not name or name.startswith("/") or name.startswith("//") or
            any(part in ("", ".", "..") for part in name.split("/")) or
            any(":" in part or any(ord(c) < 32 for c in part) for part in path.parts) or
            any(part.startswith("-") for part in path.parts)):
        raise ValueError(f"Unsafe archive path: {name!r}")
    return path


def list_7z(archive):
    out = run("7z", "l", "-slt", str(archive), stdout=subprocess.PIPE, text=True).stdout
    if "----------\n" not in out:
        raise ValueError("Archive listing is unavailable")
    records = out.split("----------\n", 1)[1].split("\n\n")
    names = set()
    for record in records:
        fields = dict(line.split(" = ", 1) for line in record.splitlines() if " = " in line)
        if not fields or "Path" not in fields:
            continue
        name = fields["Path"]
        safe_name(name)
        key = name.replace("\\", "/")
        if key in names:
            raise ValueError(f"Duplicate archive path: {name}")
        names.add(key)
        attributes=fields.get("Attributes", "")
        mode=fields.get("Mode", "")
        # 7z may prefix POSIX modes with DOS flags, e.g. "A_ lrwxrwxrwx".
        special_mode=bool(re.search(r"(?:^|\s)[bclps][rwxstST-]{9}(?:\s|$)",
                                    attributes+" "+mode))
        if (special_mode or
                " L" in fields.get("Attributes", "") or
                "Symbolic Link" in record or "Hard Link" in record):
            raise ValueError(f"Archive link is not allowed: {name}")
    if not names:
        raise ValueError("Archive is empty")


def reject_links(root):
    for parent, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            if (Path(parent) / name).is_symlink():
                raise ValueError(f"Extracted link is not allowed: {name}")


def extract(source, dest, rexiso):
    dest.mkdir(parents=True)
    if source.suffix.lower() in {".iso", ".xiso"}:
        if not rexiso:
            raise ValueError("rexiso is required to extract an Xbox 360 ISO")
        listing=run(rexiso,"list",str(source),stdout=subprocess.PIPE,text=True).stdout
        for line in listing.splitlines():
            # rexiso lists files as a right-aligned size, two spaces, then name.
            pieces=line.strip().split("  ",1)
            if len(pieces)==2 and pieces[0].isdigit():
                safe_name(pieces[1])
        command=[rexiso,"extract",str(source),str(dest)]
        if shutil.which("bwrap"):
            command=["bwrap","--ro-bind","/","/","--bind",str(dest),str(dest),*command]
        run(*command)
    else:
        list_7z(source)
        command=["7z", "x", "-y", f"-o{dest}", str(source)]
        if shutil.which("bwrap"):
            command=["bwrap","--ro-bind","/","/","--bind",str(dest),str(dest),*command]
        run(*command,stdout=subprocess.DEVNULL)
    reject_links(dest)


def find_packages(root):
    found = {}
    for file in root.rglob("*"):
        if file.is_file() and file.name in PACKAGES and file.name not in found:
            found[file.name] = file
    return found


def nested_disc_archives(root):
    # The game itself contains many ZIP resources. They are payload, rather
    # than additional original discs to unpack during setup.
    found = [p for p in root.rglob("*")
             if p.is_file() and p.suffix.lower() in ARCHIVES and
             not any(part.casefold() == "media" for part in p.relative_to(root).parts[:-1])]
    if len(found) > 8:
        raise ValueError("Archive contains too many nested disc images")
    return found


def extract_disc2(source, stage, rexiso):
    extract(source, stage / "content_source", rexiso)
    packages = find_packages(stage / "content_source")
    if len(packages) != 4:
        images = [f for f in (stage / "content_source").rglob("*")
                  if f.is_file() and f.suffix.lower() in ARCHIVES]
        if len(images) > 8:
            raise ValueError("Disc 2 archive contains too many nested images")
        for i, image in enumerate(images):
            extract(image, stage / f"disc2-image-{i}", rexiso)
            packages.update(find_packages(stage / f"disc2-image-{i}"))
            if len(packages) == 4:
                break
    if len(packages) != 4:
        raise ValueError("Disc 2 source does not contain all four Forza content packages")
    target = stage / "disc2" / "content/0000000000000000/4d530910/00000002"
    target.mkdir(parents=True)
    for name, file in packages.items():
        shutil.move(str(file), target / name)
    return stage / "disc2"


def populate_lower(source, lower, external=False):
    lower.mkdir(exist_ok=True)
    old_lower = source / "media"
    if old_lower.is_dir():
        for item in old_lower.rglob("*"):
            if not item.is_file():
                continue
            relative = item.relative_to(old_lower)
            target = lower / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                target.symlink_to(item.resolve() if external else os.path.relpath(item, target.parent))
    for item in (source / "Media").rglob("*"):
        if not item.is_file():
            continue
        relative = Path(*(part.lower() for part in item.relative_to(source / "Media").parts))
        target = lower / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            target.symlink_to(item.resolve() if external else os.path.relpath(item, target.parent))
    if not (lower / "zipmanifest.xml").is_file():
        raise ValueError("The primary disc Media/zipmanifest.xml is missing")


def build_overlay(source, dest):
    dest.mkdir(parents=True)
    for item in source.iterdir():
        if item.name == "media":
            continue
        (dest / item.name).symlink_to(item.resolve(), target_is_directory=item.is_dir())
    populate_lower(source, dest / "media", external=True)


def build_materialized(source, dest):
    """Create an independent game tree without modifying the selected folder.

    File data is linked when possible (copy on cross-device imports). Original
    media aliases are rebuilt as relative links inside the new game root.
    """
    source_root = source.resolve(strict=True)
    count = 0

    def entry(item, target, ancestors, depth):
        nonlocal count
        count += 1
        if count > 500000 or depth > 64:
            raise ValueError("The original game tree is too deep or contains too many entries")
        resolved = item.resolve(strict=True)
        if not resolved.is_relative_to(source_root):
            raise ValueError(f"Original game link leaves the selected folder: {item}")
        if resolved.is_dir():
            if resolved in ancestors:
                raise ValueError(f"Original game contains a directory link cycle: {item}")
            target.mkdir()
            next_ancestors = ancestors | {resolved}
            for child in resolved.iterdir():
                entry(child, target / child.name, next_ancestors, depth + 1)
        elif resolved.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(resolved, target)
            except OSError:
                shutil.copy2(resolved, target)
        else:
            raise ValueError(f"Unsupported original game entry: {item}")

    dest.mkdir()
    for item in source.iterdir():
        if item.name == "media":
            continue
        entry(item, dest / item.name, {source_root}, 0)

    old_lower = source / "media"
    new_lower = dest / "media"
    new_lower.mkdir()
    if old_lower.is_dir() and not old_lower.is_symlink():
        # Preserve physical Disc 2 files, but rebuild case aliases from Media/.
        for parent, dirs, files in os.walk(old_lower, followlinks=False):
            dirs[:] = [name for name in dirs if not (Path(parent) / name).is_symlink()]
            for name in files:
                item = Path(parent) / name
                if item.is_symlink():
                    continue
                target = new_lower / item.relative_to(old_lower)
                entry(item, target, {source_root}, len(target.relative_to(dest).parts))
    populate_lower(dest, new_lower)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--source", type=Path)
    p.add_argument("--disc2-source", type=Path)
    p.add_argument("--include-disc2", action="store_true")
    p.add_argument("--materialize", action="store_true")
    p.add_argument("--destination", required=True, type=Path)
    p.add_argument("--rexiso", default="")
    args = p.parse_args()
    if bool(args.source) == bool(args.disc2_source):
        raise ValueError("Choose exactly one Disc 1 or Disc 2 source")
    source = (args.source or args.disc2_source).resolve(strict=True)
    rexiso = args.rexiso or os.environ.get("REXISO_BIN") or shutil.which("rexiso")
    if not rexiso:
        for parent in Path(__file__).resolve().parents:
            candidates = (parent / "bin/rexiso", parent / "rexiso")
            rexiso = next((str(p) for p in candidates if p.is_file() and os.access(p, os.X_OK)), None)
            if rexiso:
                break
    if not rexiso:
        local = Path(__file__).resolve().parents[3] / "_library/rexglue-vmx/out/install/bin/rexiso"
        if local.is_file():
            rexiso = str(local)
    destination = args.destination.absolute()
    if destination.exists():
        if args.disc2_source and len(find_packages(destination)) == 4:
            print(destination)
            return
        if args.source and valid_game(destination) and (destination / "media/zipmanifest.xml").is_file():
            recorded = destination / ".import-source"
            if (recorded.is_file() and recorded.read_text() != str(source) and
                    destination.resolve() != source):
                raise ValueError("A different Disc 1 is already imported; choose its original source or use another profile")
            print(destination)
            return
        raise ValueError(f"Existing game import needs attention: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".forza4-import-", dir=destination.parent) as temp:
        stage = Path(temp)
        if args.disc2_source:
            if source.is_dir():
                packages=find_packages(source)
                if len(packages)!=4:
                    raise ValueError("Disc 2 folder does not contain all four Forza content packages")
                target=stage/"disc2"/"content/0000000000000000/4d530910/00000002"
                target.mkdir(parents=True)
                for name,file in packages.items():
                    (target/name).symlink_to(file.resolve())
            elif source.is_file() and source.suffix.lower() in ARCHIVES:
                extract_disc2(source,stage,rexiso)
            else:
                raise ValueError("Choose an extracted Disc 2 folder, Xbox ISO, RAR, ZIP, or 7z file")
            os.replace(stage/"disc2",destination)
            print(destination)
            return
        directory_source = source.is_dir()
        if directory_source:
            game = find_game(source)
        elif source.is_file() and source.suffix.lower() in ARCHIVES:
            extract(source, stage / "extracted", rexiso)
            nested = nested_disc_archives(stage / "extracted")
            game_image = None
            try:
                game = find_game(stage / "extracted")
            except ValueError:
                for i, item in enumerate(nested):
                    extract(item, stage / f"nested-{i}", rexiso)
                    try:
                        game = find_game(stage / f"nested-{i}")
                        game_image=item
                        break
                    except ValueError:
                        pass
                else:
                    raise ValueError("No matching Forza Motorsport 4 Disc 1 was found in the archive")
        else:
            raise ValueError("Choose an extracted folder, Xbox ISO, RAR, ZIP, or 7z file")
        if directory_source:
            if args.materialize:
                build_materialized(game, stage / "game")
            else:
                build_overlay(game, stage / "game")
        else:
            shutil.move(str(game), str(stage / "game"))
            populate_lower(stage / "game", stage / "game/media")
            if args.include_disc2:
                packages=find_packages(stage/"game")
                packages.update(find_packages(stage/"extracted"))
                if len(packages)!=4:
                    extras=list(nested)
                    extras.extend(nested_disc_archives(stage / "game"))
                    for i,item in enumerate(extras):
                        if item==game_image or not item.exists():
                            continue
                        extract(item,stage/f"extra-{i}",rexiso)
                        packages.update(find_packages(stage/f"extra-{i}"))
                        if len(packages)==4:
                            break
                if len(packages)==4:
                    target=stage/"game"/"Disc2/content/0000000000000000/4d530910/00000002"
                    target.mkdir(parents=True,exist_ok=True)
                    for name,file in packages.items():
                        if not file.is_relative_to(stage/"game"):
                            shutil.move(str(file),target/name)
                elif nested:
                    raise ValueError("The combined archive has no complete Disc 2 content; select Disc 2 separately or skip it")
        (stage/"game"/".import-source").write_text(str(source))
        os.replace(stage / "game", destination)
    print(destination)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"Forza import: {exc}", file=sys.stderr)
        sys.exit(1)

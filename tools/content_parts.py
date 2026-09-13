#!/usr/bin/env python3
"""Pack private disc content and restore it without putting binaries in Git."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tempfile

BLOCK = 1024 * 1024
ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pack(source, manifest_path, parts_dir, part_size, repository, release):
    parts_dir.mkdir(parents=True, exist_ok=True)
    manifest = {'version': 1, 'repository': repository,
                'release': release, 'files': [], 'parts': []}
    output = None
    try:
        for path in sorted(source.rglob('*')):
            if path == source / '.recomp-content-verified':
                continue
            if path.is_symlink():
                raise ValueError(f'Symlink in content: {path}')
            if not path.is_file():
                continue
            file_hash = hashlib.sha256()
            size = 0
            with path.open('rb') as stream:
                while data := stream.read(BLOCK):
                    file_hash.update(data)
                    size += len(data)
                    while data:
                        if output is None:
                            name = f'content.part-{len(manifest["parts"]):04d}'
                            output = (parts_dir / name).open('wb')
                            part_hash = hashlib.sha256()
                            part_bytes = 0
                        count = min(len(data), part_size - part_bytes)
                        output.write(data[:count])
                        part_hash.update(data[:count])
                        part_bytes += count
                        data = data[count:]
                        if part_bytes == part_size:
                            output.close()
                            output = None
                            manifest['parts'].append({'name': name, 'size': part_bytes,
                                                      'sha256': part_hash.hexdigest()})
            manifest['files'].append({'name': path.relative_to(source).as_posix(),
                                      'size': size, 'sha256': file_hash.hexdigest()})
        if output is not None:
            output.close()
            output = None
            manifest['parts'].append({'name': name, 'size': part_bytes,
                                      'sha256': part_hash.hexdigest()})
    finally:
        if output is not None:
            output.close()
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Packed {len(manifest["files"])} files into {len(manifest["parts"])} parts.')


def safe_name(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or '..' in path.parts or '\\' in name:
        raise ValueError(f'Unsafe content path: {name!r}')
    return path


def restore(manifest_path, parts_dir, destination, download):
    manifest = json.loads(manifest_path.read_text())
    if manifest['version'] != 1:
        raise ValueError('Unsupported content manifest version')
    for entry in manifest['files'] + manifest['parts']:
        safe_name(entry['name'])
    if sum(x['size'] for x in manifest['files']) != sum(x['size'] for x in manifest['parts']):
        raise ValueError('Content manifest sizes do not match')
    marker = destination / '.recomp-content-verified'
    fingerprint = digest(manifest_path)
    complete = all((destination / x['name']).is_file() and
                   (destination / x['name']).stat().st_size == x['size']
                   for x in manifest['files'])
    if complete:
        if marker.exists() and marker.read_text().strip() == fingerprint:
            return
        if all(digest(destination / x['name']) == x['sha256'] for x in manifest['files']):
            marker.write_text(fingerprint + '\n')
            print('Existing game content verified.')
            return
    parts_dir.mkdir(parents=True, exist_ok=True)
    for part in manifest['parts']:
        path = parts_dir / part['name']
        if not path.exists() and download:
            subprocess.run(['gh', 'release', 'download', manifest['release'],
                            '--repo', manifest['repository'], '--pattern', part['name'],
                            '--dir', str(parts_dir)], check=True)
        if not path.exists() or path.stat().st_size != part['size'] or digest(path) != part['sha256']:
            raise ValueError(f'Missing or corrupt content part: {path}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Validate every output before installing any of it; never extract archive paths.
    with tempfile.TemporaryDirectory(prefix='.content-restore-', dir=destination.parent) as tmp:
        staged = Path(tmp)
        part_index = 0
        source = None
        try:
            for entry in manifest['files']:
                target = staged / entry['name']
                target.parent.mkdir(parents=True, exist_ok=True)
                remaining = entry['size']
                file_hash = hashlib.sha256()
                with target.open('wb') as output:
                    while remaining:
                        if source is None:
                            source = (parts_dir / manifest['parts'][part_index]['name']).open('rb')
                            part_index += 1
                        data = source.read(min(BLOCK, remaining))
                        if not data:
                            source.close()
                            source = None
                            continue
                        output.write(data)
                        file_hash.update(data)
                        remaining -= len(data)
                if file_hash.hexdigest() != entry['sha256']:
                    raise ValueError(f'Reassembled file checksum mismatch: {entry["name"]}')
        finally:
            if source is not None:
                source.close()
        for entry in manifest['files']:
            target = destination / entry['name']
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.is_symlink() or any(p.is_symlink() for p in target.parents):
                raise ValueError(f'Refusing to write through symlink: {target}')
            os.replace(staged / entry['name'], target)
    marker.write_text(fingerprint + '\n')
    print(f'Restored and verified {len(manifest["files"])} game files.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['pack', 'restore'])
    parser.add_argument('--manifest', type=Path, default=ROOT / 'content/manifest.json')
    parser.add_argument('--parts', type=Path, default=ROOT / 'content/parts')
    parser.add_argument('--assets', type=Path, default=ROOT / 'assets')
    parser.add_argument('--part-size', type=int, default=256 * BLOCK)
    parser.add_argument('--repository', default='CrownParkComputing/PROJECT-recomp',
                        help='GitHub repository holding the release parts (pack only)')
    parser.add_argument('--release', default='assets-ztm-v1',
                        help='Release tag holding the parts (pack only)')
    parser.add_argument('--download', action='store_true')
    args = parser.parse_args()
    if args.action == 'pack':
        if args.part_size <= 0:
            parser.error('--part-size must be positive')
        pack(args.assets, args.manifest, args.parts, args.part_size,
             args.repository, args.release)
    else:
        restore(args.manifest, args.parts, args.assets, args.download)


if __name__ == '__main__':
    main()

"""Content must survive part boundaries; corrupt input must never be installed."""
import json
from pathlib import Path
import tempfile
import unittest

from content_parts import pack, restore


class ContentPartsTest(unittest.TestCase):
    def test_roundtrip_and_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source'
            source.mkdir()
            (source / 'nested').mkdir()
            expected = {'empty': b'', 'nested/data': bytes(range(256)) * 7,
                        'last': b'last file across a boundary'}
            for name, data in expected.items():
                (source / name).write_bytes(data)
            manifest = root / 'manifest.json'
            parts = root / 'parts'
            destination = root / 'restored'
            pack(source, manifest, parts, 37, 'CrownParkComputing/rru-recomp', 'assets-ztm-v1')
            restore(manifest, parts, destination, False)
            for name, data in expected.items():
                self.assertEqual((destination / name).read_bytes(), data)
            restore(manifest, parts, destination, False)
            first = next(parts.iterdir())
            first.write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                restore(manifest, parts, root / 'rejected', False)
            self.assertFalse((root / 'rejected').exists())
            contents = json.loads(manifest.read_text())
            contents['files'][0]['name'] = '../escape'
            manifest.write_text(json.dumps(contents))
            with self.assertRaises(ValueError):
                restore(manifest, parts, root / 'unsafe', False)
            self.assertFalse((root / 'escape').exists())


if __name__ == '__main__':
    unittest.main()

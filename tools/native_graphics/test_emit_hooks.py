#!/usr/bin/env python3
"""Fail-closed metadata checks using synthetic code, never retail game code."""
import hashlib
from pathlib import Path
import tempfile
import unittest

from emit_hooks import emit


class EmissionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.generated = self.root / "generated"
        self.generated.mkdir()
        self.source = "DEFINE_REX_FUNC(sub_82001000) {\n  return;\n}\n"
        self.path = self.generated / "synthetic_recomp.0.cpp"
        self.path.write_text(self.source)
        digest = hashlib.sha256(self.source.strip().encode()).hexdigest()
        self.manifest = self.root / "native.toml"
        self.text = ('version = 1\n[[hook]]\naddress = 0x82001000\n'
                     'profile = "forza4_float4"\nconstants_offset = 1920\n'
                     f'dirty_offset = 0\nsource_sha256 = "{digest}"\n')
        self.manifest.write_text(self.text)
        self.output = self.root / "output"

    def test_output_only_contains_guard_and_forward(self):
        report = emit(self.generated, self.manifest, self.output)
        output = (self.output / "native_graphics_hooks.cpp").read_text()
        self.assertFalse(report["default_enabled"])
        self.assertIn("REX_EXTERN(sub_82001000)", output)
        self.assertIn("if (!consumed) __imp__sub_82001000(ctx, base);", output)
        self.assertNotIn(self.source, output)

    def test_changed_function_rejected_before_output(self):
        self.path.write_text(self.source.replace("return;", "ctx.r3.u64 = 1;"))
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            emit(self.generated, self.manifest, self.output)
        self.assertFalse(self.output.exists())

    def test_unknown_profile_and_layout_rejected(self):
        for before, after in (("forza4_float4", "unknown"), ("1920", "1936")):
            self.manifest.write_text(self.text.replace(before, after))
            with self.assertRaises(ValueError):
                emit(self.generated, self.manifest, self.output)

    def test_duplicate_function_rejected(self):
        (self.generated / "synthetic_recomp.1.cpp").write_text(self.source)
        with self.assertRaisesRegex(ValueError, "Duplicate function"):
            emit(self.generated, self.manifest, self.output)

    def test_copy_threshold_and_emitter_lr_rejected(self):
        self.manifest.write_text(self.text + "min_count = 257\n")
        with self.assertRaisesRegex(ValueError, "minimum copy count"):
            emit(self.generated, self.manifest, self.output)
        self.manifest.write_text(self.text.replace("forza4_float4", "grouped_float4_emitter") +
                                 "entry_after_save_lr = 0x82001004\n")
        with self.assertRaisesRegex(ValueError, "entry LR"):
            emit(self.generated, self.manifest, self.output)


if __name__ == "__main__":
    unittest.main()

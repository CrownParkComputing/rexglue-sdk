#!/usr/bin/env python3
"""Asset-free regression checks for generated-code build-copy rewriting."""

import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
import tempfile
import unittest


def load_adapter(path):
    spec = importlib.util.spec_from_file_location("prepare_codegen", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PrepareCodegenProbe(unittest.TestCase):
    def test_forza_tiling_hook_is_guarded_and_title_specific(self):
        text = "DEFINE_REX_FUNC(sub_828837A0) {\n\tREX_FUNC_PROLOGUE();\n}\n"
        result, count = self.adapter.rewrite_forza_tiling_hooks(text)
        self.assertEqual(count, 1)
        self.assertIn("Forza4NativePrepareTiling(ctx.r4.u32,base);", result)
        self.assertEqual(self.adapter.rewrite_forza_tiling_hooks("DEFINE_REX_FUNC(sub_1) {}"),
                         ("DEFINE_REX_FUNC(sub_1) {}", 0))
        with self.assertRaisesRegex(RuntimeError, "Missing Forza scene tiling entry"):
            self.adapter.rewrite_forza_tiling_hooks(text.replace("REX_FUNC_PROLOGUE();", "other();"))

    def test_loading_adapter_does_not_modify_source_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "adapter.py"
            source.write_text("value = 7\n")
            self.assertEqual(load_adapter(source).value, 7)
            self.assertFalse((source.parent / "__pycache__").exists())

    def test_mcla_tiling_anchors_and_guarded_fallback(self):
        text = ("DEFINE_REX_FUNC(sub_8217A470) {\n"
                "loc_8217A700:\n\t// addi r11,r7,159\n}\n"
                "DEFINE_REX_FUNC(sub_82410D70) {\n"
                "\t// cmplwi cr6,r11,2048\n"
                "\tctx.cr6.compare<uint32_t>(ctx.r11.u32, 2048, ctx.xer);\n"
                "\t// ble cr6,0x82410e48\nloc_82410E48:\n}\n")
        result, count = self.adapter.rewrite_mcla_tiling_hooks(text)
        self.assertEqual(count, 2)
        self.assertIn("if (MclaNativePrepareTile", result)
        self.assertIn("ctx.r7.u64 = r28.u64; ctx.r8.u64 = r25.u64;", result)
        self.assertIn("if (MclaNativeAllowSurface", result)
        self.assertIn("ctx.cr6.compare<uint32_t>(ctx.r11.u32, 2048, ctx.xer);", result)
        self.assertEqual(self.adapter.rewrite_mcla_tiling_hooks("DEFINE_REX_FUNC(sub_1) {}"),
                         ("DEFINE_REX_FUNC(sub_1) {}", 0))
        with self.assertRaisesRegex(RuntimeError, "Missing MCLA main tiling"):
            self.adapter.rewrite_mcla_tiling_hooks(text.replace("r11,r7,159", "r11,r7,158"))
        with self.assertRaisesRegex(RuntimeError, "Missing MCLA surface limit"):
            self.adapter.rewrite_mcla_tiling_hooks(text.replace("loc_82410E48:", "loc_other:"))

    def test_exact_pairs_and_mixed_newlines(self):
        source = (
            "// db16cyc \r\n\tstd::this_thread::yield();\r\n"
            "// db16cyc\n  std::this_thread::yield();\n"
            "// unrelated\n\tstd::this_thread::yield();\n"
        )
        rewritten, count = self.adapter.rewrite_db16cyc_delay_hints(source)
        self.assertEqual(count, 2)
        self.assertEqual(rewritten.count("rex::runtime::guest_delay_hint();"), 2)
        self.assertIn("// db16cyc \r\n\trex::runtime::guest_delay_hint();\r\n", rewritten)
        self.assertIn("// unrelated\n\tstd::this_thread::yield();\n", rewritten)

    def test_malformed_marker_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Unrecognized generated db16cyc"):
            self.adapter.rewrite_db16cyc_delay_hints("// db16cyc\n\tnop;\n")

    def test_prepare_injects_include_only_for_changed_split_unit(self):
        with tempfile.TemporaryDirectory(prefix="prepare-codegen-probe-") as temp:
            root = Path(temp)
            source = root / "source"
            module = source / "default"
            module.mkdir(parents=True)
            init = module / "forza4_init.cpp"
            unit = module / "forza4_recomp.0.cpp"
            init.write_text("// synthetic registration\n")
            original = (
                '#include "fixture.h"\n'
                "DEFINE_REX_FUNC(sub_1) {\n"
                "\t// db16cyc\n\tstd::this_thread::yield();\n"
                "\t// db16cyc\n\tstd::this_thread::yield();\n"
                "}\n"
                "DEFINE_REX_FUNC(sub_2) {\n"
                "\tstd::this_thread::yield();\n"
                "}\n"
            )
            unit.write_text(original)
            before = {path: path.read_bytes() for path in (init, unit)}

            self.adapter.prepare(source, root / "build-copy", functions_per_unit=1,
                                 title="forza4", modules=("default",))

            first = (root / "build-copy/default/native_unit_0000.cpp").read_text()
            second = (root / "build-copy/default/native_unit_0001.cpp").read_text()
            self.assertEqual(first.count("#include <guest/guest_delay_hint.hpp>"), 1)
            self.assertEqual(first.count("rex::runtime::guest_delay_hint();"), 2)
            self.assertNotIn("std::this_thread::yield();", first)
            self.assertNotIn("guest_delay_hint.hpp", second)
            self.assertIn("\tstd::this_thread::yield();", second)
            self.assertEqual({path: path.read_bytes() for path in before}, before)

    def test_prepare_accepts_premapped_input_and_canonicalizes_include(self):
        with tempfile.TemporaryDirectory(prefix="prepare-codegen-premapped-") as temp:
            root = Path(temp)
            module = root / "source/default"
            module.mkdir(parents=True)
            init = module / "forza4_init.cpp"
            unit = module / "forza4_recomp.0.cpp"
            init.write_text("// synthetic registration\n")
            original = (
                '#include "fixture.h"\n'
                '#include "../../native/runtime/guest/guest_delay_hint.hpp"\n'
                '#include <extra/guest_delay_hint.hpp>\n'
                "DEFINE_REX_FUNC(sub_1) {\n"
                "\t// db16cyc\n\trex::runtime::guest_delay_hint();\n"
                "}\n"
                "DEFINE_REX_FUNC(sub_2) {\n\tstd::this_thread::yield();\n}\n"
            )
            unit.write_text(original)
            before = unit.read_bytes()

            self.adapter.prepare(root / "source", root / "build-copy", functions_per_unit=1,
                                 title="forza4", modules=("default",))

            first = (root / "build-copy/default/native_unit_0000.cpp").read_text()
            second = (root / "build-copy/default/native_unit_0001.cpp").read_text()
            self.assertEqual(first.count("#include <guest/guest_delay_hint.hpp>"), 1)
            self.assertNotIn("../../native/runtime/guest/guest_delay_hint.hpp", first)
            self.assertNotIn("extra/guest_delay_hint.hpp", first)
            self.assertIn("rex::runtime::guest_delay_hint();", first)
            self.assertNotIn("guest_delay_hint.hpp", second)
            self.assertIn("\tstd::this_thread::yield();", second)
            self.assertEqual(unit.read_bytes(), before)

    @classmethod
    def setUpClass(cls):
        if len(sys.argv) < 2:
            raise RuntimeError("pass the path to native/prepare_codegen.py")
        cls.adapter = load_adapter(Path(sys.argv[1]))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])

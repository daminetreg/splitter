import contextlib
import io
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build


class BuildTests(unittest.TestCase):
    def test_semantic_comparison_escapes_code_and_preserves_roles(self):
        block = build.Block("semantic", "plain-gc | split-lto-relink-gc",
                            "```diff\n= max_of<T>\n```\n---\n```diff\n+ source.cpp\n```", 9)
        block.path = Path("comparison.md")
        result = build.render_block(block, {})
        self.assertIn('class="same"> max_of&lt;T&gt;', result)
        self.assertIn('class="diffline"> source.cpp', result)
        self.assertIn('class="semantic"', result)

    def setUp(self):
        self.assets_tmp = tempfile.TemporaryDirectory()
        self.old_here = build.HERE
        build.HERE = Path(self.assets_tmp.name)
        (build.HERE / "template.html").write_text(
            "<style>/*__STYLE__*/</style><script>const DECK=/*__DECK__*/;\n/*__RUNTIME__*/</script>")
        (build.HERE / "theme.css").write_text("body{color:green}")
        (build.HERE / "runtime.js").write_text("void DECK;")

    def tearDown(self):
        build.HERE = self.old_here
        self.assets_tmp.cleanup()

    def source(self, root, order="one.md\ntwo.md\n"):
        root, slides = Path(root), Path(root) / "custom" / "slides"
        slides.mkdir(parents=True)
        (slides.parent / "snippets.md").write_text(
            "::: snippet x | X | description\n```text\ncode\n```\n:::\n")
        (slides / "manifest.txt").write_text(order)
        for name in ("one.md", "two.md"):
            (slides / name).write_text(
                "---\nchapter: Test\nnotes: note\n---\n## " + name + "\n\nHello **world**.\n")
        return slides

    def replace_slide(self, source, body):
        (source / "one.md").write_text("---\nchapter: Test\nnotes: note\n---\n" + body)

    def test_build_is_deterministic_and_escapes(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source(directory)
            self.replace_slide(source, "## One\n\n<script>unsafe</script>\n")
            with self.assertRaises(build.SourceError):
                build.build(source, Path(directory) / "out.html")
            self.replace_slide(source, "## One\n\n5 < 7 & safe\n")
            output = Path(directory) / "out.html"
            build.build(source, output)
            first = output.read_bytes()
            build.build(source, output)
            self.assertEqual(first, output.read_bytes())
            self.assertIn(b"5 &lt; 7 &amp; safe", first)

    def test_schema_failures_are_actionable(self):
        cases = [
            ("::: bars\n- row | NaN | 1\n:::\n", "finite and non-negative"),
            ("::: bars\n- row | -1 | 1\n:::\n", "finite and non-negative"),
            ('::: sizes\n{"scale":10,"metrics":[]}\n:::\n', "non-empty metrics"),
            ('::: sizes\n{"scale":10,"metrics":[{"name":"x","plain":1,"split":1,"note":"a"},{"name":"x","plain":1,"split":1,"note":"b"}]}\n:::\n', "duplicate"),
            ('::: sizes\n{"scale":10,"metrics":[{"name":"x","plain":-1,"split":1,"note":"a"}]}\n:::\n', "finite and non-negative"),
            ('::: sizes\n{"scale":10,"metrics":[{"name":"x","plain":NaN,"split":1,"note":"a"}]}\n:::\n', "finite and non-negative"),
            ('::: sizes\n{"scale":10,"metrics":[{"name":"x","plain":1,"split":1,"note":"a"}],"controls":[{}]}\n:::\n', "list of metric names"),
            ('::: section-map\n[{"name":"x","equal":"yes","detail":"d"}]\n:::\n', "boolean"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            source = self.source(directory)
            for body, message in cases:
                self.replace_slide(source, body)
                with self.assertRaisesRegex(build.SourceError, message) as error:
                    build.render(source)
                self.assertIn("one.md:", str(error.exception))

    def test_section_map_is_generalizable_but_requires_unique_strict_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source(directory)
            self.replace_slide(source, '::: section-map\n[{"name":"custom","equal":true,"detail":"editable"}]\n:::\n')
            build.render(source)  # No presentation-specific fixed entry count.
            self.replace_slide(source, '::: section-map\n[{"name":"x","equal":true,"detail":"a"},{"name":"x","equal":false,"detail":"b"}]\n:::\n')
            with self.assertRaisesRegex(build.SourceError, "duplicate section-map"):
                build.render(source)

    def test_missing_reference_and_unknown_directive(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source(directory)
            self.replace_slide(source, "::: popup no-such\nX\n:::\n")
            with self.assertRaisesRegex(build.SourceError, "unknown snippet"):
                build.render(source)
            self.replace_slide(source, "::: trace\n{}\n:::\n")
            with self.assertRaisesRegex(build.SourceError, "unknown directive"):
                build.render(source)

    def test_check_renders_but_does_not_write(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = self.source(directory), Path(directory) / "not-written.html"
            self.assertEqual(0, build.main(["--source", str(source), "--output", str(output), "--check"]))
            self.assertFalse(output.exists())

    def test_manifest_reorder_and_edit_rebuild(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = self.source(directory), Path(directory) / "out.html"
            build.build(source, output)
            first = output.read_text()
            (source / "manifest.txt").write_text("two.md\none.md\n")
            build.build(source, output)
            second = output.read_text()
            self.assertLess(first.index("one.md"), first.index("two.md"))
            self.assertLess(second.index("two.md"), second.index("one.md"))
            self.replace_slide(source, "## Edited heading\n")
            build.build(source, output)
            self.assertIn("Edited heading", output.read_text())

    def test_failed_build_preserves_previous_output_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = self.source(directory), Path(directory) / "out.html"
            build.build(source, output)
            before = output.read_bytes()
            self.replace_slide(source, "::: bars\n- broken | nope | 1\n:::\n")
            with self.assertRaises(build.SourceError):
                build.build(source, output)
            self.assertEqual(before, output.read_bytes())

    def test_watch_inputs_custom_source_assets_and_output_exclusion(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source(directory)
            output = source.parent / "generated.html"
            paths = build.watch_inputs(source, output)
            self.assertIn((source / "manifest.txt").resolve(), paths)
            self.assertIn((source / "one.md").resolve(), paths)
            self.assertIn((source.parent / "snippets.md").resolve(), paths)
            self.assertIn((build.HERE / "template.html").resolve(), paths)
            self.assertIn((build.HERE / "theme.css").resolve(), paths)
            self.assertIn((build.HERE / "runtime.js").resolve(), paths)
            self.assertNotIn(output.resolve(), paths)

    def test_cli_source_error_has_no_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source(directory)
            self.replace_slide(source, "::: bars\n- broken | nope | 1\n:::\n")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(2, build.main(["--source", str(source), "--check"]))
            self.assertIn("one.md:", stderr.getvalue())
            self.assertNotIn("Traceback", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
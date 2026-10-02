"""Unit tests for config loading (no X server, no touchpad needed)."""

from __future__ import annotations

import contextlib
import dataclasses
import io
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from wintab import config as config_module  # noqa: E402


class ConfigLoadTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self._tmp.name) / "config.toml"
        self.addCleanup(self._tmp.cleanup)

    def write(self, text: str) -> config_module.Config:
        self.path.write_text(text, encoding="utf-8")
        return config_module.load(self.path)

    def test_missing_file_gives_defaults(self):
        cfg = config_module.load(self.path)
        self.assertEqual(cfg.order, "recent")
        self.assertEqual(cfg.step_fraction, config_module.Config().step_fraction)

    def test_order_round_trips(self):
        self.assertEqual(self.write('order = "opened"\n').order, "opened")

    def test_bad_order_falls_back_without_crashing(self):
        self.assertEqual(self.write('order = "sideways"\n').order, "recent")

    def test_shipped_example_has_no_unknown_or_bad_keys(self):
        # config.example.toml is the documentation; a key renamed in the code
        # must be renamed there too, or users copy a file that warns.
        example = pathlib.Path(__file__).resolve().parent.parent / "config.example.toml"
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            cfg = config_module.load(example)
        self.assertEqual(buffer.getvalue().strip(), "")
        self.assertIn(cfg.order, config_module.ORDER_MODES)

    def test_every_default_key_appears_in_the_example(self):
        example = (
            pathlib.Path(__file__).resolve().parent.parent / "config.example.toml"
        ).read_text(encoding="utf-8")
        for field in dataclasses.fields(config_module.Config):
            self.assertIn(field.name, example, f"{field.name} is undocumented")

    def test_unknown_keys_are_ignored(self):
        cfg = self.write("nonsense = 5\norder = \"recent\"\n")
        self.assertFalse(hasattr(cfg, "nonsense"))
        self.assertEqual(cfg.order, "recent")

    def test_highlight_color_is_clamped(self):
        cfg = self.write("highlight_rgb = [2.0, -1.0]\n")
        self.assertEqual(cfg.highlight_color, (1.0, 0.0, 0.0))


if __name__ == "__main__":
    unittest.main()

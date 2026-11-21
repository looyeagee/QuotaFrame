from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from quotaframe_bridge.config import (
    config_path,
    load_config,
    read_deepseek_settings,
    read_log_level,
    read_minimax_settings,
)

WINDOWS = "win32"
MACOS = "darwin"


class ConfigTests(unittest.TestCase):
    """Both platform layouts are exercised from either host.

    The platform is an explicit argument rather than the host's own, so a
    Windows developer still covers the macOS branch and the other way round.
    """

    def _environ(self, root: str, platform: str) -> dict[str, str]:
        return {"APPDATA": root} if platform == WINDOWS else {"HOME": root}

    def _directory(self, root: str, platform: str) -> Path:
        base = Path(root)
        if platform == MACOS:
            base = base / "Library" / "Application Support"
        return base / "quotaframe"

    def _write(self, root: str, platform: str, body: str) -> None:
        directory = self._directory(root, platform)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "config.toml").write_text(body, encoding="utf-8")

    def test_missing_file_yields_empty_mapping(self) -> None:
        for platform in (WINDOWS, MACOS):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as root:
                self.assertEqual(
                    load_config(self._environ(root, platform), platform=platform),
                    {},
                )

    def test_malformed_file_yields_empty_mapping(self) -> None:
        for platform in (WINDOWS, MACOS):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as root:
                self._write(root, platform, "this is not = valid = toml")
                self.assertEqual(
                    load_config(self._environ(root, platform), platform=platform),
                    {},
                )

    def test_absent_root_variable_yields_empty_mapping(self) -> None:
        for platform in (WINDOWS, MACOS):
            with self.subTest(platform=platform):
                self.assertIsNone(config_path({}, platform=platform))
                self.assertEqual(load_config({}, platform=platform), {})

    def test_macos_config_lives_in_application_support(self) -> None:
        path = config_path({"HOME": "/Users/panel"}, platform=MACOS)

        self.assertEqual(
            path,
            Path("/Users/panel/Library/Application Support/quotaframe/config.toml"),
        )

    def test_reads_log_level(self) -> None:
        for platform in (WINDOWS, MACOS):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as root:
                self._write(root, platform, 'log_level = "DEBUG"\n')
                self.assertEqual(
                    read_log_level(self._environ(root, platform), platform=platform),
                    "DEBUG",
                )

    def test_log_level_defaults_to_info_when_absent(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self._write(root, WINDOWS, 'language = "zh"\n')
            self.assertEqual(
                read_log_level(self._environ(root, WINDOWS), platform=WINDOWS),
                "INFO",
            )

    def test_invalid_log_level_falls_back_to_info(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self._write(root, WINDOWS, 'log_level = "LOUD"\n')
            self.assertEqual(
                read_log_level(self._environ(root, WINDOWS), platform=WINDOWS),
                "INFO",
            )

    def test_deepseek_settings_come_from_the_file_or_the_environment(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self._write(
                root,
                MACOS,
                '[deepseek]\napi_key = "sk-file"\ncurrency = "usd"\n',
            )
            environ = self._environ(root, MACOS)

            from_file = read_deepseek_settings(environ, platform=MACOS)
            assert from_file is not None
            self.assertEqual(from_file.api_key, "sk-file")
            self.assertEqual(from_file.currency, "USD")

            environ = dict(environ, DEEPSEEK_API_KEY="sk-env")
            from_env = read_deepseek_settings(environ, platform=MACOS)
            assert from_env is not None
            self.assertEqual(from_env.api_key, "sk-env")

    def test_deepseek_settings_absent_without_a_key(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self._write(root, MACOS, 'log_level = "INFO"\n')
            self.assertIsNone(
                read_deepseek_settings(self._environ(root, MACOS), platform=MACOS)
            )

    def test_minimax_key_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self._write(root, MACOS, '[minimax]\nkey = "sk-cp-file"\n')
            environ = self._environ(root, MACOS)
            settings = read_minimax_settings(environ, platform=MACOS)
            assert settings is not None
            self.assertEqual(settings.api_key, "sk-cp-file")

            self._write(root, MACOS, '[minimax]\napi_key = "sk-cp-alias"\n')
            settings = read_minimax_settings(environ, platform=MACOS)
            assert settings is not None
            self.assertEqual(settings.api_key, "sk-cp-alias")

            environ = dict(environ, MINIMAX_API_KEY="sk-cp-env")
            settings = read_minimax_settings(environ, platform=MACOS)
            assert settings is not None
            self.assertEqual(settings.api_key, "sk-cp-env")

    def test_minimax_settings_absent_without_a_key(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            self._write(root, MACOS, 'log_level = "INFO"\n')
            self.assertIsNone(
                read_minimax_settings(self._environ(root, MACOS), platform=MACOS)
            )


if __name__ == "__main__":
    unittest.main()

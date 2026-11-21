from __future__ import annotations

import re
import unittest
from pathlib import Path

from quotaframe_bridge.targets import TARGETS

ROOT = Path(__file__).resolve().parents[2]


def parse_defaults(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
            continue
        match = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
        if match:
            values[match.group(1)] = "n"
    return values


class FirmwareConfigurationTests(unittest.TestCase):
    def test_target_memory_and_nimble_roles_are_pinned(self) -> None:
        for target in TARGETS:
            config = parse_defaults(ROOT / target.firmware_project / "sdkconfig.defaults")
            with self.subTest(target=target.id):
                self._assert_pinned(config, target.ota_transfer_bytes)

    def _assert_pinned(self, config: dict[str, str], transfer_bytes: int) -> None:
        expected = {
            "CONFIG_IDF_TARGET": '"esp32s3"',
            "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
            "CONFIG_SPIRAM": "y",
            "CONFIG_SPIRAM_MODE_OCT": "y",
            "CONFIG_BT_ENABLED": "y",
            "CONFIG_BT_NIMBLE_ENABLED": "y",
            "CONFIG_BT_NIMBLE_ROLE_CENTRAL": "n",
            "CONFIG_BT_NIMBLE_ROLE_OBSERVER": "n",
            "CONFIG_BT_NIMBLE_ROLE_BROADCASTER": "y",
            "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL": "y",
            "CONFIG_BT_NIMBLE_MAX_CONNECTIONS": "1",
            "CONFIG_BT_NIMBLE_NVS_PERSIST": "y",
            "CONFIG_BT_NIMBLE_SM_SC_ONLY": "1",
            "CONFIG_BT_NIMBLE_SM_LVL": "3",
            "CONFIG_ESP_DESKTOP_BUDDY_RX_INBOX_DEPTH": "32",
            "CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_MAX_RAW_CHUNK": "4096",
            "CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_MAX_DECODED_CHUNK": "2880",
            "CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_MAX_TRANSFER_BYTES": str(transfer_bytes),
            "CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_FLAT_PATHS_ONLY": "y",
        }
        self.assertEqual(
            {key: config.get(key) for key in expected},
            expected,
        )

    def test_runtime_ble_security_policy_is_explicit(self) -> None:
        source = (
            ROOT
            / "firmware"
            / "components"
            / "usage_ble"
            / "src"
            / "ble_service.cpp"
        ).read_text()

        for statement in (
            "transport_config.security.bonding = true;",
            "transport_config.security.mitm = true;",
            "transport_config.security.secure_connections = true;",
            "ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_DISPLAY_ONLY",
        ):
            with self.subTest(statement=statement):
                self.assertIn(statement, source)


if __name__ == "__main__":
    unittest.main()

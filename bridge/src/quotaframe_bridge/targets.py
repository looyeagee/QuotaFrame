"""Authoritative definitions for maintained hardware targets."""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Mapping


@dataclass(frozen=True, slots=True)
class WebFlashDefinition:
    """User-facing metadata needed to assemble the browser flasher."""

    name: str
    description: str
    chip_family: str
    asset: str


@dataclass(frozen=True, slots=True)
class TargetDefinition:
    """Product-level metadata shared by Bridge and repository tooling."""

    id: str
    label: str
    firmware_project: str
    idf_version: str
    ota_image: str
    full_image: str
    release_stem: str
    image_chip_id: int
    ota_partition_bytes: int
    test_apps: tuple[str, ...] = ()
    web_flash: WebFlashDefinition | None = None

    @property
    def ota_transfer_bytes(self) -> int:
        """Allow a full app partition plus the OTA sink's 512-byte manifest buffer."""
        return self.ota_partition_bytes + 512


TARGETS: tuple[TargetDefinition, ...] = (
    TargetDefinition(
        id="waveshare_rlcd_42",
        label="Waveshare RLCD 4.2",
        firmware_project="firmware/targets/waveshare_rlcd_42",
        idf_version="6.1",
        ota_image="ws_rlcd_42.bin",
        full_image="ws_rlcd_42_full.bin",
        release_stem="waveshare-esp32-s3-rlcd-42",
        image_chip_id=9,
        ota_partition_bytes=4 * 1024 * 1024,
        test_apps=("firmware/targets/waveshare_rlcd_42/test_apps/logic",),
        web_flash=WebFlashDefinition(
            name="Waveshare RLCD 4.2",
            description="400 × 300 黑白反射式 LCD · KEY / BOOT 按键翻页",
            chip_family="ESP32-S3",
            asset="assets/devices/waveshare_rlcd_42.jpg",
        ),
    ),
    TargetDefinition(
        id="zltech_154tft",
        label="ZLTech 1.54 TFT",
        firmware_project="firmware/targets/zltech_154tft",
        idf_version="6.1",
        ota_image="zltech_154tft.bin",
        full_image="zltech_154tft_full.bin",
        release_stem="zltech-esp32-s3-154tft",
        image_chip_id=9,
        ota_partition_bytes=7 * 1024 * 1024,
    ),
)

IDF_IMAGES: Mapping[str, str] = MappingProxyType(
    {
        "6.1": (
            "espressif/idf:v6.1@sha256:"
            "81893c71bb5e570088901f21def8684c25cd2a9020281bd01b843a7655edb18c"
        ),
    }
)

TARGETS_BY_ID: Mapping[str, TargetDefinition] = MappingProxyType(
    {target.id: target for target in TARGETS}
)

if len(TARGETS_BY_ID) != len(TARGETS):
    raise RuntimeError("maintained target IDs must be unique")

for target in TARGETS:
    if target.idf_version not in IDF_IMAGES:
        raise RuntimeError(
            f"maintained target {target.id!r} uses unknown ESP-IDF "
            f"version {target.idf_version!r}"
        )

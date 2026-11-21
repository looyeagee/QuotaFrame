"""Which macOS applications currently hold an audio output stream.

CoreAudio publishes one process object per client that is connected to the
audio system. Reading the process list and each process' output flag answers
"who is producing sound right now" without asking an application for
permission and without the private MediaRemote framework.

The probe is advisory: a platform or API change must degrade to "nobody is
playing" instead of breaking the caller.
"""

from __future__ import annotations

import ctypes
import logging
import sys
from typing import NamedTuple

LOGGER = logging.getLogger(__name__)

# AudioObjectPropertyAddress selectors from AudioHardware.h.
_SELECTOR_PROCESS_OBJECT_LIST = 0x70727323  # 'prs#'
_SELECTOR_BUNDLE_ID = 0x70626964  # 'pbid'
_SELECTOR_IS_RUNNING_OUTPUT = 0x7069726F  # 'piro'

# The process list and the output flag are global properties on the main element.
_SCOPE_GLOBAL = 0x676C6F62  # 'glob'
_ELEMENT_MAIN = 0x6D61696E  # 'main'


class _PropertyAddress(ctypes.Structure):
    _fields_ = [
        ("mSelector", ctypes.c_uint32),
        ("mScope", ctypes.c_uint32),
        ("mElement", ctypes.c_uint32),
    ]


class AudioClient(NamedTuple):
    """One process connected to the audio system."""

    bundle_id: str
    running_output: bool


def _load_toolbox():
    library = ctypes.CDLL("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox")
    library.AudioObjectGetPropertyData.restype = ctypes.c_int32
    library.AudioObjectGetPropertyData.argtypes = [
        ctypes.c_uint32,
        ctypes.POINTER(_PropertyAddress),
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_void_p,
    ]
    library.AudioObjectGetPropertyDataSize.restype = ctypes.c_int32
    library.AudioObjectGetPropertyDataSize.argtypes = [
        ctypes.c_uint32,
        ctypes.POINTER(_PropertyAddress),
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    return library


def _read(library, object_id: int, selector: int):
    """Read one untyped property; returns None when the property is absent."""

    address = _PropertyAddress(selector, _SCOPE_GLOBAL, _ELEMENT_MAIN)
    size = ctypes.c_uint32(0)
    status = library.AudioObjectGetPropertyDataSize(
        object_id, ctypes.byref(address), 0, None, ctypes.byref(size)
    )
    if status != 0 or size.value == 0:
        return None
    buffer = ctypes.create_string_buffer(size.value)
    status = library.AudioObjectGetPropertyData(
        object_id, ctypes.byref(address), 0, None, ctypes.byref(size), buffer
    )
    return buffer if status == 0 else None


def _bundle_id(library, object_id: int) -> str:
    buffer = _read(library, object_id, _SELECTOR_BUNDLE_ID)
    if buffer is None:
        return ""
    reference = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_void_p)).contents.value
    if not reference:
        return ""
    core_foundation = ctypes.CDLL(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
    )
    core_foundation.CFStringGetCStringPtr.restype = ctypes.c_char_p
    core_foundation.CFStringGetCStringPtr.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    text = core_foundation.CFStringGetCStringPtr(reference, 0x08000100)
    return text.decode("utf-8", "replace") if text else ""


def _running_output(library, object_id: int) -> bool:
    buffer = _read(library, object_id, _SELECTOR_IS_RUNNING_OUTPUT)
    if buffer is None:
        return False
    value = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_uint32)).contents.value
    return value != 0


def audio_clients() -> tuple[AudioClient, ...]:
    """List the processes connected to the audio system.

    Returns an empty tuple on platforms without CoreAudio and whenever the
    query fails, so callers can treat "no audio" as a normal answer.
    """

    if sys.platform != "darwin":
        return ()
    try:
        library = _load_toolbox()
        buffer = _read(library, 1, _SELECTOR_PROCESS_OBJECT_LIST)
        if buffer is None:
            return ()
        count = len(buffer) // 4
        identifiers = ctypes.cast(buffer, ctypes.POINTER(ctypes.c_uint32))
        clients = []
        for index in range(count):
            object_id = identifiers[index]
            clients.append(
                AudioClient(
                    _bundle_id(library, object_id),
                    _running_output(library, object_id),
                )
            )
        return tuple(clients)
    except Exception:  # pragma: no cover - depends on the host audio stack
        LOGGER.debug("CoreAudio process query failed", exc_info=True)
        return ()


def playing_bundle_ids() -> frozenset[str]:
    """Bundle identifiers of the applications that currently output audio."""

    return frozenset(
        client.bundle_id for client in audio_clients() if client.running_output
    )


__all__ = ["AudioClient", "audio_clients", "playing_bundle_ids"]

"""Recent Claude Code sessions and their live status, read from this machine.

Claude Code keeps one JSONL transcript per conversation under
`~/.claude/projects/<project>/<session-id>.jsonl` and publishes the state of
each running session as `~/.claude/sessions/<pid>.json`. The snapshot joins
the two, so a transcript that is still being written reads as live rather
than ended. Everything here is a read; nothing in `~/.claude` is modified.
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

# The transcript head carries the session metadata and the opening prompt;
# anything past it is conversation history the page never shows.
HEAD_BYTES = 32768
TITLE_CHARS = 140
MAX_RECENT_SESSIONS = 20


class ClaudeSessionStore:
    """Read one snapshot of Claude Code's sessions from its data directory."""

    def __init__(self, *, claude_dir: Path | None = None) -> None:
        self._claude_dir = claude_dir if claude_dir is not None else Path.home() / ".claude"

    @property
    def sessions_dir(self) -> Path:
        """The directory Claude publishes one state file per running session in."""

        return self._claude_dir / "sessions"

    def live_snapshot(self) -> list[dict[str, object]]:
        """The sessions that are running right now.

        The panels draw this roster and nothing else, so it skips the transcript
        walk: reading the head of every transcript on disk answers a question
        the panel does not ask.
        """

        return self._live_sessions()

    def snapshot(self) -> dict[str, object]:
        live = self._live_sessions()
        by_id = {entry["session_id"]: entry for entry in live if entry["session_id"]}
        return {
            "now": int(time.time()),
            "live": live,
            "recent": self._recent_transcripts(by_id),
        }

    def _live_sessions(self) -> list[dict[str, object]]:
        sessions: list[dict[str, object]] = []
        try:
            files = list((self._claude_dir / "sessions").glob("*.json"))
        except OSError:
            files = []
        for path in files:
            try:
                raw = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError):
                continue
            if not isinstance(raw, dict):
                continue
            pid = raw.get("pid")
            started_at = _epoch_seconds(raw.get("startedAt"))
            sessions.append(
                {
                    "session_id": _text(raw.get("sessionId")),
                    "name": _text(raw.get("name")),
                    "cwd": _text(raw.get("cwd")),
                    "project": _project_name(raw.get("cwd")),
                    "status": _text(raw.get("status")),
                    # What a waiting session waits for; Claude Code states it
                    # only while the session is waiting.
                    "waiting_for": _text(raw.get("waitingFor")),
                    "version": _text(raw.get("version")),
                    "kind": _text(raw.get("kind")),
                    "entrypoint": _text(raw.get("entrypoint")),
                    "started_at": started_at,
                    "updated_at": _epoch_seconds(raw.get("updatedAt")),
                    "alive": isinstance(pid, int) and _pid_alive(pid),
                }
            )
        # The roster leads with the session touched last, because the panel
        # draws a few rows and the one that just moved is the one being
        # watched. `updatedAt` is what Claude Code rewrites as a session
        # changes, so an idle session started later sinks below an older one
        # that is answering now. Ties fall back to the start time and then the
        # id: a roster that reorders without a state change is a redraw the
        # glass cannot show, and it defeats the dedupe in publish_sessions that
        # keeps unchanged snapshots off the link.
        sessions.sort(
            key=lambda entry: (
                entry["updated_at"] or entry["started_at"] or 0,
                entry["started_at"] or 0,
                entry["session_id"] or "",
            ),
            reverse=True,
        )
        return sessions

    def _recent_transcripts(
        self, live_by_id: dict[str, dict[str, object]]
    ) -> list[dict[str, object]]:
        try:
            candidates = list((self._claude_dir / "projects").glob("*/*.jsonl"))
        except OSError:
            candidates = []
        stats: list[tuple[Path, float, int]] = []
        for path in candidates:
            try:
                info = path.stat()
            except OSError:
                continue
            stats.append((path, info.st_mtime, info.st_size))
        # mtime orders the page and picks what is worth reading; only the
        # newest transcripts get their head parsed.
        stats.sort(key=lambda item: item[1], reverse=True)
        recent = []
        for path, mtime, size in stats[:MAX_RECENT_SESSIONS]:
            head = _read_head(path)
            cwd, branch = _context_from_head(head)
            title = _title_from_head(head)
            if not cwd and not title:
                # Nothing identifiable: not a transcript worth listing.
                continue
            entry: dict[str, object] = {
                "session_id": path.stem,
                "title": title,
                "cwd": cwd,
                "project": _project_name(cwd),
                "branch": branch,
                # The write time is the session's last activity, whatever the
                # newest entry inside records.
                "last_at": int(mtime),
                "size_bytes": size,
                "live": None,
            }
            live = live_by_id.get(path.stem)
            if live is not None:
                entry["live"] = {
                    "status": live["status"],
                    "waiting_for": live["waiting_for"],
                    "alive": live["alive"],
                }
            recent.append(entry)
        return recent


def _read_head(path: Path) -> bytes:
    """Read the first window of a transcript, cut to complete lines."""

    try:
        with path.open("rb") as handle:
            data = handle.read(HEAD_BYTES)
    except OSError:
        return b""
    if len(data) == HEAD_BYTES:
        cut = data.rfind(b"\n")
        if cut != -1:
            data = data[:cut]
    return data


def _title_from_head(head: bytes) -> str:
    """Pick a title: Claude's own summary when present, else the first prompt."""

    summary = ""
    prompt = ""
    for line in head.splitlines():
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(entry, dict):
            continue
        kind = entry.get("type")
        if kind == "summary" and not summary:
            summary = _one_line(entry.get("summary"))
        elif kind == "user" and not prompt and not entry.get("isSidechain"):
            # System reminders and command wrappers start with markup; the
            # human prompt is the first entry that does not.
            prompt = _one_line(_user_text(entry.get("message")))
            if prompt.startswith("<"):
                prompt = ""
        if summary and prompt:
            break
    return summary or prompt


def _user_text(message: object) -> str:
    if not isinstance(message, dict):
        return ""
    content = message.get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = [
            block.get("text", "")
            for block in content
            if isinstance(block, dict) and block.get("type") == "text"
        ]
        return " ".join(part for part in parts if part)
    return ""


def _context_from_head(head: bytes) -> tuple[str, str]:
    cwd = ""
    branch = ""
    for line in head.splitlines():
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(entry, dict):
            continue
        cwd = cwd or _text(entry.get("cwd"))
        branch = branch or _text(entry.get("gitBranch"))
        if cwd and branch:
            break
    return cwd, branch


def _pid_alive(pid: int) -> bool:
    """Best-effort liveness probe; a re-used pid can only ever show extra life."""

    if sys.platform == "win32":
        import ctypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        kernel32 = ctypes.windll.kernel32
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid)
        if not handle:
            return False
        kernel32.CloseHandle(handle)
        return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass  # alive, but owned by another account
    return True


def _epoch_seconds(value: object) -> int | None:
    """Normalise an epoch field that may arrive in milliseconds or seconds."""

    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if value <= 0:
        return None
    return int(value / 1000) if value >= 1e12 else int(value)


def _text(value: object) -> str:
    return value if isinstance(value, str) else ""


def _project_name(cwd: object) -> str:
    if isinstance(cwd, str) and cwd:
        return Path(cwd).name or cwd
    return ""


def _one_line(value: object) -> str:
    text = " ".join(value.split()) if isinstance(value, str) else ""
    if len(text) > TITLE_CHARS:
        return text[: TITLE_CHARS - 1].rstrip() + "…"
    return text

"""The Claude sessions reader and the routes that serve it."""

from __future__ import annotations

import asyncio
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from quotaframe_bridge.control.cc_sessions import TITLE_CHARS, ClaudeSessionStore
from quotaframe_bridge.control.page import render_page
from quotaframe_bridge.control.routes import ControlRoutes, Request
from quotaframe_bridge.sources.claude_sessions import ClaudeSessionsSource

TOKEN = "test-token-0123456789"

SESSION_ID = "aaaaaaaa-1111-2222-3333-444444444444"


class StoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.claude = self.root / ".claude"

    def store(self) -> ClaudeSessionStore:
        return ClaudeSessionStore(claude_dir=self.claude)

    def write_transcript(self, session_id: str, lines: list[str]) -> Path:
        project = self.claude / "projects" / "-tmp-demo"
        project.mkdir(parents=True, exist_ok=True)
        path = project / f"{session_id}.jsonl"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def write_live(self, name: str, payload: dict[str, object]) -> Path:
        sessions = self.claude / "sessions"
        sessions.mkdir(parents=True, exist_ok=True)
        path = sessions / name
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def a_prompt_line(self, content: object, **extra: object) -> str:
        return json.dumps(
            {"type": "user", "message": {"role": "user", "content": content},
             "cwd": "/tmp/demo", "gitBranch": "main", **extra}
        )

    def test_a_live_session_joins_its_transcript(self) -> None:
        self.write_transcript(
            SESSION_ID,
            [self.a_prompt_line("修复列表滚动的问题\n第二行")],
        )
        self.write_live(
            "4242.json",
            {
                "pid": os.getpid(),
                "sessionId": SESSION_ID,
                "cwd": "/tmp/demo",
                "name": "Demo session",
                "status": "busy",
                "version": "2.1.0",
                "kind": "interactive",
                "entrypoint": "claude-desktop-3p",
                "startedAt": 1795330905785,
                "updatedAt": 1795330961861,
            },
        )

        snapshot = self.store().snapshot()

        live = snapshot["live"]
        assert isinstance(live, list)
        self.assertEqual(len(live), 1)
        self.assertEqual(live[0]["session_id"], SESSION_ID)
        self.assertTrue(live[0]["alive"])
        self.assertEqual(live[0]["started_at"], 1795330905)
        self.assertEqual(live[0]["project"], "demo")

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        self.assertEqual(recent[0]["title"], "修复列表滚动的问题 第二行")
        self.assertEqual(recent[0]["branch"], "main")
        self.assertEqual(recent[0]["live"], {"status": "busy", "alive": True})
        self.assertTrue(snapshot["now"])

    def test_a_transcript_without_a_live_process_reads_as_ended(self) -> None:
        self.write_transcript(SESSION_ID, [self.a_prompt_line("hello")])

        snapshot = self.store().snapshot()

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        self.assertEqual(recent[0]["live"], None)
        self.assertEqual(snapshot["live"], [])

    def test_a_summary_line_wins_as_the_title(self) -> None:
        self.write_transcript(
            SESSION_ID,
            [
                json.dumps({"type": "summary", "summary": "Fix scrolling"}),
                self.a_prompt_line("原始终提示词"),
            ],
        )

        snapshot = self.store().snapshot()

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        self.assertEqual(recent[0]["title"], "Fix scrolling")

    def test_a_sidechain_prompt_is_not_a_title(self) -> None:
        self.write_transcript(
            SESSION_ID,
            [
                self.a_prompt_line("子代理指令", isSidechain=True),
                self.a_prompt_line("真正的提示词"),
            ],
        )

        snapshot = self.store().snapshot()

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        self.assertEqual(recent[0]["title"], "真正的提示词")

    def test_a_list_content_prompt_renders_as_text(self) -> None:
        self.write_transcript(
            SESSION_ID,
            [self.a_prompt_line([
                {"type": "text", "text": "块一"},
                {"type": "image", "source": {}},
                {"type": "text", "text": "块二"},
            ])],
        )

        snapshot = self.store().snapshot()

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        self.assertEqual(recent[0]["title"], "块一 块二")

    def test_a_long_prompt_is_truncated(self) -> None:
        long_prompt = "字" * (TITLE_CHARS + 40)
        self.write_transcript(SESSION_ID, [self.a_prompt_line(long_prompt)])

        snapshot = self.store().snapshot()

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        title = recent[0]["title"]
        assert isinstance(title, str)
        self.assertLessEqual(len(title), TITLE_CHARS)
        self.assertTrue(title.endswith("…"))

    def test_a_marked_up_prompt_is_skipped_in_favour_of_a_later_one(self) -> None:
        self.write_transcript(
            SESSION_ID,
            [
                self.a_prompt_line("<system-reminder>环境信息</system-reminder>"),
                self.a_prompt_line("真正的提示词"),
            ],
        )

        snapshot = self.store().snapshot()

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        self.assertEqual(recent[0]["title"], "真正的提示词")

    def test_a_dead_pid_reports_not_alive(self) -> None:
        self.write_live(
            "999999999.json",
            {"pid": 999999999, "sessionId": SESSION_ID, "cwd": "/tmp/demo",
             "status": "busy", "startedAt": 1795330905785, "updatedAt": 1795330961861},
        )

        snapshot = self.store().snapshot()

        live = snapshot["live"]
        assert isinstance(live, list)
        self.assertFalse(live[0]["alive"])

    def test_unreadable_files_are_skipped(self) -> None:
        (self.claude / "sessions").mkdir(parents=True)
        (self.claude / "sessions" / "broken.json").write_text("{not json")
        project = self.claude / "projects" / "-tmp-demo"
        project.mkdir(parents=True)
        (project / "not-a-session.txt").write_text("ignore me")
        (project / "broken.jsonl").write_text("still\nnot json\n")
        self.write_transcript(SESSION_ID, [self.a_prompt_line("完好")])

        snapshot = self.store().snapshot()

        recent = snapshot["recent"]
        assert isinstance(recent, list)
        self.assertEqual([entry["session_id"] for entry in recent], [SESSION_ID])
        self.assertEqual(snapshot["live"], [])

    def test_an_absent_data_directory_produces_empty_lists(self) -> None:
        snapshot = self.store().snapshot()

        self.assertEqual(snapshot["live"], [])
        self.assertEqual(snapshot["recent"], [])


class SourceTests(unittest.TestCase):
    """The adapter reduces the store's snapshot to what the panel draws."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.claude = self.root / ".claude"

    def source(self) -> ClaudeSessionsSource:
        return ClaudeSessionsSource(ClaudeSessionStore(claude_dir=self.claude))

    def write_transcript(self, session_id: str, lines: list[str]) -> Path:
        project = self.claude / "projects" / "-tmp-demo"
        project.mkdir(parents=True, exist_ok=True)
        path = project / f"{session_id}.jsonl"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def write_live(self, name: str, payload: dict[str, object]) -> None:
        sessions = self.claude / "sessions"
        sessions.mkdir(parents=True, exist_ok=True)
        (sessions / name).write_text(json.dumps(payload), encoding="utf-8")

    def test_live_sessions_reduce_to_a_name_and_a_busy_flag(self) -> None:
        session_id = "aaaaaaaa-1111-2222-3333-444444444444"
        self.write_transcript(
            session_id,
            [json.dumps({"type": "user", "message": {"role": "user", "content": "提示词"},
                         "cwd": "/tmp/demo", "gitBranch": "main"})],
        )
        self.write_live(
            "4242.json",
            {"pid": os.getpid(), "sessionId": session_id, "cwd": "/tmp/demo",
             "name": "Demo session", "status": "busy", "startedAt": 1795330905785,
             "updatedAt": 1795330961861},
        )

        publication = asyncio.run(self.source().collect())

        self.assertEqual(publication.live, (("Demo session", True),))
        # The panel draws the running roster only; the transcript is not read.
        self.assertEqual(publication.recent, ())

    def test_a_session_without_a_name_falls_back_to_the_project(self) -> None:
        session_id = "aaaaaaaa-1111-2222-3333-444444444444"
        self.write_live(
            "4242.json",
            {"pid": os.getpid(), "sessionId": session_id, "cwd": "/tmp/demo",
             "status": "idle", "startedAt": 1795330905785, "updatedAt": 1795330961861},
        )

        publication = asyncio.run(self.source().collect())

        self.assertEqual(publication.live, (("demo", False),))

    def test_ended_sessions_do_not_reach_the_panel(self) -> None:
        self.write_transcript(
            "bbbbbbbb-1111-2222-3333-444444444444",
            [json.dumps({"type": "user", "message": {"role": "user", "content": "已结束"},
                         "cwd": "/tmp/demo"})],
        )

        publication = asyncio.run(self.source().collect())

        # A transcript without a process behind it is history, and this page is
        # about what is running now.
        self.assertEqual(publication.live, ())
        self.assertEqual(publication.recent, ())

    def test_a_dead_process_is_not_live(self) -> None:
        self.write_live(
            "999999999.json",
            {"pid": 999999999, "sessionId": "aaaaaaaa-1111-2222-3333-444444444444",
             "cwd": "/tmp/demo", "name": "Ghost", "status": "busy",
             "startedAt": 1795330905785, "updatedAt": 1795330961861},
        )

        publication = asyncio.run(self.source().collect())

        self.assertEqual(publication.live, ())


class WatchTests(unittest.IsolatedAsyncioTestCase):
    """The panel is fed when Claude writes, not on a cadence."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.sessions = self.root / "sessions"
        self.sessions.mkdir()
        self.source = ClaudeSessionsSource(ClaudeSessionStore(claude_dir=self.root))
        self.addCleanup(self.source.close)

    async def test_a_session_file_write_wakes_the_waiter(self) -> None:
        state = self.sessions / "4242.json"
        state.write_text("{}", encoding="utf-8")
        # The observer needs a moment to report its watch as established.
        waiter = asyncio.create_task(self.source.wait_for_change(30.0))
        await asyncio.sleep(0.3)

        state.write_text('{"pid": 4242}', encoding="utf-8")

        await asyncio.wait_for(waiter, 5.0)

    async def test_a_new_session_file_wakes_the_waiter(self) -> None:
        waiter = asyncio.create_task(self.source.wait_for_change(30.0))
        await asyncio.sleep(0.3)

        (self.sessions / "4242.json").write_text("{}", encoding="utf-8")

        await asyncio.wait_for(waiter, 5.0)

    async def test_an_unwatched_machine_still_returns_after_the_timeout(self) -> None:
        missing = ClaudeSessionsSource(
            ClaudeSessionStore(claude_dir=self.root / "absent")
        )

        started = asyncio.get_running_loop().time()
        await missing.wait_for_change(0.2)

        # No directory to watch, so the wait is bounded by the fallback poll.
        self.assertLess(asyncio.get_running_loop().time() - started, 2.0)


class RouteTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def routes(self) -> ControlRoutes:
        return ControlRoutes(
            SimpleNamespace(),
            token=TOKEN,
            bound_host="127.0.0.1",
            sessions=ClaudeSessionStore(claude_dir=self.root),
        )

    def request(self, target: str, *, method: str = "GET", token: str | None = TOKEN) -> Request:
        headers = {"host": "127.0.0.1:8788"}
        if token is not None:
            headers["x-quotaframe-token"] = token
        return Request(method=method, target=target, headers=headers, body=b"")

    def test_the_sessions_page_serves_with_a_token(self) -> None:
        response = self.routes().handle(self.request("/sessions"))

        body = response.body.decode("utf-8")
        self.assertEqual(response.status, 200)
        self.assertTrue(response.content_type.startswith("text/html"))
        self.assertIn(f'const TOKEN = "{TOKEN}";', body)
        self.assertIn('id="live"', body)
        self.assertIn("/api/cc/sessions", body)
        self.assertNotIn("{{", body)

    def test_the_sessions_page_requires_the_token(self) -> None:
        self.assertEqual(self.routes().handle(self.request("/sessions", token=None)).status, 401)

    def test_the_sessions_api_returns_the_snapshot(self) -> None:
        response = self.routes().handle(self.request("/api/cc/sessions"))

        self.assertEqual(response.status, 200)
        payload = json.loads(response.body)
        self.assertEqual(set(payload), {"now", "live", "recent"})

    def test_the_sessions_api_requires_the_token(self) -> None:
        response = self.routes().handle(
            self.request("/api/cc/sessions", token=None)
        )
        self.assertEqual(response.status, 401)

    def test_posting_to_the_sessions_api_is_refused(self) -> None:
        response = self.routes().handle(self.request("/api/cc/sessions", method="POST"))
        self.assertEqual(response.status, 405)

    def test_the_control_page_links_to_the_sessions_page(self) -> None:
        body = render_page(token=TOKEN)
        self.assertIn('id="to-sessions"', body)
        self.assertIn('"/sessions?token="', body)


if __name__ == "__main__":
    unittest.main()

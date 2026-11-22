"""The sessions page, served as one self-contained document.

Like the control page there is no build step and no bundled asset: the page is
a string, and copy comes from the catalog through `{{key}}` placeholders. It
reads only the sessions API and shares the control page's visual language.
"""

from __future__ import annotations

import html
import json

from quotaframe_bridge.i18n import language, tr

# Strings the script needs after load. Static copy is substituted into the
# markup instead.
_DYNAMIC_KEYS = (
    "sessions_busy",
    "sessions_idle",
    "sessions_alive_false",
    "sessions_started",
    "sessions_last_active",
    "control_send_failed",
)

_TEMPLATE = """<!DOCTYPE html>
<html lang="{{lang}}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>{{sessions_title}}</title>
<style>
  :root {
    color-scheme: light dark;
    --bg: #f6f6f4; --card: #ffffff; --ink: #14161a; --dim: #6b7280;
    --line: #e3e5e8; --accent: #2f6fed; --warn: #b45309; --ok: #16794a;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #14161a; --card: #1d2026; --ink: #e8eaee; --dim: #9aa1ab;
      --line: #2b3038; --accent: #6f9dff; --warn: #f0b429; --ok: #5ad19a;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 24px 16px 48px; background: var(--bg); color: var(--ink);
    font: 15px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  }
  main { max-width: 760px; margin: 0 auto; display: grid; gap: 16px; }
  header { display: flex; align-items: baseline; justify-content: space-between; gap: 12px; }
  h1 { font-size: 20px; margin: 0; }
  h2 { font-size: 13px; margin: 0 0 8px; color: var(--dim); font-weight: 600;
       text-transform: uppercase; letter-spacing: .04em; }
  .meta { color: var(--dim); font-size: 13px; text-align: right; }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 12px;
          padding: 16px; }
  .device { display: grid; gap: 4px; padding: 12px 0; border-top: 1px solid var(--line); }
  .device:first-of-type { border-top: 0; padding-top: 0; }
  .device .row { display: flex; flex-wrap: wrap; gap: 8px; align-items: baseline; }
  .name { font-weight: 600; }
  .pill { font-size: 12px; padding: 2px 8px; border-radius: 999px;
          border: 1px solid var(--line); color: var(--dim); }
  .pill.ok { color: var(--ok); border-color: currentColor; }
  .pill.warn { color: var(--warn); border-color: currentColor; }
  .dim { color: var(--dim); font-size: 13px; }
  .mono { font-variant-numeric: tabular-nums; }
  .empty { color: var(--dim); }
  .back { margin: 0; text-align: center; }
  .back a { color: var(--accent); text-decoration: none; }
  .back a:hover { text-decoration: underline; }
</style>
</head>
<body>
<main>
  <header>
    <h1>{{sessions_title}}</h1>
    <div class="meta mono" id="meta">{{control_loading}}</div>
  </header>

  <section class="card">
    <h2>{{sessions_live}}</h2>
    <div id="live"><p class="empty">{{control_loading}}</p></div>
  </section>

  <section class="card">
    <h2>{{sessions_recent}}</h2>
    <div id="recent"><p class="empty">{{control_loading}}</p></div>
  </section>

  <p class="back"><a id="back">{{sessions_back}}</a></p>
</main>
<script>
const TOKEN = {{token}};
const LABELS = {{labels}};

const nodes = {
  meta: document.getElementById("meta"),
  live: document.getElementById("live"),
  recent: document.getElementById("recent"),
};

document.getElementById("back").href = "/?token=" + encodeURIComponent(TOKEN);

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined && text !== null) node.textContent = text;
  return node;
}

async function api(path) {
  const response = await fetch(path, {
    headers: { "X-QuotaFrame-Token": TOKEN },
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error || response.status);
  return payload;
}

function ago(seconds, now) {
  if (seconds === null || seconds === undefined) return "—";
  const delta = Math.max(0, now - seconds);
  if (delta < 60) return delta + "s";
  const minutes = Math.floor(delta / 60);
  if (minutes < 60) return minutes + "m";
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return hours + "h" + String(minutes % 60).padStart(2, "0") + "m";
  return Math.floor(hours / 24) + "d" + String(hours % 24).padStart(2, "0") + "h";
}

function size(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024 * 1024) return Math.round(bytes / 1024) + " KB";
  return (bytes / 1024 / 1024).toFixed(1) + " MB";
}

// Live entries always carry a state; a transcript joined to a dead process
// still counts as running here because its writer may have crashed.
function statusText(live) {
  if (!live) return null;
  if (!live.alive) return LABELS.sessions_alive_false;
  if (live.status === "busy") return LABELS.sessions_busy;
  return LABELS.sessions_idle;
}

function statusClass(live) {
  return live && live.alive && live.status !== "busy" ? "pill ok" : "pill warn";
}

function shortId(id) {
  return id ? id.slice(0, 8) : "—";
}

function renderLive(sessions, now) {
  nodes.live.replaceChildren();
  if (!sessions.length) {
    nodes.live.append(element("p", "empty", {{sessions_live_none}}));
    return;
  }
  for (const session of sessions) {
    const card = element("div", "device");
    const row = element("div", "row");
    row.append(element("span", "name", session.name || shortId(session.session_id)));
    const text = statusText(session) || LABELS.sessions_idle;
    row.append(element("span", statusClass(session), text));
    card.append(row);
    const detail = [];
    if (session.project) detail.push(session.project);
    if (session.version) detail.push("v" + session.version);
    if (session.entrypoint) detail.push(session.entrypoint);
    if (session.started_at) detail.push(LABELS.sessions_started + " " + ago(session.started_at, now));
    if (session.updated_at) detail.push(LABELS.sessions_last_active + " " + ago(session.updated_at, now));
    card.append(element("div", "dim mono", detail.join(" · ")));
    nodes.live.append(card);
  }
}

function renderRecent(sessions, now) {
  nodes.recent.replaceChildren();
  if (!sessions.length) {
    nodes.recent.append(element("p", "empty", {{sessions_recent_none}}));
    return;
  }
  for (const session of sessions) {
    const card = element("div", "device");
    const row = element("div", "row");
    row.append(element("span", "name", session.title || shortId(session.session_id)));
    const text = statusText(session.live);
    if (text) row.append(element("span", statusClass(session.live), text));
    card.append(row);
    const detail = [];
    if (session.project) detail.push(session.project);
    if (session.branch) detail.push(session.branch);
    if (session.last_at) detail.push(LABELS.sessions_last_active + " " + ago(session.last_at, now));
    detail.push(size(session.size_bytes || 0));
    card.append(element("div", "dim mono", detail.join(" · ")));
    nodes.recent.append(card);
  }
}

function render(data) {
  const now = data.now || Math.floor(Date.now() / 1000);
  const live = data.live || [];
  const running = live.filter((session) => session.alive).length;
  nodes.meta.textContent = running + " " + {{sessions_live_count}};
  renderLive(live, now);
  renderRecent(data.recent || [], now);
}

async function poll() {
  try {
    render(await api("/api/cc/sessions"));
  } catch (error) {
    nodes.meta.textContent = LABELS.control_send_failed + ": " + error.message;
  }
}

async function loop() {
  if (!document.hidden) await poll();
  setTimeout(loop, document.hidden ? 30000 : 5000);
}

loop();
</script>
</body>
</html>
"""


def render_sessions_page(*, token: str) -> str:
    """Return the page with copy and the access token substituted.

    The token reaches the script as a JSON string literal, so a token that
    ever contained a quote could not break out of it.
    """

    page = _TEMPLATE.replace("{{lang}}", "zh" if language() == "zh" else "en")
    for key in _STATIC_KEYS:
        page = page.replace("{{" + key + "}}", html.escape(tr(key)))
    labels = {key: tr(key) for key in _DYNAMIC_KEYS}
    page = page.replace("{{token}}", json.dumps(token))
    page = page.replace("{{labels}}", json.dumps(labels, ensure_ascii=False))
    for key in _JS_KEYS:
        page = page.replace("{{" + key + "}}", json.dumps(tr(key), ensure_ascii=False))
    return page


# Copy the markup renders directly.
_STATIC_KEYS = (
    "sessions_title",
    "sessions_live",
    "sessions_recent",
    "sessions_back",
    "control_loading",
)

# Copy the script embeds as JSON string literals.
_JS_KEYS = (
    "sessions_live_none",
    "sessions_recent_none",
    "sessions_live_count",
)

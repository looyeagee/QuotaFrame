"""The control page, served as one self-contained document.

There is no build step and no bundled asset: the page is a string, so a frozen
Bridge and a source checkout serve exactly the same thing. Copy comes from the
catalog, which is why the markup carries `{{key}}` placeholders instead of text.
"""

from __future__ import annotations

import html
import json

from quotaframe_bridge.i18n import language, tr

# Strings the script needs after load, keyed by the reason code the backend
# reports. Static copy is substituted into the markup instead.
_DYNAMIC_KEYS = (
    "connected",
    "disconnected",
    "usage_short",
    "usage_week",
    "control_balance",
    "control_unavailable_usage",
    "control_send_failed",
    "control_unavailable_closing",
    "control_unavailable_busy",
    "control_unavailable_disconnected",
    "control_unavailable_exclusive",
    "control_unavailable_unsecured",
    "control_unavailable_missing_capability",
    "control_unavailable_unknown_device",
    "control_unavailable_unsupported",
    "control_unavailable_manual",
    "alert_off",
    "alert_page",
    "alert_beep",
    "control_alert_unknown",
    "control_pair_scanning",
    "control_pair_added",
    "control_pair_none_found",
    "control_pair_failed",
    "control_pair_busy",
    "control_pair_unsupported",
    "control_pair_stopped",
    "control_pairing_error_failed",
    "repair",
    "forget",
    "repair_body",
    "forget_body",
    "repair_macos_body",
    "ble_cleanup_failed",
    "devices_unreadable",
)

_TEMPLATE = """<!DOCTYPE html>
<html lang="{{lang}}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>{{control_title}}</title>
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
  .actions { display: flex; flex-wrap: wrap; gap: 10px; }
  button {
    font: inherit; padding: 10px 16px; border-radius: 10px; border: 1px solid var(--line);
    background: var(--card); color: var(--ink); cursor: pointer; min-height: 44px;
  }
  button.primary { background: var(--accent); border-color: transparent; color: #fff; }
  select {
    font: inherit; padding: 10px 12px; border-radius: 10px; border: 1px solid var(--line);
    background: var(--card); color: var(--ink); min-height: 44px;
  }
  button:disabled { opacity: .45; cursor: not-allowed; }
  button:active:not(:disabled) { transform: translateY(1px); }
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
  .usage { display: grid; gap: 10px; }
  .usage .row { display: flex; justify-content: space-between; gap: 12px; }
  .bar { height: 6px; border-radius: 3px; background: var(--line); overflow: hidden; }
  .bar > span { display: block; height: 100%; background: var(--accent); }
  .empty { color: var(--dim); }
  .status { min-height: 20px; font-size: 13px; color: var(--dim); }
  .status.error { color: var(--warn); }
</style>
</head>
<body>
<main>
  <header>
    <h1>{{control_title}}</h1>
    <div class="meta mono" id="meta">{{control_loading}}</div>
  </header>

  <section class="card">
    <h2>{{control_actions}}</h2>
    <div class="actions">
      <button class="primary" id="toggle">{{toggle_screen}}</button>
      <button id="previous">{{previous_page}}</button>
      <button id="next">{{next_page}}</button>
      <button id="refresh">{{refresh}}</button>
    </div>
    <p class="status" id="status" role="status"></p>
  </section>

  <section class="card">
    <h2>{{control_alert}}</h2>
    <div class="actions">
      <select id="alert">
        <option value="0">{{alert_off}}</option>
        <option value="1">{{alert_page}}</option>
        <option value="2">{{alert_beep}}</option>
      </select>
      <button class="primary" id="alert-apply">{{alert_apply}}</button>
    </div>
    <p class="status" id="alert-status" role="status"></p>
  </section>

  <section class="card">
    <h2>{{control_pairing}}</h2>
    <div class="actions">
      <button class="primary" id="pair">{{control_pair_add}}</button>
      <button id="pair-stop" hidden>{{control_pair_stop}}</button>
    </div>
    <p class="status" id="pairing-status" role="status"></p>
  </section>

  <section class="card">
    <h2>{{control_devices}}</h2>
    <div id="devices"><p class="empty">{{control_loading}}</p></div>
  </section>

  <section class="card">
    <h2>{{control_usage}}</h2>
    <div class="usage" id="usage"><p class="empty">{{control_loading}}</p></div>
  </section>

  <section class="card">
    <h2>{{control_now_playing}}</h2>
    <div id="nowplaying"><p class="empty">{{control_no_track}}</p></div>
  </section>
</main>
<script>
const TOKEN = {{token}};
const LABELS = {{labels}};

const nodes = {
  meta: document.getElementById("meta"),
  status: document.getElementById("status"),
  devices: document.getElementById("devices"),
  usage: document.getElementById("usage"),
  nowplaying: document.getElementById("nowplaying"),
  pairing: document.getElementById("pairing-status"),
};
const PROVIDER_NAMES = { deepseek: "DeepSeek", minimax: "MiniMax" };

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined && text !== null) node.textContent = text;
  return node;
}

async function api(path, options) {
  const response = await fetch(path, {
    ...options,
    headers: { "X-QuotaFrame-Token": TOKEN, "Content-Type": "application/json" },
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error || response.status);
  return payload;
}

function say(text, isError) {
  nodes.status.textContent = text || "";
  nodes.status.className = isError ? "status error" : "status";
}

function duration(seconds) {
  if (seconds === null || seconds === undefined) return "—";
  if (seconds < 60) return seconds + "s";
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return minutes + "m" + String(seconds % 60).padStart(2, "0") + "s";
  const hours = Math.floor(minutes / 60);
  return hours + "h" + String(minutes % 60).padStart(2, "0") + "m";
}

function clock(seconds) {
  const total = Math.max(0, seconds || 0);
  return Math.floor(total / 60) + ":" + String(total % 60).padStart(2, "0");
}

function renderMeta(state) {
  const bridge = state.bridge || {};
  const collection = state.collection || {};
  nodes.meta.textContent = [
    (bridge.version || "") + " · " + duration(bridge.uptime_seconds),
    {{control_collections_label}} + " " + (collection.count ?? 0) +
      (collection.consecutive_failures ? " · " + {{control_failures_label}} +
        " " + collection.consecutive_failures : ""),
  ].join("\\n");
  nodes.meta.style.whiteSpace = "pre-line";
}

function reasonText(reason) {
  return LABELS["control_unavailable_" + reason] || reason;
}

function renderDevices(state) {
  const devices = state.devices || [];
  nodes.devices.replaceChildren();
  if (!devices.length) {
    nodes.devices.append(element("p", "empty", {{control_no_devices}}));
    return;
  }
  for (const device of devices) {
    const card = element("div", "device");
    const row = element("div", "row");
    row.append(element("span", "name", device.name));
    const state_pill = element(
      "span",
      "pill " + (device.connected ? "ok" : "warn"),
      device.connected ? LABELS.connected : LABELS.disconnected,
    );
    row.append(state_pill);
    if (device.incompatible) row.append(element("span", "pill warn", "bridge/firmware"));
    card.append(row);
    const detail = [];
    if (device.address) detail.push(device.address);
    if (device.firmware) detail.push(device.firmware);
    if (device.page) detail.push(device.page);
    if (!device.capabilities.length) detail.push("no status");
    card.append(element("div", "dim mono", detail.join(" · ")));
    const blocked = new Set();
    let pending = false;
    for (const key of ["toggle", "page"]) {
      const screen = (device.screen || {})[key] || {};
      pending = pending || Boolean(screen.pending);
      if (screen.available === false && screen.reason) {
        blocked.add(reasonText(screen.reason));
      }
    }
    if (blocked.size) {
      card.append(element("div", "dim", [...blocked].join(" · ")));
    } else if (pending) {
      card.append(element("div", "dim", {{control_pending}}));
    }
    card.append(pairingActions(device));
    nodes.devices.append(card);
  }
  const anyToggle = devices.some((d) => (d.screen || {}).toggle?.available);
  const anyPage = devices.some((d) => (d.screen || {}).page?.available);
  document.getElementById("toggle").disabled = !anyToggle;
  document.getElementById("previous").disabled = !anyPage;
  document.getElementById("next").disabled = !anyPage;
}

function availabilityTitle(availability) {
  return availability.available === false && availability.reason
    ? reasonText(availability.reason) : "";
}

function pairingActions(device) {
  const pairing = device.pairing || {};
  const actions = element("div", "row");
  const repair = pairing.repair || {};
  const repairButton = element("button", null, LABELS.repair);
  repairButton.disabled = !repair.available;
  repairButton.title = availabilityTitle(repair);
  if (repair.mode === "manual") {
    // Nothing in the Bridge can drop a macOS bond; the tray shows the same
    // steps instead of pretending a request went out.
    repairButton.addEventListener("click", () => say(LABELS.repair_macos_body));
  } else {
    repairButton.addEventListener("click", (event) =>
      pairCommand(event.currentTarget, "/api/pair/repair", device, LABELS.repair_body));
  }
  actions.append(repairButton);
  const forget = pairing.forget || {};
  const forgetButton = element("button", null, LABELS.forget);
  forgetButton.disabled = !forget.available;
  forgetButton.title = availabilityTitle(forget);
  forgetButton.addEventListener("click", (event) =>
    pairCommand(event.currentTarget, "/api/pair/forget", device, LABELS.forget_body));
  actions.append(forgetButton);
  return actions;
}

async function pairCommand(button, path, device, confirmText) {
  if (!window.confirm(confirmText)) return;
  await command(button, path, { address: device.address });
}

function usageRow(label, window, suffix) {
  const wrapper = element("div");
  const row = element("div", "row");
  row.append(element("span", null, label + " " + suffix));
  row.append(element("span", "mono", window.used_percent + "%"));
  wrapper.append(row);
  const bar = element("div", "bar");
  const fill = element("span");
  fill.style.width = Math.max(0, Math.min(100, window.used_percent)) + "%";
  bar.append(fill);
  wrapper.append(bar);
  return wrapper;
}

function renderUsage(state) {
  const providers = state.providers || [];
  nodes.usage.replaceChildren();
  if (!providers.length) {
    nodes.usage.append(element("p", "empty", {{control_no_usage}}));
    return;
  }
  for (const usage of providers) {
    const name = PROVIDER_NAMES[usage.provider] || usage.provider;
    if (usage.state === "unavailable") {
      nodes.usage.append(element("div", "dim", name + " · " + LABELS.control_unavailable_usage));
      continue;
    }
    if (usage.balance !== null) {
      const row = element("div", "row");
      row.append(element("span", null, name));
      row.append(element("span", "mono",
        LABELS.control_balance + " " + usage.balance + " " + (usage.currency || "")));
      nodes.usage.append(row);
    }
    if (usage.short) {
      nodes.usage.append(usageRow(name, usage.short, LABELS.usage_short));
    }
    if (usage.week) {
      nodes.usage.append(usageRow(name, usage.week, LABELS.usage_week));
    }
  }
}

function renderNowPlaying(state) {
  const track = state.now_playing;
  nodes.nowplaying.replaceChildren();
  if (!track) {
    nodes.nowplaying.append(element("p", "empty", {{control_no_track}}));
    return;
  }
  nodes.nowplaying.append(element("div", "name", track.title));
  const detail = [track.artist, track.album].filter(Boolean).join(" · ");
  if (detail) nodes.nowplaying.append(element("div", "dim", detail));
  nodes.nowplaying.append(element("div", "dim mono",
    clock(track.position_seconds) + " / " + clock(track.duration_seconds) +
    (track.playing ? "" : " · " + {{control_paused}})));
}

const ALERT_LABELS = { 0: LABELS.alert_off, 1: LABELS.alert_page, 2: LABELS.alert_beep };
const alertSelect = document.getElementById("alert");
const alertApply = document.getElementById("alert-apply");
const alertStatus = document.getElementById("alert-status");
// A poll must not overwrite a choice the person has not applied yet.
let alertDirty = false;
alertSelect.addEventListener("change", () => { alertDirty = true; });

function renderAlert(state) {
  const devices = state.devices || [];
  const modes = devices.map((device) => (device.alert || {}).mode);
  const reported = modes.filter((mode) => typeof mode === "number");
  alertApply.disabled = !devices.some((device) => (device.alert || {}).set?.available);
  // Follow the panels when they agree; a mixed fleet keeps the manual choice.
  if (!alertDirty && reported.length &&
      reported.every((mode) => mode === reported[0])) {
    alertSelect.value = String(reported[0]);
  }
  const parts = devices.map((device) => {
    const alert = device.alert || {};
    const current = typeof alert.mode === "number"
      ? ALERT_LABELS[alert.mode] : LABELS.control_alert_unknown;
    const blocked = alert.set && alert.set.available === false && alert.set.reason
      ? " · " + reasonText(alert.set.reason) : "";
    return device.name + ": " + current + blocked;
  });
  alertStatus.textContent = parts.join("\\n");
  alertStatus.style.whiteSpace = "pre-line";
  alertStatus.className = "status";
}

const PAIRING_ERRORS = {
  cleanup_failed: LABELS.ble_cleanup_failed,
  busy: LABELS.control_unavailable_exclusive,
};

function pairingErrorText(error) {
  return PAIRING_ERRORS[error] || LABELS.control_pairing_error_failed;
}

function adoptionResultText(result) {
  if (result.stopped) return LABELS.control_pair_stopped;
  if (result.error === "records") return LABELS.devices_unreadable;
  if (result.error) return LABELS.control_pair_failed;
  const names = result.adopted || [];
  if (!names.length) return LABELS.control_pair_none_found;
  return LABELS.control_pair_added + " " + names.join(" · ");
}

const pairButton = document.getElementById("pair");
const pairStopButton = document.getElementById("pair-stop");

function renderPairing(state) {
  const pairing = state.pairing || {};
  const adoption = pairing.adoption || {};
  const running = Boolean(adoption.running);
  // Scanning owns the card: only stopping it makes sense while it runs.
  pairButton.hidden = running;
  pairStopButton.hidden = !running;
  pairButton.disabled = !pairing.supported || running;
  let text = "";
  let failed = false;
  if (!pairing.supported) {
    text = LABELS.control_pair_unsupported;
  } else if (running) {
    text = LABELS.control_pair_scanning;
  } else if (adoption.last) {
    text = adoptionResultText(adoption.last);
    failed = Boolean(adoption.last.error);
  }
  // A queued repair or removal can still fail while it runs; the outcome
  // arrives with the next poll instead of the command's reply.
  const error = pairing.last_error;
  if (error) {
    text = (text ? text + "\\n" : "") + error.device + ": " + pairingErrorText(error.error);
    failed = true;
  }
  nodes.pairing.textContent = text;
  nodes.pairing.style.whiteSpace = "pre-line";
  nodes.pairing.className = failed ? "status error" : "status";
}

function render(state) {
  renderMeta(state);
  renderDevices(state);
  renderAlert(state);
  renderPairing(state);
  renderUsage(state);
  renderNowPlaying(state);
}

async function poll() {
  try {
    render(await api("/api/state"));
  } catch (error) {
    say(LABELS.control_send_failed + ": " + error.message, true);
  }
}

function describe(payload) {
  const devices = payload.devices;
  if (!devices) return {{control_sent}};
  const accepted = devices.filter((d) => d.accepted).length;
  if (!devices.length) return {{control_no_devices}};
  if (accepted === devices.length) return {{control_sent}};
  const reasons = devices.filter((d) => !d.accepted)
    .map((d) => d.name + ": " + reasonText(d.reason));
  return (accepted ? {{control_sent}} + " · " : "") + reasons.join(" · ");
}

async function command(button, path, body) {
  button.disabled = true;
  try {
    say(describe(await api(path, {
      method: "POST",
      body: body ? JSON.stringify(body) : "{}",
    })));
  } catch (error) {
    say(LABELS.control_send_failed + ": " + error.message, true);
  } finally {
    button.disabled = false;
    await poll();
    // The session holds the command until the panel acknowledges it, so the
    // first poll can still see it in flight; sample again until it has landed.
    setTimeout(poll, 250);
    setTimeout(poll, 900);
  }
}

document.getElementById("toggle").addEventListener("click", (event) =>
  command(event.currentTarget, "/api/screen/toggle"));
document.getElementById("previous").addEventListener("click", (event) =>
  command(event.currentTarget, "/api/screen/page", { direction: "previous" }));
document.getElementById("next").addEventListener("click", (event) =>
  command(event.currentTarget, "/api/screen/page", { direction: "next" }));
document.getElementById("refresh").addEventListener("click", async (event) => {
  await command(event.currentTarget, "/api/refresh");
});
alertApply.addEventListener("click", async (event) => {
  const mode = Number(alertSelect.value);
  alertDirty = false;
  await command(event.currentTarget, "/api/alert", { mode });
});

pairButton.addEventListener("click", async (event) => {
  const button = event.currentTarget;
  button.disabled = true;
  try {
    const result = await api("/api/pair", { method: "POST", body: "{}" });
    if (!result.started) say(LABELS.control_pair_busy, result.reason !== "busy");
  } catch (error) {
    say(LABELS.control_send_failed + ": " + error.message, true);
  } finally {
    button.disabled = false;
    await poll();
    // Adoption scans for boards that may still be booting, so the first
    // state after "started" can still change again before the scan ends.
    setTimeout(poll, 900);
  }
});

pairStopButton.addEventListener("click", async (event) => {
  const button = event.currentTarget;
  button.disabled = true;
  try {
    await api("/api/pair/stop", { method: "POST", body: "{}" });
  } catch (error) {
    say(LABELS.control_send_failed + ": " + error.message, true);
  } finally {
    button.disabled = false;
    await poll();
    // Cancellation lands on the next loop iteration; sample again so the
    // "scan stopped" result and the restored button are both visible.
    setTimeout(poll, 250);
    setTimeout(poll, 900);
  }
});

async function loop() {
  if (!document.hidden) await poll();
  setTimeout(loop, document.hidden ? 15000 : 3000);
}

loop();
</script>
</body>
</html>
"""


def render_page(*, token: str) -> str:
    """Return the page with copy and the access token substituted.

    The token reaches the script as a JSON string literal, so a token that ever
    contained a quote could not break out of it.
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
    "control_title",
    "control_actions",
    "control_alert",
    "alert_off",
    "alert_page",
    "alert_beep",
    "alert_apply",
    "control_pairing",
    "control_pair_add",
    "control_pair_stop",
    "toggle_screen",
    "previous_page",
    "next_page",
    "refresh",
    "control_devices",
    "control_usage",
    "control_now_playing",
    "control_loading",
)

# Copy the script embeds as JSON string literals.
_JS_KEYS = (
    "control_collections_label",
    "control_failures_label",
    "control_no_devices",
    "control_no_usage",
    "control_no_track",
    "control_sent",
    "control_pending",
    "control_paused",
)

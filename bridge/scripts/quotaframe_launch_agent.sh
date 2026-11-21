#!/bin/bash
# Login agent for the console Bridge on macOS.
#
# launchd runs this same script with `run`, so every path is derived from the
# script's own location and the generated plist is the only file that holds a
# machine-specific path.
#
# The log lives next to the tray entry point's log but under its own name: that
# one is written through a RotatingFileHandler, and rotating a file while
# launchd still holds the descriptor would send later lines to the renamed
# file. launchd owns this descriptor instead, so the runner trims it before
# each start.

set -euo pipefail

LABEL="com.quotaframe.bridge"
LOG_DIR="${QUOTAFRAME_LOG_DIR:-$HOME/Library/Logs/quotaframe}"
LOG_FILE="$LOG_DIR/bridge-console.log"
LOG_LEVEL="${QUOTAFRAME_LOG_LEVEL:-INFO}"
MAX_LOG_BYTES=$((8 * 1024 * 1024))

script_path="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$(basename -- "${BASH_SOURCE[0]}")"
bridge_dir="$(dirname -- "$(dirname -- "$script_path")")"
repository_root="$(dirname -- "$bridge_dir")"
python="$repository_root/.venv/bin/python"
adapter_path="$repository_root/assets/media-control/"
plist_path="$HOME/Library/LaunchAgents/$LABEL.plist"
domain="gui/$(id -u)"

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

trim_log() {
    mkdir -p "$LOG_DIR"
    if [[ -f "$LOG_FILE" ]]; then
        local size
        size="$(stat -f %z "$LOG_FILE")"
        if ((size > MAX_LOG_BYTES)); then
            mv -f "$LOG_FILE" "$LOG_FILE.1"
        fi
    fi
}

# A terminated Bridge never reaches its cleanup code, and the MediaRemote
# adapter it spawned is not collateral of that signal, so every start reclaims
# the adapter clients of this checkout before opening a new one.
reap_adapters() {
    pkill -f "$adapter_path" 2>/dev/null || true
}

cmd_run() {
    trim_log
    reap_adapters
    cd -- "$bridge_dir"
    # exec keeps the launched process identical to the interpreter, so signals
    # and the exit status launchd observes belong to the Bridge itself.
    exec "$python" -m quotaframe_bridge --log-level "$LOG_LEVEL"
}

write_plist() {
    mkdir -p "$LOG_DIR" "$HOME/Library/LaunchAgents"
    cat >"$plist_path" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/bash</string>
        <string>$script_path</string>
        <string>run</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>ExitTimeOut</key>
    <integer>20</integer>
    <key>LimitLoadToSessionType</key>
    <string>Aqua</string>
    <key>WorkingDirectory</key>
    <string>$bridge_dir</string>
    <key>StandardOutPath</key>
    <string>$LOG_FILE</string>
    <key>StandardErrorPath</key>
    <string>$LOG_FILE</string>
    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
        <key>QUOTAFRAME_LOG_LEVEL</key>
        <string>$LOG_LEVEL</string>
    </dict>
</dict>
</plist>
PLIST
    plutil -lint "$plist_path" >/dev/null
}

cmd_install() {
    [[ -x "$python" ]] || die "no interpreter at $python"
    [[ -d "$bridge_dir/src/quotaframe_bridge" ]] || die "no package at $bridge_dir/src/quotaframe_bridge"
    write_plist
    launchctl bootout "$domain/$LABEL" 2>/dev/null || true
    launchctl bootstrap "$domain" "$plist_path"
    launchctl enable "$domain/$LABEL" 2>/dev/null || true
    printf 'installed %s\n' "$plist_path"
    cmd_status
}

cmd_uninstall() {
    launchctl bootout "$domain/$LABEL" 2>/dev/null || true
    reap_adapters
    rm -f "$plist_path"
    printf 'removed %s\n' "$plist_path"
}

cmd_start() {
    [[ -f "$plist_path" ]] || die "run install first"
    launchctl bootstrap "$domain" "$plist_path" 2>/dev/null || launchctl kickstart "$domain/$LABEL"
}

cmd_stop() {
    launchctl bootout "$domain/$LABEL" 2>/dev/null || true
    reap_adapters
}

cmd_restart() {
    launchctl bootout "$domain/$LABEL" 2>/dev/null || true
    reap_adapters
    launchctl bootstrap "$domain" "$plist_path"
}

cmd_status() {
    if launchctl print "$domain/$LABEL" >/dev/null 2>&1; then
        launchctl print "$domain/$LABEL" | awk '
            /^\tstate = / || /^\tpid = / || /^\truns = / || /^\tlast exit code = / ||
            /^\tprogram = / || /^\tpath = / { sub(/^\t+/, ""); print }
        '
    else
        printf 'agent not loaded\n'
    fi
    local found=0
    for pattern in "quotaframe_bridge --log-level" "quotaframe_launch_agent.sh run"; do
        if pgrep -fl "$pattern" 2>/dev/null; then
            found=1
        fi
    done
    ((found)) || printf 'no bridge process\n'
    pgrep -fl "mediaremote-adapter.pl" || printf 'no media-remote adapter\n'
    printf 'log: %s (%s)\n' "$LOG_FILE" "$(du -h "$LOG_FILE" 2>/dev/null | cut -f1)"
}

cmd_logs() {
    mkdir -p "$LOG_DIR"
    exec tail -n 40 -f "$LOG_FILE"
}

case "${1:-status}" in
    run) cmd_run ;;
    install) cmd_install ;;
    uninstall) cmd_uninstall ;;
    start) cmd_start ;;
    stop) cmd_stop ;;
    restart) cmd_restart ;;
    status) cmd_status ;;
    logs) cmd_logs ;;
    *) die "usage: $0 {install|uninstall|start|stop|restart|status|logs}" ;;
esac

#!/usr/bin/env bash

set -Eeuo pipefail

[[ $# == 1 && "$1" == *.app && -d "$1" ]] || exit 1
readonly APP_PATH="$(cd -- "$1" && pwd -P)"
[[ "$(/usr/bin/plutil -extract CFBundleIdentifier raw "${APP_PATH}/Contents/Info.plist")" == 'com.noxshell.ops' ]] || exit 1
/usr/bin/codesign --verify --deep --strict "${APP_PATH}"
readonly CHECK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/noxshell-startup-check.XXXXXX")"
child_pid=''
watchdog_pid=''
cleanup() {
    if [[ -n "${watchdog_pid}" ]]; then kill "${watchdog_pid}" 2>/dev/null || true; wait "${watchdog_pid}" 2>/dev/null || true; fi
    if [[ -n "${child_pid}" ]]; then kill "${child_pid}" 2>/dev/null || true; wait "${child_pid}" 2>/dev/null || true; fi
    # Only the dedicated mktemp directory belongs to this check.
    case "${CHECK_DIR}" in
        "${TMPDIR:-/tmp}"/noxshell-startup-check.*) rm -rf -- "${CHECK_DIR}" ;;
    esac
}
trap cleanup EXIT

# Exercise the actual deployed Cocoa/SQLite plugins, not the build-tree app or
# libraries supplied by a developer's environment. No real host data is opened.
env -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH -u QT_QPA_PLATFORM \
    -u DYLD_FRAMEWORK_PATH -u DYLD_LIBRARY_PATH \
    "${APP_PATH}/Contents/MacOS/NoxShell" --startup-smoke-test >"${CHECK_DIR}/startup.log" 2>&1 &
child_pid=$!
(sleep 20; kill -KILL "${child_pid}" 2>/dev/null || true) &
watchdog_pid=$!
status=0
wait "${child_pid}" || status=$?
child_pid=''
kill "${watchdog_pid}" 2>/dev/null || true
wait "${watchdog_pid}" 2>/dev/null || true
watchdog_pid=''
cat "${CHECK_DIR}/startup.log"
[[ "${status}" == 0 ]] && /usr/bin/grep -q '^NOXSHELL_STARTUP_SMOKE_OK$' "${CHECK_DIR}/startup.log" || {
    printf '打包后启动检查失败（退出码 %s），禁止生成可发布安装包。\n' "${status}" >&2
    exit 1
}
/usr/bin/codesign --verify --deep --strict "${APP_PATH}"

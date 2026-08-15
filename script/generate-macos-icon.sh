#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly SOURCE_PNG="${1:-${PROJECT_DIR}/assets/app-icon.png}"
readonly OUTPUT_ICNS="${2:-${PROJECT_DIR}/assets/NoxShell.icns}"

[[ "$(uname -s)" == "Darwin" ]] || { printf '生成 .icns 需要 macOS。\n' >&2; exit 1; }
[[ -f "${SOURCE_PNG}" ]] || { printf '未找到图标源文件：%s\n' "${SOURCE_PNG}" >&2; exit 1; }
command -v png2icns >/dev/null 2>&1 || { printf '未找到 png2icns；请先执行 brew install libicns。\n' >&2; exit 1; }
command -v xcrun >/dev/null 2>&1 || { printf '未找到 xcrun。\n' >&2; exit 1; }

readonly WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/noxshell-icon.XXXXXX")"
readonly ICONSET_DIR="${WORK_DIR}/NoxShell.iconset"
readonly MODULE_CACHE_DIR="${WORK_DIR}/module-cache"
cleanup() {
    case "${WORK_DIR}" in
        "${TMPDIR:-/tmp}"/noxshell-icon.*|/tmp/noxshell-icon.*) rm -rf -- "${WORK_DIR}" ;;
    esac
}
trap cleanup EXIT
mkdir -p "${ICONSET_DIR}" "${MODULE_CACHE_DIR}" "$(dirname -- "${OUTPUT_ICNS}")"

CLANG_MODULE_CACHE_PATH="${MODULE_CACHE_DIR}" SWIFT_MODULECACHE_PATH="${MODULE_CACHE_DIR}" \
    xcrun swift "${SCRIPT_DIR}/GenerateMacIconset.swift" "${SOURCE_PNG}" "${ICONSET_DIR}"
png2icns "${OUTPUT_ICNS}" \
    "${ICONSET_DIR}/icon_16x16.png" \
    "${ICONSET_DIR}/icon_32x32.png" \
    "${ICONSET_DIR}/icon_128x128.png" \
    "${ICONSET_DIR}/icon_256x256.png" \
    "${ICONSET_DIR}/icon_512x512.png" \
    "${ICONSET_DIR}/icon_512x512@2x.png"
printf '已生成 macOS 图标：%s\n' "${OUTPUT_ICNS}"

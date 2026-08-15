#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly VERSION="$(sed -nE 's/^project\(NoxShell VERSION ([0-9.]+).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")"
readonly BUILD_DIR="${NOXSHELL_RELEASE_BUILD_DIR:-${PROJECT_DIR}/build-release}"
readonly OUTPUT_DIR="${PROJECT_DIR}/output"
readonly MACOS_DEPLOYMENT_TARGET="${NOXSHELL_MACOS_DEPLOYMENT_TARGET:-14.0}"

[[ -n "${VERSION}" ]] || { printf '无法读取项目版本。\n' >&2; exit 1; }
mkdir -p "${OUTPUT_DIR}"

printf '构建并测试玄壳 v%s…\n' "${VERSION}"
NOXSHELL_MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
    QT_UI_BUILD_DIR="${BUILD_DIR}" \
    "${SCRIPT_DIR}/qt-ui-start.sh" --release --build-only --test

readonly STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/noxshell-package.XXXXXX")"
cleanup() {
    case "${STAGE_DIR}" in
        "${TMPDIR:-/tmp}"/noxshell-package.*|/tmp/noxshell-package.*) rm -rf -- "${STAGE_DIR}" ;;
    esac
}
trap cleanup EXIT

cmake --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"

case "$(uname -s)" in
    Darwin)
        readonly APP_PATH="${STAGE_DIR}/NoxShell.app"
        [[ -d "${APP_PATH}" ]] || { printf '安装阶段未生成应用包：%s\n' "${APP_PATH}" >&2; exit 1; }
        readonly QT_PREFIX="${QT_ROOT:-$(brew --prefix qt)}"
        readonly PLUGIN_ROOT="${QT_PREFIX}/share/qt/plugins"
        readonly PLUGIN_DIR="${APP_PATH}/Contents/PlugIns"
        mkdir -p "${PLUGIN_DIR}/platforms" "${PLUGIN_DIR}/sqldrivers" "${PLUGIN_DIR}/styles" \
            "${PLUGIN_DIR}/iconengines"
        cp "${PLUGIN_ROOT}/platforms/libqcocoa.dylib" "${PLUGIN_DIR}/platforms/"
        cp "${PLUGIN_ROOT}/sqldrivers/libqsqlite.dylib" "${PLUGIN_DIR}/sqldrivers/"
        cp "${PLUGIN_ROOT}/styles/libqmacstyle.dylib" "${PLUGIN_DIR}/styles/"
        cp "${PLUGIN_ROOT}/iconengines/libqsvgicon.dylib" "${PLUGIN_DIR}/iconengines/"
        local_plugins=(
            "${PLUGIN_DIR}/platforms/libqcocoa.dylib"
            "${PLUGIN_DIR}/sqldrivers/libqsqlite.dylib"
            "${PLUGIN_DIR}/styles/libqmacstyle.dylib"
            "${PLUGIN_DIR}/iconengines/libqsvgicon.dylib"
        )
        deploy_args=("${APP_PATH}" -no-plugins -always-overwrite)
        for plugin in "${local_plugins[@]}"; do deploy_args+=("-executable=${plugin}"); done
        "${QT_PREFIX}/bin/macdeployqt" "${deploy_args[@]}"

        readonly PLIST_PATH="${APP_PATH}/Contents/Info.plist"
        readonly BINARY_PATH="${APP_PATH}/Contents/MacOS/NoxShell"
        readonly PLIST_MINIMUM="$(plutil -extract LSMinimumSystemVersion raw "${PLIST_PATH}")"
        readonly BINARY_MINIMUM="$(vtool -show-build "${BINARY_PATH}" | awk '/minos/{print $2; exit}')"
        [[ "${PLIST_MINIMUM}" == "${MACOS_DEPLOYMENT_TARGET}" ]] || {
            printf 'Info.plist 最低系统版本异常：期望 %s，实际 %s\n' \
                "${MACOS_DEPLOYMENT_TARGET}" "${PLIST_MINIMUM}" >&2
            exit 1
        }
        [[ "${BINARY_MINIMUM}" == "${MACOS_DEPLOYMENT_TARGET}" ]] || {
            printf '主程序最低系统版本异常：期望 %s，实际 %s\n' \
                "${MACOS_DEPLOYMENT_TARGET}" "${BINARY_MINIMUM}" >&2
            exit 1
        }

        readonly ARCH="$(uname -m)"
        codesign --verify --deep --strict "${APP_PATH}" 2>/dev/null || \
            printf '提示：当前包未使用 Developer ID 签名；内测可用，公开分发前需签名和公证。\n'

        readonly DMG_ROOT="${STAGE_DIR}/dmg-root"
        readonly DMG_PATH="${OUTPUT_DIR}/玄壳-v${VERSION}-macOS-${ARCH}.dmg"
        mkdir -p "${DMG_ROOT}"
        ditto "${APP_PATH}" "${DMG_ROOT}/玄壳.app"
        ln -s /Applications "${DMG_ROOT}/Applications"
        hdiutil create -quiet -ov -volname "玄壳" -fs HFS+ -format ULMO \
            -srcfolder "${DMG_ROOT}" "${DMG_PATH}"

        if [[ "${NOXSHELL_CREATE_ZIP:-0}" == "1" ]]; then
            readonly ZIP_PATH="${OUTPUT_DIR}/玄壳-v${VERSION}-macOS-${ARCH}.zip"
            ditto -c -k --sequesterRsrc --keepParent "${APP_PATH}" "${ZIP_PATH}"
            printf '备用 ZIP：%s\n' "${ZIP_PATH}"
        fi
        printf 'DMG 安装包：%s\n最低系统：macOS %s\n' "${DMG_PATH}" "${MACOS_DEPLOYMENT_TARGET}"
        ;;
    Linux)
        readonly ARCH="$(uname -m)"
        readonly ARCHIVE_PATH="${OUTPUT_DIR}/NoxShell-v${VERSION}-Linux-${ARCH}.tar.gz"
        tar -C "${STAGE_DIR}" -czf "${ARCHIVE_PATH}" .
        printf '发布包：%s\n' "${ARCHIVE_PATH}"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        command -v makensis >/dev/null 2>&1 || {
            printf '未找到 NSIS（makensis），请先安装 NSIS 3.03 或更高版本。\n' >&2
            exit 1
        }
        cpack --config "${BUILD_DIR}/CPackConfig.cmake" -C Release -B "${OUTPUT_DIR}"
        printf 'Windows 安装包与便携包已写入：%s\n' "${OUTPUT_DIR}"
        ;;
    *)
        printf '不支持的打包平台：%s\n' "$(uname -s)" >&2
        exit 1
        ;;
esac

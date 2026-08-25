#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${QT_UI_BUILD_DIR:-${PROJECT_DIR}/build}"
BUILD_TYPE="${QT_UI_BUILD_TYPE:-Debug}"
JOBS="${QT_UI_JOBS:-}"
RUN_TESTS=0
BUILD_ONLY=0
CLEAN_BUILD=0
APP_ARGS=()

usage() {
    printf '%s\n' \
        "用法: ./script/qt-ui-start.sh [选项] [-- 应用参数...]" \
        "" \
        "增量配置、编译并启动玄壳。再次执行时 CMake 只编译变更文件。" \
        "" \
        "选项:" \
        "  --test          编译后运行 ctest，再启动应用" \
        "  --build-only    只增量编译，不启动应用" \
        "  --clean         删除当前构建目录后重新配置（会丢失构建缓存）" \
        "  --release       使用 Release 构建，默认 Debug" \
        "  -j, --jobs N    并行编译任务数" \
        "  -h, --help      显示帮助" \
        "" \
        "环境变量:" \
        "  QT_UI_BUILD_DIR   构建目录，默认 ./build" \
        "  QT_UI_BUILD_TYPE  Debug 或 Release" \
        "  QT_UI_JOBS        并行任务数" \
        "  QT_ROOT           Qt 安装前缀；未设置时自动探测 Homebrew/qmake" \
        "  NOXSHELL_MACOS_DEPLOYMENT_TARGET  macOS 最低版本，默认 14.0" \
        "" \
        "示例:" \
        "  ./script/qt-ui-start.sh" \
        "  ./script/qt-ui-start.sh --test" \
        "  ./script/qt-ui-start.sh --build-only -j 8"
}

fail() {
    printf '错误: %s\n' "$*" >&2
    exit 1
}

while (($# > 0)); do
    case "$1" in
        --test)
            RUN_TESTS=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        --clean)
            CLEAN_BUILD=1
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        -j|--jobs)
            (($# >= 2)) || fail "$1 需要一个任务数"
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            APP_ARGS=("$@")
            break
            ;;
        *)
            fail "未知选项: $1（使用 --help 查看帮助）"
            ;;
    esac
done

command -v cmake >/dev/null 2>&1 || fail "未找到 cmake，请先安装 CMake"

find_qt_root() {
    if [[ -n "${QT_ROOT:-}" && -d "${QT_ROOT}" ]]; then
        printf '%s\n' "${QT_ROOT}"
        return
    fi

    if command -v brew >/dev/null 2>&1; then
        local brew_qt
        brew_qt="$(brew --prefix qt 2>/dev/null || true)"
        if [[ -n "${brew_qt}" && -d "${brew_qt}" ]]; then
            printf '%s\n' "${brew_qt}"
            return
        fi
    fi

    local qmake_path
    qmake_path="$(command -v qmake6 || command -v qmake || true)"
    if [[ -n "${qmake_path}" ]]; then
        cd -- "$(dirname -- "${qmake_path}")/.." && pwd
        return
    fi

    fail "未找到 Qt 6；可通过 QT_ROOT 指定 Qt 安装前缀"
}

QT_PREFIX="$(find_qt_root)"

if ((CLEAN_BUILD)); then
    case "${BUILD_DIR}" in
        "${PROJECT_DIR}"/build|"${PROJECT_DIR}"/build-*) ;;
        *) fail "为避免误删，--clean 仅允许清理项目内的 build 或 build-* 目录: ${BUILD_DIR}" ;;
    esac
    if [[ -d "${BUILD_DIR}" ]]; then
        printf '清理构建目录: %s\n' "${BUILD_DIR}"
        rm -rf -- "${BUILD_DIR}"
    fi
fi

printf '项目: %s\nQt: %s\n构建: %s (%s)\n' \
    "${PROJECT_DIR}" "${QT_PREFIX}" "${BUILD_DIR}" "${BUILD_TYPE}"

cmake_args=(
    -S "${PROJECT_DIR}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_PREFIX_PATH="${QT_PREFIX}"
    -DBUILD_TESTING=ON
)
if [[ "$(uname -s)" == "Darwin" ]]; then
    cmake_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${NOXSHELL_MACOS_DEPLOYMENT_TARGET:-14.0}")
fi
cmake "${cmake_args[@]}"

build_args=(--build "${BUILD_DIR}" --parallel)
if [[ -n "${JOBS}" ]]; then
    [[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || fail "并行任务数必须是正整数: ${JOBS}"
    build_args+=("${JOBS}")
fi
cmake "${build_args[@]}"

if ((RUN_TESTS)); then
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

if ((BUILD_ONLY)); then
    printf '增量编译完成。\n'
    exit 0
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    APP_PATH="${BUILD_DIR}/NoxShell.app"
    [[ -d "${APP_PATH}" ]] || fail "未找到应用包: ${APP_PATH}"
    # 测试脚本的目标是查看本次增量构建。先关闭仍在运行的旧实例，
    # 否则 macOS 可能把旧窗口置前，让源码文字已经更新却看起来没有变化。
    if pgrep -x NoxShell >/dev/null 2>&1; then
        printf '关闭旧的玄壳测试实例…\n'
        pkill -x NoxShell || true
        for _ in 1 2 3 4 5; do
            pgrep -x NoxShell >/dev/null 2>&1 || break
            sleep 0.1
        done
    fi
    printf '启动: %s\n' "${APP_PATH}"
    if ((${#APP_ARGS[@]} > 0)); then
        # `open` 默认会激活已经运行的旧进程，导致刚编译的界面看起来没有更新。
        # `-n` 强制从本次构建产物启动一个新实例。
        open -n "${APP_PATH}" --args "${APP_ARGS[@]}"
    else
        open -n "${APP_PATH}"
    fi
else
    APP_PATH="${BUILD_DIR}/NoxShell"
    [[ -x "${APP_PATH}" ]] || fail "未找到可执行文件: ${APP_PATH}"
    printf '启动: %s\n' "${APP_PATH}"
    exec "${APP_PATH}" "${APP_ARGS[@]}"
fi

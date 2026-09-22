#!/usr/bin/env bash

# Packaging policy only; sourcing this file does not build, sign or read secrets.
noxshell_package_options() {
    NOXSHELL_PACKAGE_MODE=local
    NOXSHELL_PACKAGE_HELP=0
    local chosen=''
    while (($#)); do
        case "$1" in
            --local|--release)
                if [[ -n "${chosen}" && "${chosen}" != "$1" ]]; then
                    printf '不能同时指定 --local 和 --release。\n' >&2
                    return 1
                fi
                chosen="$1"
                NOXSHELL_PACKAGE_MODE="${1#--}"
                ;;
            -h|--help) NOXSHELL_PACKAGE_HELP=1 ;;
            *) printf '未知打包选项：%s（使用 --help 查看帮助）\n' "$1" >&2; return 1 ;;
        esac
        shift
    done
}

noxshell_package_signing() {
    case "${NOXSHELL_PACKAGE_MODE}" in
        local)
            printf '本地测试打包：未配置证书时使用临时签名，不代表正式签名或公证。\n'
            export NOXSHELL_ALLOW_ADHOC_SIGNING=1
            ;;
        release)
            printf '正式发布打包：必须通过 Developer ID 身份检查。\n'
            # Explicit release must override even an inherited test opt-in.
            export NOXSHELL_ALLOW_ADHOC_SIGNING=0
            ;;
        *) return 1 ;;
    esac
    # Do not erase a configured identity: malformed/partial/unavailable
    # release credentials must still fail, never fall back to test signing.
    noxshell_load_signing_config
}

noxshell_version_valid() {
    [[ "$1" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]
}

noxshell_version_greater() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        split(a, x, "."); split(b, y, ".");
        for (i = 1; i <= 3; i++) {
            if (x[i]+0 > y[i]+0) exit 0;
            if (x[i]+0 < y[i]+0) exit 1;
        }
        exit 1;
    }'
}

noxshell_binary_minimum() {
    # Handles both LC_BUILD_VERSION and older LC_VERSION_MIN_MACOSX. Do not
    # mistake the SDK used to compile the binary for its runtime requirement.
    local minimum
    minimum="$(/usr/bin/otool -l "$1" | awk '
        $1 == "cmd" { command = $2 }
        (command == "LC_BUILD_VERSION" && $1 == "minos") ||
        (command == "LC_VERSION_MIN_MACOSX" && $1 == "version") { print $2; exit }
    ')" || return 1
    noxshell_version_valid "${minimum}" || {
        printf '无法读取 macOS 最低版本：%s\n' "$1" >&2
        return 1
    }
    printf '%s\n' "${minimum}"
}

noxshell_select_deployment_target() {
    local requested="$1" minimum=14.0 version
    shift
    for version in "$@"; do
        noxshell_version_valid "${version}" || return 1
        if noxshell_version_greater "${version}" "${minimum}"; then minimum="${version}"; fi
    done
    if [[ -n "${requested}" ]]; then
        noxshell_version_valid "${requested}" || {
            printf '最低系统版本格式错误：%s（示例：14.0）\n' "${requested}" >&2
            return 1
        }
        if noxshell_version_greater "${minimum}" "${requested}"; then
            printf '指定的 macOS %s 低于当前依赖要求 %s；请换用兼容依赖，或取消 NOXSHELL_MACOS_DEPLOYMENT_TARGET 以自动选择。\n' "${requested}" "${minimum}" >&2
            return 1
        fi
        minimum="${requested}"
    fi
    printf '%s\n' "${minimum}"
}

noxshell_qt_deployment_target() {
    local prefix="$1" requested="$2" component binary
    local minimums=()
    # These are the Qt frameworks linked by the application and its tests.
    for component in Core Gui Network Sql Svg Widgets Test; do
        binary="${prefix}/lib/Qt${component}.framework/Qt${component}"
        [[ -f "${binary}" ]] || {
            printf '缺少 Qt 框架：%s\n' "${binary}" >&2
            return 1
        }
        minimums+=("$(noxshell_binary_minimum "${binary}")") || return 1
    done
    noxshell_select_deployment_target "${requested}" "${minimums[@]}"
}

noxshell_check_bundle_minimum() {
    local app="$1" target="$2" binary minimum
    # Validate every deployed Mach-O, including transitive libraries/plugins,
    # rather than trusting only the main executable's Info.plist.
    while IFS= read -r -d '' binary; do
        [[ "$(/usr/bin/file -b "${binary}")" == *Mach-O* ]] || continue
        minimum="$(noxshell_binary_minimum "${binary}")" || return 1
        if noxshell_version_greater "${minimum}" "${target}"; then
            printf '依赖 %s 要求 macOS %s，高于目标 %s。请使用兼容依赖，或设置 NOXSHELL_MACOS_DEPLOYMENT_TARGET=%s 后重试。\n' "${binary}" "${minimum}" "${target}" "${minimum}" >&2
            return 1
        fi
    done < <(/usr/bin/find "${app}/Contents" -type f -print0)
}

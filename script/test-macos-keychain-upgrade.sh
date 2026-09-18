#!/usr/bin/env bash

set -Eeuo pipefail
[[ "$(uname -s)" == Darwin ]] || { printf '此验证仅适用于 macOS。\n' >&2; exit 1; }
[[ $# == 0 || ( $# == 1 && "$1" == --expect-adhoc-failure ) ]] || exit 1
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/macos-signing-config.sh"
noxshell_load_signing_config
if [[ "${NOXSHELL_SIGNING_MODE}" != developer-id && "${1:-}" != --expect-adhoc-failure ]]; then
    printf '跨版本密码保持验证需要 Developer ID；不把临时签名误报为通过。\n' >&2
    exit 1
fi
if [[ "${1:-}" == --expect-adhoc-failure && "${NOXSHELL_SIGNING_MODE}" != adhoc-test ]]; then exit 1; fi

umask 077
readonly TEST_DIR="$(mktemp -d /private/tmp/noxshell-keychain-upgrade.XXXXXX)"
readonly TEST_KEYCHAIN="${TEST_DIR}/credentials.keychain-db"
cleanup() {
    if [[ -f "${TEST_KEYCHAIN}" && -x "${TEST_DIR}/probe-A" ]]; then
        "${TEST_DIR}/probe-A" delete "${TEST_KEYCHAIN}" || {
            printf '临时测试钥匙串清理失败，保留以便检查：%s\n' "${TEST_DIR}" >&2
            return 1
        }
    fi
    # Remove ONLY the directory this invocation created, never a user keychain.
    case "${TEST_DIR}" in /private/tmp/noxshell-keychain-upgrade.*) rm -rf -- "${TEST_DIR}" ;; esac
}
trap cleanup EXIT
readonly IDENTIFIER='com.noxshell.test.keychain-upgrade'
for build in A B Untrusted; do
    xcrun clang++ -std=c++17 -Wno-deprecated-declarations \
        "-DNOXSHELL_UPGRADE_BUILD=\"${build}\"" "${SCRIPT_DIR}/../tests/MacKeychainUpgradeProbe.cpp" \
        -framework Security -framework CoreFoundation -o "${TEST_DIR}/probe-${build}"
    if [[ "${build}" == Untrusted ]]; then
        /usr/bin/codesign --force --sign - --timestamp=none --identifier "${IDENTIFIER}" "${TEST_DIR}/probe-${build}"
    else
        /usr/bin/codesign "${NOXSHELL_SIGN_ARGS[@]}" --identifier "${IDENTIFIER}" "${TEST_DIR}/probe-${build}"
        noxshell_verify_signing_identity "${TEST_DIR}/probe-${build}" "${IDENTIFIER}"
    fi
done
# A distinct binary is essential: restarting the same test executable does not
# reproduce an application upgrade. Do not log the synthetic secret itself.
hash_a="$(/usr/bin/codesign -d --verbose=4 "${TEST_DIR}/probe-A" 2>&1 | sed -n 's/^CDHash=//p')"
hash_b="$(/usr/bin/codesign -d --verbose=4 "${TEST_DIR}/probe-B" 2>&1 | sed -n 's/^CDHash=//p')"
[[ -n "${hash_a}" && -n "${hash_b}" && "${hash_a}" != "${hash_b}" ]] || { printf '测试构建必须有不同代码哈希。\n' >&2; exit 1; }
if [[ "${NOXSHELL_SIGNING_MODE}" == developer-id ]]; then
    requirement_a="$(/usr/bin/codesign -d -r- "${TEST_DIR}/probe-A" 2>&1 | sed -nE 's/^(# )?designated => //p')"
    requirement_b="$(/usr/bin/codesign -d -r- "${TEST_DIR}/probe-B" 2>&1 | sed -nE 's/^(# )?designated => //p')"
    [[ -n "${requirement_a}" && "${requirement_a}" == "${requirement_b}" ]] || { printf '两个构建的身份要求不一致。\n' >&2; exit 1; }
fi
"${TEST_DIR}/probe-A" create "${TEST_KEYCHAIN}"
"${TEST_DIR}/probe-A" write "${TEST_KEYCHAIN}"
"${TEST_DIR}/probe-A" read "${TEST_KEYCHAIN}"
if [[ "${NOXSHELL_SIGNING_MODE}" == developer-id ]]; then
    "${TEST_DIR}/probe-B" read "${TEST_KEYCHAIN}"
else
    "${TEST_DIR}/probe-B" read-denied "${TEST_KEYCHAIN}"
fi
"${TEST_DIR}/probe-Untrusted" read-denied "${TEST_KEYCHAIN}"
"${TEST_DIR}/probe-A" read "${TEST_KEYCHAIN}"
if [[ "${NOXSHELL_SIGNING_MODE}" == developer-id ]]; then
    printf '跨构建密码读取通过；非受信构建被拒绝；全程未申请系统授权。\n'
else
    printf '已复现临时签名升级后读取被拒绝；这不是正式修复验证通过。\n'
fi

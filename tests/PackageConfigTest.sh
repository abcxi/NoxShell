#!/usr/bin/env bash
# No actual builds, signing, credentials or network: policy + mocked Mach-O data.
set -Eeuo pipefail
readonly PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "${PROJECT_DIR}/script/package-config.sh"
source "${PROJECT_DIR}/script/macos-signing-config.sh"
unset NOXSHELL_CODESIGN_IDENTITY NOXSHELL_CODESIGN_TEAM_ID NOXSHELL_CODESIGN_KEYCHAIN
unset NOXSHELL_ALLOW_ADHOC_SIGNING

pass() { printf 'PASS: %s\n' "$1"; }
reject() {
    local label="$1"
    shift
    if "$@" >/dev/null 2>&1; then printf 'FAIL: %s\n' "${label}" >&2; exit 1; fi
    pass "${label}"
}
function /usr/bin/security { printf 'FAIL: unexpected certificate lookup\n' >&2; return 71; }
function /usr/bin/otool {
    [[ "$1" == -l ]] || return 72
    case "$2" in
        /fixture/modern)
            printf 'cmd LC_BUILD_VERSION\nplatform MACOS\nminos 14.0\nsdk 26.4\n' ;;
        /fixture/legacy)
            printf 'cmd LC_VERSION_MIN_MACOSX\nversion 10.15\nsdk 14.0\n' ;;
        /fixture/new-qt)
            printf 'cmd LC_BUILD_VERSION\nminos 26.0\nsdk 26.4\n' ;;
        /fixture/broken) return 73 ;;
        *) printf 'cmd LC_BUILD_VERSION\nsdk 26.4\n' ;;
    esac
}

noxshell_package_options
[[ "${NOXSHELL_PACKAGE_MODE}" == local ]]
noxshell_package_signing >/dev/null 2>&1
[[ "${NOXSHELL_SIGNING_MODE}" == adhoc-test && "${NOXSHELL_ALLOW_ADHOC_SIGNING}" == 1 ]]
pass 'plain command selects local test packaging without a certificate'

noxshell_package_options --release
reject 'explicit release rejects missing certificate despite inherited test flag' noxshell_package_signing
[[ "${NOXSHELL_ALLOW_ADHOC_SIGNING}" == 0 && -z "${NOXSHELL_SIGNING_MODE}" ]]
noxshell_package_options --local
NOXSHELL_CODESIGN_IDENTITY='-'
reject 'local packaging does not conceal invalid configured identity' noxshell_package_signing
unset NOXSHELL_CODESIGN_IDENTITY
NOXSHELL_CODESIGN_TEAM_ID='ABCDEFGHIJ'
reject 'local packaging does not conceal partial release configuration' noxshell_package_signing
unset NOXSHELL_CODESIGN_TEAM_ID
reject 'conflicting modes are rejected' noxshell_package_options --local --release
reject 'unknown options are rejected' noxshell_package_options --releas
noxshell_package_options --help
[[ "${NOXSHELL_PACKAGE_HELP}" == 1 ]]
pass 'help does not require a build'

[[ "$(noxshell_binary_minimum /fixture/modern)" == 14.0 ]]
[[ "$(noxshell_binary_minimum /fixture/legacy)" == 10.15 ]]
[[ "$(noxshell_binary_minimum /fixture/new-qt)" == 26.0 ]]
pass 'minimum OS is read from modern/legacy Mach-O commands, not the SDK'
reject 'unreadable Mach-O fails closed' noxshell_binary_minimum /fixture/broken
reject 'missing minimum OS fails closed' noxshell_binary_minimum /fixture/missing
[[ "$(noxshell_select_deployment_target '' 10.15 13.0)" == 14.0 ]]
[[ "$(noxshell_select_deployment_target '' 14.0 26.0 15.2)" == 26.0 ]]
[[ "$(noxshell_select_deployment_target '' 14.9 14.10.1)" == 14.10.1 ]]
[[ "$(noxshell_select_deployment_target 15.0 14.0)" == 15.0 ]]
pass 'deployment target uses numeric max, project floor and compatible explicit target'
reject 'explicit target below dependency minimum is rejected' noxshell_select_deployment_target 14.0 26.0
reject 'invalid target is rejected' noxshell_select_deployment_target banana 14.0
reject 'invalid dependency version is rejected' noxshell_select_deployment_target '' unknown
reject 'missing Qt component is rejected before building' noxshell_qt_deployment_target /fixture/missing-qt ''

function /usr/bin/file {
    [[ "$1" == -b ]] || return 74
    if [[ "$2" == /fixture/readme ]]; then printf 'ASCII text\n'; else printf 'Mach-O 64-bit dynamically linked shared library arm64\n'; fi
}
function /usr/bin/find {
    [[ "$*" == '/fixture/NoxShell.app/Contents -type f -print0' ]] || return 75
    printf '%s\0' /fixture/modern /fixture/readme "${BUNDLE_FIXTURE:-/fixture/new-qt}"
}
reject 'deployed transitive dependency above target is rejected' noxshell_check_bundle_minimum /fixture/NoxShell.app 14.0
noxshell_check_bundle_minimum /fixture/NoxShell.app 26.0
pass 'compatible deployed binaries pass and ordinary resources are skipped'
BUNDLE_FIXTURE=/fixture/broken
reject 'unreadable deployed Mach-O is rejected' noxshell_check_bundle_minimum /fixture/NoxShell.app 26.0

# Exercise the public entry point without starting a real build.
"${PROJECT_DIR}/script/package-release.sh" --help >/dev/null
reject 'entry point rejects unknown options before building' "${PROJECT_DIR}/script/package-release.sh" --typo

#!/usr/bin/env bash
# Pure configuration tests: shadow the absolute tool names in this test shell
# so no certificate/keychain lookup, signing, or authorization can occur.
set -Eeuo pipefail
readonly PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "${PROJECT_DIR}/script/macos-signing-config.sh"
readonly VALID_ID='ABCDEF0123ABCDEF0123ABCDEF0123ABCDEF0123'
function /usr/bin/security {
    [[ "$*" == 'find-identity -v -p codesigning' ]] || return 71
    printf '  1) %s "Developer ID Application: Fixture (ABCDEFGHIJ)"\n     1 valid identities found\n' "${VALID_ID}"
}
function /usr/bin/codesign {
    if [[ "$*" == '--verify --deep --strict /test/NoxShell.app' ]]; then return 0; fi
    [[ $# == 5 && "$1" == --verify && "$2" == --strict && "$3" == --test-requirement \
        && "$5" == /test/NoxShell.app ]] || return 72
    [[ "$4" == *'identifier "com.noxshell.ops"'* \
        && "$4" == *'anchor apple generic'* \
        && "$4" == *'certificate 1[field.1.2.840.113635.100.6.2.6] exists'* \
        && "$4" == *'certificate leaf[field.1.2.840.113635.100.6.1.13] exists'* \
        && "$4" == *'certificate leaf[subject.OU] = "ABCDEFGHIJ"'* ]] || return 73
    [[ "${REJECT_TEST_SIGNATURE:-0}" != 1 ]]
}

reset_config() {
    unset NOXSHELL_CODESIGN_IDENTITY NOXSHELL_CODESIGN_TEAM_ID NOXSHELL_CODESIGN_KEYCHAIN
    unset NOXSHELL_ALLOW_ADHOC_SIGNING REJECT_TEST_SIGNATURE
}
expect_rejected() {
    if noxshell_load_signing_config >/dev/null 2>&1; then
        printf 'FAIL: %s unexpectedly accepted\n' "$1" >&2
        exit 1
    fi
    printf 'PASS: %s\n' "$1"
}
reset_config
expect_rejected 'missing identity blocks release'
NOXSHELL_ALLOW_ADHOC_SIGNING=1
noxshell_load_signing_config >/dev/null 2>&1
[[ "${NOXSHELL_SIGNING_MODE}" == adhoc-test && "${NOXSHELL_SIGN_ARGS[*]}" == '--force --sign - --timestamp=none' ]]
printf 'PASS: explicitly opted-in test signing\n'
NOXSHELL_CODESIGN_IDENTITY='-'
expect_rejected 'invalid configured identity never falls back'
NOXSHELL_CODESIGN_IDENTITY="${VALID_ID}"
expect_rejected 'missing team pin'
NOXSHELL_CODESIGN_TEAM_ID='bad"team'
expect_rejected 'requirement injection rejected'
NOXSHELL_CODESIGN_TEAM_ID='ABCDEFGHIJ'
NOXSHELL_CODESIGN_IDENTITY='0000000000000000000000000000000000000000'
expect_rejected 'unavailable identity never falls back'
NOXSHELL_CODESIGN_IDENTITY='abcdef0123abcdef0123abcdef0123abcdef0123'
noxshell_load_signing_config
[[ "${NOXSHELL_SIGNING_MODE}" == developer-id \
    && "${NOXSHELL_SIGN_ARGS[*]}" == "--force --sign ${VALID_ID} --timestamp --options runtime" ]]
noxshell_verify_signing_identity /test/NoxShell.app com.noxshell.ops
printf 'PASS: exact certificate, fixed team, Apple chain, identifier, hardened runtime\n'
REJECT_TEST_SIGNATURE=1
if noxshell_verify_signing_identity /test/NoxShell.app com.noxshell.ops >/dev/null 2>&1; then
    printf 'FAIL: self-signed/wrong-team signature accepted\n' >&2
    exit 1
fi
printf 'PASS: failed release signature rejected\n'
NOXSHELL_CODESIGN_KEYCHAIN='relative/signing.keychain-db'
expect_rejected 'relative keychain path'
reset_config
NOXSHELL_ALLOW_ADHOC_SIGNING=1
NOXSHELL_CODESIGN_TEAM_ID='ABCDEFGHIJ'
expect_rejected 'partial release config cannot produce ad-hoc artifact'
if noxshell_verify_signing_identity /test/NoxShell.app com.noxshell.ops; then
    printf 'FAIL: uninitialized verification mode accepted\n' >&2
    exit 1
fi
printf 'PASS: failed configuration cannot bypass identity verification\n'

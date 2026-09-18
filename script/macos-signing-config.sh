#!/usr/bin/env bash

# Shared by packaging, signing and the isolated cross-build Keychain test.
# Never import certificates, change trust/ACLs, or silently fall back to ad-hoc.
noxshell_load_signing_config() {
    NOXSHELL_SIGN_ARGS=()
    NOXSHELL_SIGNING_REQUIREMENT=''
    NOXSHELL_SIGNING_MODE=''
    local identity="${NOXSHELL_CODESIGN_IDENTITY:-}"
    local team="${NOXSHELL_CODESIGN_TEAM_ID:-}"
    local keychain="${NOXSHELL_CODESIGN_KEYCHAIN:-}"
    if [[ -z "${identity}" ]]; then
        if [[ -n "${team}" || -n "${keychain}" ]]; then
            printf '签名配置不完整：必须设置 NOXSHELL_CODESIGN_IDENTITY。\n' >&2
            return 1
        fi
        if [[ "${NOXSHELL_ALLOW_ADHOC_SIGNING:-0}" != 1 ]]; then
            printf '%s\n' \
                '停止打包：未配置固定的 Developer ID Application 发布身份。' \
                '临时签名/本地自签不能保证覆盖升级后继续读取 Keychain 密码。' \
                '请设置证书 SHA-1（NOXSHELL_CODESIGN_IDENTITY）和 Apple Team ID（NOXSHELL_CODESIGN_TEAM_ID）。' \
                '仅限开发测试时可显式设置 NOXSHELL_ALLOW_ADHOC_SIGNING=1；不能把该产物作为密码保持修复版。' >&2
            return 1
        fi
        NOXSHELL_SIGNING_MODE='adhoc-test'
        NOXSHELL_SIGN_ARGS=(--force --sign - --timestamp=none)
        printf '警告：仅生成临时签名测试包；覆盖升级可能需要重新输入 SSH 密码。\n' >&2
        return 0
    fi
    if [[ ! "${identity}" =~ ^[[:xdigit:]]{40}$ || ! "${team}" =~ ^[A-Z0-9]{10}$ ]]; then
        printf '发布签名需要完整的 40 位证书 SHA-1 和 10 位 Apple Team ID；不接受名称匹配或临时签名。\n' >&2
        return 1
    fi
    local keychain_args=()
    if [[ -n "${keychain}" ]]; then
        if [[ "${keychain}" != /* || ! -f "${keychain}" ]]; then
            printf 'NOXSHELL_CODESIGN_KEYCHAIN 必须是已存在的钥匙串文件绝对路径。\n' >&2
            return 1
        fi
        keychain_args=("${keychain}")
    fi
    local identities
    identities="$(/usr/bin/security find-identity -v -p codesigning ${keychain_args[@]+"${keychain_args[@]}"})" || return 1
    identity="$(printf '%s' "${identity}" | tr '[:lower:]' '[:upper:]')"
    # Match the complete fingerprint, not a display name or partial identity.
    if ! printf '%s\n' "${identities}" | awk -v id="${identity}" '$2 == id { found = 1 } END { exit !found }'; then
        printf '未找到配置的有效发布证书；不会回退到临时签名。请先在钥匙串中安装该证书及私钥。\n' >&2
        return 1
    fi
    NOXSHELL_SIGNING_MODE='developer-id'
    NOXSHELL_SIGN_ARGS=(--force --sign "${identity}" --timestamp --options runtime)
    if [[ -n "${keychain}" ]]; then NOXSHELL_SIGN_ARGS+=(--keychain "${keychain}"); fi
    # Apple-issued Developer ID chain + pinned team, not a self-signed certificate
    # with a familiar name/OU. The identifier is added by the caller.
    NOXSHELL_SIGNING_REQUIREMENT="anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists and certificate leaf[subject.OU] = \"${team}\""
}

noxshell_verify_signing_identity() {
    local target="$1" identifier="$2"
    case "${NOXSHELL_SIGNING_MODE:-}" in developer-id|adhoc-test) ;; *) return 1 ;; esac
    /usr/bin/codesign --verify --deep --strict "${target}" || return 1
    if [[ "${NOXSHELL_SIGNING_MODE}" == 'developer-id' ]]; then
        /usr/bin/codesign --verify --strict --test-requirement \
            "identifier \"${identifier}\" and (${NOXSHELL_SIGNING_REQUIREMENT})" "${target}" || {
            printf '签名身份校验失败：必须是指定 Apple 团队的 Developer ID Application，且应用标识保持不变。\n' >&2
            return 1
        }
    fi
}

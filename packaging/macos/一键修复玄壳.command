#!/bin/bash

set -u

readonly APP_PATH="${NOXSHELL_REPAIR_APP_PATH:-/Applications/玄壳.app}"
readonly NO_DIALOG="${NOXSHELL_REPAIR_NO_DIALOG:-0}"

show_message() {
    local title="$1"
    local message="$2"
    if [[ "${NO_DIALOG}" == "1" ]] || [[ ! -x /usr/bin/osascript ]]; then
        printf '%s：%s\n' "${title}" "${message}"
        return
    fi
    /usr/bin/osascript - "${title}" "${message}" <<'APPLESCRIPT'
on run argv
    display dialog (item 2 of argv) buttons {"知道了"} default button 1 with title (item 1 of argv)
end run
APPLESCRIPT
}

quarantine_remains() {
    /usr/bin/xattr -lr "${APP_PATH}" 2>/dev/null | /usr/bin/grep -q 'com\.apple\.quarantine'
}

if [[ ! -d "${APP_PATH}" ]]; then
    show_message "未找到玄壳" "请先把“玄壳.app”拖入 Applications 文件夹，再运行一键修复。"
    exit 1
fi

/usr/bin/xattr -dr com.apple.quarantine "${APP_PATH}" 2>/dev/null || true

# A normal Finder installation is owned by the current user. If permissions are
# stricter, request administrator authorization only for this fixed app path.
if quarantine_remains && [[ "${APP_PATH}" == "/Applications/玄壳.app" ]] \
    && [[ "${NO_DIALOG}" != "1" ]] && [[ -x /usr/bin/osascript ]]; then
    /usr/bin/osascript <<'APPLESCRIPT' >/dev/null 2>&1 || true
do shell script "/usr/bin/xattr -dr com.apple.quarantine '/Applications/玄壳.app'" with administrator privileges
APPLESCRIPT
fi

if quarantine_remains; then
    show_message "修复失败" "未能清除玄壳的隔离属性，请确认当前账户有权限修改 Applications 中的应用。"
    exit 1
fi

show_message "修复完成" "玄壳的隔离属性已清除，现在可以重新打开应用。"

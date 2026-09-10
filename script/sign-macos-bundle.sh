#!/usr/bin/env bash

set -Eeuo pipefail

# Sign only our staged application, after all install_name_tool/strip/deploy
# operations. Ad-hoc integrity signing does not replace Developer ID/notarization.
[[ $# == 1 && "$1" == *.app && -d "$1" && ! -L "$1" ]] || {
    printf '用法: bash script/sign-macos-bundle.sh /path/to/NoxShell.app\n' >&2
    exit 1
}
readonly APP_PATH="$(cd -- "$1" && pwd -P)"
[[ "$(/usr/bin/plutil -extract CFBundleIdentifier raw "${APP_PATH}/Contents/Info.plist")" == 'com.noxshell.ops' ]] || {
    printf '拒绝签名：目标不是 NoxShell 应用。\n' >&2
    exit 1
}
[[ -f "${APP_PATH}/Contents/MacOS/NoxShell" ]] || exit 1

# Sign individual nested components from the inside out; --deep is used for
# verification only, not as a substitute for signing each deployed library.
while IFS= read -r -d '' library; do
    /usr/bin/codesign --force --sign - --timestamp=none "${library}"
done < <(/usr/bin/find "${APP_PATH}/Contents" -type f -name '*.dylib' -print0)
while IFS= read -r -d '' framework; do
    /usr/bin/codesign --force --sign - --timestamp=none "${framework}"
done < <(/usr/bin/find "${APP_PATH}/Contents" -depth -type d -name '*.framework' -print0)
/usr/bin/codesign --force --sign - --timestamp=none "${APP_PATH}"
/usr/bin/codesign --verify --deep --strict "${APP_PATH}"
printf '应用与全部内嵌组件签名校验通过：%s\n' "${APP_PATH}"

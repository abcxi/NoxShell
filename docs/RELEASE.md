# 发布检查清单

## 自动检查

运行：

```bash
./script/package-release.sh
```

脚本执行独立 Release 构建、全量测试、安装部署和压缩包生成。产物写入 `output/`。

### macOS 升级后保持已保存密码

正式打包必须使用同一 Apple 团队的 **Developer ID Application** 签名身份，
保持 `com.noxshell.ops` 标识不变。将已有证书和私钥安装到构建机的钥匙串后，配置：

```bash
# 查看有效代码签名身份；只显示公开的证书标识，不显示私钥。
security find-identity -v -p codesigning

export NOXSHELL_CODESIGN_IDENTITY='证书的完整40位SHA-1指纹'
export NOXSHELL_CODESIGN_TEAM_ID='10位AppleTeamID'
# 如证书位于专用钥匙串，可指定该文件的绝对路径：
# export NOXSHELL_CODESIGN_KEYCHAIN='/absolute/path/signing.keychain-db'
./script/package-release.sh
```

打包会先检查有效证书，再运行隔离的跨构建 Keychain 测试：A 构建保存虚构密码，
A 重启与不同代码哈希的 B 构建均须读取成功，不受信构建必须读取失败。
测试只操作新建的临时钥匙串，恢复原搜索列表、不更换默认钥匙串、不访问真实
SSH 凭据、不改变其 ACL，所有密码操作禁用系统授权界面。测试结束删除测试数据。
签名启用 hardened runtime 和安全时间戳；最后校验 Apple Developer ID 证书链、
指定 Team ID、应用标识及代码完整性。配置错误或身份缺失均停止，不回退。

临时签名把访问身份绑定到代码哈希，重新构建后可能无法静默读取旧条目。
**固定本地自签证书也不能保证解决此问题**：macOS Keychain 还检查签名分区，
非 Apple 开发者身份仍可能按代码哈希隔离，而非仅比较证书或应用名称。
依据：[Apple securityd 的 partitionIdForProcess 实现](https://github.com/apple-oss-distributions/Security/blob/main/securityd/src/clientid.cpp)。

旧临时签名创建的密码不会因新包改为正式签名就自动获得新访问权限。
在不申请系统授权、不修改旧权限的策略下，切换到正式签名后可能需要最后重输一次
SSH 密码并勾选记住；之后只有实际通过跨构建测试，才能声明覆盖升级保持密码。
钥匙串被锁定、条目被删除、团队变化等情况仍可能需要重新输入。

确需构建未签名开发测试包时必须显式允许，不可当作上述问题的修复版分发：

```bash
NOXSHELL_ALLOW_ADHOC_SIGNING=1 ./script/package-release.sh
# 仅复现旧问题；“预期拒绝”不等于正式修复通过：
NOXSHELL_ALLOW_ADHOC_SIGNING=1 bash script/test-macos-keychain-upgrade.sh --expect-adhoc-failure
```

这些步骤不创建证书、不上传私钥、不改变系统信任，也不完成 Apple 公证。
私钥、证书口令和导出的 p12 不应进入源码仓库或发布包。

macOS 必须在 `macdeployqt` 完成依赖复制及路径改写后，为内嵌 dylib、
framework 和应用包从内到外重新签名。`codesign --verify --deep --strict`
失败会中止打包，不能当作“未配置 Developer ID”的普通提示放行。
打包脚本还会启动最终部署的应用执行 `--startup-smoke-test`，验证原生 Cocoa
窗口与 SQLite 插件；测试使用临时数据库和临时设置，不读取真实服务器配置、
系统凭据库或写入正常运行日志，最多等待 20 秒。

可对部署后的包单独执行：

```bash
bash script/sign-macos-bundle.sh /path/to/NoxShell.app
bash script/verify-macos-bundle.sh /path/to/NoxShell.app
```

签名之后不能继续使用 `install_name_tool`、`strip` 或修改包内资源；若有修改，
必须重新签名、验证和测试启动。`xattr` 清理隔离属性不能修复无效代码签名。

## GitHub Actions 自动发布

`.github/workflows/release.yml` 提供两种触发方式：

- 在 GitHub Actions 页面手动运行：只构建并保存各平台 Artifacts。
- 推送与项目版本一致的标签（例如 `v0.2.51`）：构建 Windows 与 macOS 安装包，并自动创建或更新 GitHub Release。

自动发布包含 macOS arm64/x86_64 DMG、Windows x64 NSIS EXE/便携 ZIP 和统一的 SHA-256 校验文件。Windows 使用 `vcpkg` 的 `x64-windows-static-md` triplet 静态链接 libssh2/zlib，避免安装后缺少非 Qt DLL；Qt 运行库和插件由 CMake 的 Qt 部署脚本写入安装包。

macOS 非标签构建可显式生成临时签名测试产物；标签发布不允许临时签名。
当前工作流不会自动导入证书：正式发布前还需由维护者安全配置 CI 证书导入、
`NOXSHELL_CODESIGN_IDENTITY` / `NOXSHELL_CODESIGN_TEAM_ID` 仓库变量和 Apple 公证。
未配置时 macOS 标签任务会停止，避免再次发布升级后密码失效的临时签名版本。

macOS DMG 同时包含“一键修复玄壳.command”。该入口仅在未签名或未公证的测试包被 Gatekeeper 隔离时使用，目标固定为 `/Applications/玄壳.app`；它不能替代正式发布所需的 Developer ID 签名与 Apple 公证。

Windows 本地打包要求：CMake 3.25+、Qt 6.5+、vcpkg、NSIS 3.03+。配置时指定 vcpkg toolchain，然后运行：

```powershell
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64 `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-release --config Release --parallel
ctest --test-dir build-release -C Release --output-on-failure
cpack --config build-release/CPackConfig.cmake -C Release -B output
```

未配置发布证书时，macOS 正式打包会停止；显式允许的 ad-hoc 测试包只保证代码完整性，不能保证升级后静默读取旧密码，也不会自动解除 Gatekeeper 限制。Windows 安装包仍未进行 Authenticode 签名。正式公开分发前仍需完成相应签名/公证配置，证书及口令仅由维护者配置到安全的构建密钥存储中。

## 发布前人工检查

- 用密码、私钥和 SSH Agent 各连接一台测试服务器。
- 核对首次连接指纹确认与已知指纹变更阻断。
- 打开、复制、关闭多个终端标签，重启应用验证标签恢复。
- 验证 1/15/60 分钟监控曲线、阈值保存和告警记录。
- 上传与下载大文件，测试取消、重试、断点续传和限速。
- 检查日志不包含密码、口令、令牌或 Authorization 值。
- macOS 公开分发前使用 Developer ID 签名并完成 Apple 公证。
- Windows 检查 NSIS 安装、覆盖升级、开始菜单/桌面快捷方式和卸载，再对安装程序及主程序完成 Authenticode 签名。

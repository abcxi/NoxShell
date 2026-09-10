# 发布检查清单

## 自动检查

运行：

```bash
./script/package-release.sh
```

脚本执行独立 Release 构建、全量测试、安装部署和压缩包生成。产物写入 `output/`。

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

未配置发布证书时，macOS 应用使用 ad-hoc 临时签名保证代码完整性；这不是 Developer ID 身份认证或 Apple 公证，也不会自动解除 Gatekeeper 限制。Windows 安装包仍未进行 Authenticode 签名。正式公开分发前仍需增加相应签名/公证步骤，并将证书及口令保存在 GitHub Secrets 中。

## 发布前人工检查

- 用密码、私钥和 SSH Agent 各连接一台测试服务器。
- 核对首次连接指纹确认与已知指纹变更阻断。
- 打开、复制、关闭多个终端标签，重启应用验证标签恢复。
- 验证 1/15/60 分钟监控曲线、阈值保存和告警记录。
- 上传与下载大文件，测试取消、重试、断点续传和限速。
- 检查日志不包含密码、口令、令牌或 Authorization 值。
- macOS 公开分发前使用 Developer ID 签名并完成 Apple 公证。
- Windows 检查 NSIS 安装、覆盖升级、开始菜单/桌面快捷方式和卸载，再对安装程序及主程序完成 Authenticode 签名。

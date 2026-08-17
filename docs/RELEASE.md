# 发布检查清单

## 自动检查

运行：

```bash
./script/package-release.sh
```

脚本执行独立 Release 构建、全量测试、安装部署和压缩包生成。产物写入 `output/`。

## GitHub Actions 自动发布

`.github/workflows/release.yml` 提供两种触发方式：

- 在 GitHub Actions 页面手动运行：只构建并保存各平台 Artifacts。
- 推送与项目版本一致的标签（例如 `v0.2.48`）：构建 Windows 与 macOS 安装包，并自动创建或更新 GitHub Release。

自动发布包含 macOS arm64/x86_64 DMG、Windows x64 NSIS EXE/便携 ZIP 和统一的 SHA-256 校验文件。Windows 使用 `vcpkg` 的 `x64-windows-static-md` triplet 静态链接 libssh2/zlib，避免安装后缺少非 Qt DLL；Qt 运行库和插件由 CMake 的 Qt 部署脚本写入安装包。

Windows 本地打包要求：CMake 3.25+、Qt 6.5+、vcpkg、NSIS 3.03+。配置时指定 vcpkg toolchain，然后运行：

```powershell
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64 `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-release --config Release --parallel
ctest --test-dir build-release -C Release --output-on-failure
cpack --config build-release/CPackConfig.cmake -C Release -B output
```

未配置签名证书时，工作流生成的是未签名安装包。正式公开分发前仍需增加 macOS Developer ID 签名/公证和 Windows Authenticode 签名步骤，并将证书及口令保存在 GitHub Secrets 中。

## 发布前人工检查

- 用密码、私钥和 SSH Agent 各连接一台测试服务器。
- 核对首次连接指纹确认与已知指纹变更阻断。
- 打开、复制、关闭多个终端标签，重启应用验证标签恢复。
- 验证 1/15/60 分钟监控曲线、阈值保存和告警记录。
- 上传与下载大文件，测试取消、重试、断点续传和限速。
- 检查日志不包含密码、口令、令牌或 Authorization 值。
- macOS 公开分发前使用 Developer ID 签名并完成 Apple 公证。
- Windows 检查 NSIS 安装、覆盖升级、开始菜单/桌面快捷方式和卸载，再对安装程序及主程序完成 Authenticode 签名。

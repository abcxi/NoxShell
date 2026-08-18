# 玄壳（NoxShell）

面向 SSH 远程管理的 C++20/Qt 6 桌面应用。当前版本为 `v0.2.50`，覆盖多会话终端、Linux 实时监控、告警和 SFTP 文件管理。

## 下载

[前往 GitHub Releases 下载最新版本](https://github.com/abcxi/NoxShell/releases/latest)

当前版本：`v0.2.50`

| 系统 | 安装包 | 适用设备 |
| --- | --- | --- |
| macOS | [下载 Apple Silicon DMG](https://github.com/abcxi/NoxShell/releases/download/v0.2.50/%E7%8E%84%E5%A3%B3-v0.2.50-macOS-arm64.dmg) | M1、M2、M3、M4 等 Apple 芯片 Mac |
| macOS | [下载 Intel DMG](https://github.com/abcxi/NoxShell/releases/download/v0.2.50/%E7%8E%84%E5%A3%B3-v0.2.50-macOS-x86_64.dmg) | Intel 芯片 Mac |
| Windows | [下载安装版 EXE](https://github.com/abcxi/NoxShell/releases/download/v0.2.50/NoxShell-v0.2.50-Windows-x64.exe) | 64 位 Windows，推荐使用 |
| Windows | [下载便携版 ZIP](https://github.com/abcxi/NoxShell/releases/download/v0.2.50/NoxShell-v0.2.50-Windows-x64.zip) | 64 位 Windows，解压即用 |
| 校验文件 | [下载 SHA256SUMS.txt](https://github.com/abcxi/NoxShell/releases/download/v0.2.50/SHA256SUMS.txt) | 用于验证下载文件完整性 |

> 安装包会在推送对应版本标签并且 GitHub Actions 构建成功后出现。私有仓库的下载链接需要先登录具有仓库访问权限的 GitHub 账号。

## 当前能力

- 桌面应用统一使用去除外围黑边的黑色玻璃质感与金色终端符号图标；透明圆角主体铺满有效画布，macOS 发布包内置 16–1024px Retina `.icns`，Dock、Finder、窗口与应用切换器保持一致。
- 单一主机导航栏采用“名称 / IP”单行紧凑列表，支持名称/IP/分组筛选；不展示也不承担会话在线状态，单击不切换终端标签，双击连接后自动收起主机栏。
- 移除占用垂直空间的自定义顶栏和 Qt 工具栏；macOS 使用原生标题栏附件，将两个独立图标直接放在红黄绿系统按钮右侧，可分别收起或恢复主机列表和实时监控栏，内容区紧贴系统标题栏。
- 主机右键菜单支持连接、编辑、复制主机配置、复制连接地址和删除。
- 真实 SSH 主机通过独立 exec channel 秒级采集 CPU/内核态、内存、1/5/15 分钟负载和根磁盘占用；演示主机提供可预测的离线数据。
- 监控历史在 SQLite 中保留 7 天，界面显示最近 1、15、60 分钟；每台服务器可独立设置 CPU、内存、负载与磁盘阈值，并保存告警事件。
- SSH 终端使用 VT/ANSI 网格状态机渲染，支持常见颜色、光标控制、滚动区、备用屏幕、功能键、Ctrl 组合键、中文输入和括号粘贴；PTY 行列随窗口尺寸同步。
- 终端保留最多 5000 行滚屏历史，支持滚轮浏览、鼠标拖选和双击选词；右键菜单提供复制、粘贴和全选，`Cmd+C/Cmd+V`、`Ctrl+C/Ctrl+V` 均可直接操作，有选区时 `Cmd/Ctrl+C` 智能复制、无选区时向远端发送中断信号以退出 `top` 等前台命令，并兼容 `Ctrl+Shift+C/Ctrl+Shift+V`。
- 终端支持 X10/SGR 鼠标报告，按住 Shift 可临时进行本地选择；右键始终保留给本地复制/粘贴菜单，不会误发给远端程序。
- 应用启动时不恢复终端标签、也不自动连接；没有会话时隐藏标签与终端内容，改为展示按成功登录时间倒序排列的最近登录记录；同一主机只保留最新一次成功登录，双击记录可重新连接。
- 多终端标签拥有独立 SSH 连接，使用灰色空心点、蓝色进度环和绿色实心点区分未连接、连接中和连接成功；标签右键支持连接、断开、复制会话及批量关闭。
- 已建立的终端是不可变的连接快照；在线时修改主机名、IP、端口或凭据不会断开、清空或重连旧会话，下一次显式连接会使用新配置创建新标签。
- 终端标签、左侧主机选中项、监控栏和 SFTP 文件区双向联动；监控与文件管理直接复用当前标签的 SSH 会话，切换时不重新连接。
- SFTP 文件区采用双栏布局：左侧目录树按展开节点逐层加载，右侧显示当前目录明细；支持路径输入、刷新、返回上级、历史返回和双击进入目录。
- 右侧文件列表支持 Shift/Cmd 多选和右键批量下载；可从 Finder 拖入一个或多个本地文件上传，拖到远程目录行时直接上传到该目录。
- 双击普通远端文件会打开独立多标签编辑窗口；同一服务器的文件复用一个窗口，标签显示“服务器名称 · 文件名”，未保存时显示红点。
- 远端编辑器采用无按钮的键盘工作流，支持 `Cmd/Ctrl+S` 保存、`Cmd/Ctrl+Z` 撤销、`Ctrl+/` 切换 Shell 风格 `#` 注释和 `Cmd/Ctrl+W` 关闭当前标签；文本区显示行号与当前行高亮。
- 编辑器支持按需展开的查找/替换栏：`Cmd/Ctrl+F` 查找后可直接点击“替换”展开第二行，macOS 也可用 `Cmd+Option+F`（其他平台可用 `Ctrl+H`）；Enter/Shift+Enter 前后循环匹配，支持区分大小写、单个替换、全部替换和匹配计数，Esc 收起面板。
- 关闭带未保存内容的文件标签或编辑窗口时会确认保存；二进制文件禁止文本编辑，单文件上限 4 MiB，保存通过远端临时文件原子替换并保留原权限。
- 终端执行 `cd` 后下方目录会同步绝对路径、相对路径、`..` 与 `~`；终端与文件面板之间可拖拽调整高度。
- SFTP 文件操作支持上传、下载、新建文件、新建目录、重命名和删除；上传通过 Finder 拖拽完成，下载及新建文件/目录统一从右键或“⋯”菜单进入，已存在的同名目标不会被覆盖。
- 文件管理采用单行工具栏，返回、上级、路径输入和刷新直接位于标题栏，不再占用第二行；上传/下载进入持久化传输队列，同一 SSH 会话串行执行，队列图标以弹出面板集中展示任务、限速、取消和失败重试；支持异常退出恢复及 `.noxshell.part` 断点续传。
- 当前主机采用三块同时可见的运维工作区：左侧纵向监控，右上 SSH 终端，右下文件管理；水平和垂直分隔条均可拖动。
- 终端区只保留会话标签和清屏按钮，复制与批量关闭统一从标签右键菜单操作；清屏等同于 `Ctrl+L`，清除历史后保留远端 Shell 当前提示符。连接期间显示居中 Loading 提示板，成功或失败后自动收起。
- 监控栏主机信息精简为 `IP:端口`，支持一键复制 IP；在线/连接中/离线由实际 SSH 会话状态驱动。
- 新增/编辑主机窗口内置“连接测试”，会直接验证尚未保存的地址、端口和凭据；测试不会占用或替换当前终端会话。
- 已知主机指纹严格绑定 `IP/域名:端口`；编辑时改变 IP 或端口会清除窗口中的旧指纹，并在新地址首次连接时独立确认。
- 密码框支持显示/隐藏当前输入，并明确标识测试使用“当前输入”还是 Keychain 已保存密码；旧密码认证失败时会提示重新输入或切换私钥认证。
- 密码与私钥口令输入固定为英文半角；`。`、`，` 等中文/全角标点自动转换为 `.`、`,` 等半角字符，其他非英文字符会被阻止并提示核对。
- 工作区标题栏不放置刷新、测试、编辑和删除等高风险快捷按钮；主机操作统一从左侧右键菜单进入。

真实 SSH 基础层已接入 libssh2，支持 TCP/SSH 握手、SHA-256 主机指纹确认、密码（自动协商 `keyboard-interactive`/`password`）、私钥/SSH Agent 认证和交互式 PTY。每个会话使用独立线程、socket 与 libssh2 session，多开连接不会互相覆盖凭据；指标采集使用同一已认证 SSH 会话中的独立 channel，不会把采集命令写入用户终端。内置示例主机默认保持演示模式，避免误连不可达地址。

Linux 指标来源为 `/proc/stat`、`/proc/meminfo`、`/proc/loadavg`、`getconf _NPROCESSORS_ONLN` 和 `df -Pk`。CPU/内核态比例基于相邻样本差分计算，因此真实主机的第一个样本用于建立基线，从第二个样本开始展示 CPU 百分比。

服务器元数据、监控历史、告警、传输状态和 `known_hosts` 信任记录保存在 Qt SQLite 数据库中。密码与私钥口令分别通过 macOS Keychain、Windows Credential Manager 或 Linux Secret Service 保存，SQLite 仅保存凭据引用。数据库默认位于系统应用数据目录的 `noxshell-ops.sqlite3`。

系统凭据统一使用 `com.noxshell.ops.ssh` 服务名，应用数据、主机指纹和服务器配置均写入 NoxShell 专属数据目录。

运行日志保存在系统应用数据目录的 `logs/noxshell-ops.log`，单文件达到 5 MiB 后轮换并保留 3 份历史；常见密码、口令、令牌和 Authorization 字段会脱敏。

## 构建

要求：CMake 3.25+、C++20 编译器、Qt 6.5+（Core、Gui、Network、Sql、Svg、Widgets）、libssh2 1.11+。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

macOS 运行：

```bash
open build/NoxShell.app
```

日常界面开发推荐使用增量编译启动脚本：

```bash
./script/qt-ui-start.sh
./script/qt-ui-start.sh --test
./script/qt-ui-start.sh --build-only
```

脚本会自动定位 Qt，复用 `build` 中的 CMake 缓存并只编译发生变化的文件；使用 `--clean` 可重新生成构建目录。

生成 Release 包：

```bash
./script/package-release.sh
```

macOS 默认生成单文件、LZMA 压缩的 `.dmg`，挂载后把“玄壳”拖入 Applications 即可安装；最低部署版本默认为 macOS 14.0。若还需要备用 ZIP，可执行 `NOXSHELL_CREATE_ZIP=1 ./script/package-release.sh`。产物写入 `output/`。

Windows 使用 CPack + NSIS 生成带开始菜单、桌面快捷方式和卸载入口的 `.exe` 安装程序，同时保留免安装 `.zip`。Windows 的 libssh2 由 `vcpkg.json` 管理并静态链接，Qt DLL 与平台插件由 Qt 部署脚本自动收集。

仓库内的 `.github/workflows/release.yml` 支持手动运行，也会在推送 `v*` 标签时自动构建并测试以下产物：

- macOS Apple Silicon `.dmg`
- macOS Intel `.dmg`
- Windows x64 NSIS `.exe` 与便携 `.zip`

标签必须与 `CMakeLists.txt` 中的项目版本一致，例如当前版本使用 `v0.2.50`。标签构建完成后，工作流会创建或更新同名 GitHub Release，并附加所有安装包及 `SHA256SUMS.txt`。

本地生成的 macOS 包采用临时签名，未使用 Developer ID 签名或 Apple 公证，仅适合内测；公开分发仍需正式签名与公证。使用当前 Homebrew Qt 构建时应在目标最低版本的 macOS 机器上做一次实际启动测试，正式兼容构建建议使用 Qt Online Installer 提供的官方 Qt。

## 目录

```text
src/core    主机模型、指标解析、持久化与 SSH 会话
src/ui      Qt Widgets 界面组件
output      产品文档和效果图
docs        发布与维护文档
```

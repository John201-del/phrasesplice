# PhraseSplice 使用指南

## 基础安装与第一次导出

使用 Windows 11 x64，把仓库放在可写的英文目录。双击根目录的 `Start-PhraseSplice.cmd`。第一次需要联网下载；后续启动会校验应用文件并复用运行目录。

请保留整个运行包。编辑器需要其中的 Qt、KDE、MLT、解码库和资源文件。安装失败会显示具体错误，不会自动忽略校验或转用不明镜像。

导入一个短视频，先在原生时间线上预览，然后导出 MP4。普通手工剪辑不需要连接 Codex。自动安装脚本仅面向已发布的 Windows 版本；修改源码后要先按 [BUILD.md](BUILD.md) 构建和打包自己的组件。

在软件中选择「帮助 → PhraseSplice 帮助手册」或按 F1，即可离线查看使用说明。

## 连接语言助手

点击 **连接 AI**，选择 Codex、兼容云端 API、本地 Ollama 或 WorkBuddy MCP，详见 [AI 接入指南](AI_PROVIDERS.md)。以下为 Codex 路线：先自行安装并登录 Codex。登录与额度由你的账号提供，不包含在本项目中。官方说明见 [Codex authentication](https://developers.openai.com/codex/auth)。

程序会查找 Windows Codex 桌面应用的安装目录。若你的安装方式不同，可在同一个 PowerShell 窗口指定真实可执行文件，再启动：

```powershell
$env:LOCALCUT_CODEX_EXECUTABLE = 'E:\Tools\Codex\codex.exe'
.\Start-PhraseSplice.cmd
```

这里的路径只是示例，必须改成你已安装、已登录的实际 `codex.exe`。无需把登录文件或 Key 复制进仓库。

助手需要支持 app-server 的 Codex 版本。遇到协议、网络、登录或额度错误时，先看提示并检查工程当前状态，再重新连接；程序不会把中断前的一批修改自动重放。手工时间线仍可使用。

## 可选本地 AI

这些扩展不会随基础安装自动下载。模型占用数 GB，CPU 运行可能明显慢于播放速度。先保证基础剪辑和导出可用，再安装需要的扩展。

本地修图、抠像、人声分离：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup-local-ai.ps1
```

照片说话、立体卡通跟脸（先完成上一步，并安装 [Git for Windows](https://git-scm.com/download/win)，安装器需获取固定版本的模型源代码）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup-local-avatar.ps1
```

安装器从原发布者下载依赖和模型，并在已提供校验值的模型上验证 SHA-256。失败时保留可续用的下载缓存，重新执行会复用完整文件。当前可选安装流程仍有大型网络依赖，尚不属于每次基础运行包验收的范围。不要把“基础剪辑验收通过”理解为所有模型均已在你的电脑上验证。

本地 Whisper 字幕是进阶安装项：助手预期数据根内有 `speech-venv/Scripts/python.exe` 和 `cache/whisper/small.pt`。本预览版尚未提供经独立干净安装验收的一键 Whisper 安装器。未配置时请使用原生手工字幕，或按软件的语音识别设置完成环境；不要把识别按钮当作模型已安装。

## 云端视频、配音与在线素材

查看 [云端功能一览](../README.md#云端功能)，了解 LTX 文生视频、Seedance 首帧图生视频、普通话配音与在线素材各自的入口和使用条件。

### 原生在线资源的账号

助手的 Wikimedia Commons 图片搜索不需要资源服务 Key。主界面的在线资源面板中的 Freesound、Pexels 和 Pixabay 则使用你自己的服务凭据；PhraseSplice 不包含上游的共享 Key。

在启动 PhraseSplice 的同一个 PowerShell 窗口中，按需要设置 `LOCALCUT_FREESOUND_API_KEY`、`LOCALCUT_PEXELS_API_KEY` 或 `LOCALCUT_PIXABAY_API_KEY`，然后运行 `Start-PhraseSplice.cmd`。缺少对应 Key 时，该服务会提示所需变量并停止请求。环境变量从启动时继承，修改后需要重启程序；不要把实际 Key 保存到仓库脚本、Issue 或截图中。

Freesound 的搜索 Key 与下载 OAuth 配置还需匹配。使用自己的 OAuth 应用下载时，在用户自己的 `resourceproviders` 配置中填写该应用对应的 `api.oauth2.clientId`；原生资源配置发现路径和已有自定义 `clientkey` 仍可用。不要修改或提交仓库内的配置来存储私人凭据。资源服务是否可用、账号额度和素材许可由各服务决定，尚未用接收方账号联网验收。

## 文件放在哪里

通过启动器运行时，默认数据根是 `runtime/userdata`，包含工程、助手导出、本地模型和大部分助手缓存。可用 `LOCALCUT_WORKSPACE_ROOT` 指定其他可写英文绝对路径；启动器和模型安装器必须使用同一设置。

```powershell
$env:LOCALCUT_WORKSPACE_ROOT = 'E:\PhraseSpliceData'
.\Start-PhraseSplice.cmd
```

不要使用目录联接、符号链接或网络共享。不要把数据根设成磁盘根。直接运行 exe 而未设置环境变量时，会使用系统用户数据目录下的 `LocalCut`。

**这不是完全零痕迹便携程序。** 普通 Qt/KDE 仍可能在 Windows AppData 写界面配置、书签和部分缓存。助手管理的主要数据放在所选根；原生文件对话框允许你自主选择保存位置。

安装下载缓存位于仓库 `.downloads`。确认安装成功后可删除该缓存以节省空间，后续重装需重新下载。备份时优先保留 `runtime/userdata`、外部引用的原素材和你手动保存的工程。更新时建议使用新的程序目录，先备份数据，再指定原数据根；不要覆盖唯一的工作副本。

## 功能边界

| 功能 | 当前边界 |
| --- | --- |
| 照片故事模板 | 空时间线，至少 3 张照片；其他模板按实际素材要求执行 |
| 普通卡通头像遮挡 | 视频最长 2 分钟，要求清晰单人正脸；无法确认人脸时失败 |
| 立体风格卡通头 | 最长 30 秒，≤60 fps、≤4K，单人正脸；二维光照绘制与关键点跟踪 |
| 照片说话 | 照片＋0.12–30 秒语音，256 像素人脸输出，CPU 较慢 |
| 磨皮和瑕疵修复 | 轻度局部处理；不是完整的人脸重塑系统 |
| 基础增强 | 传统降噪与锐化，不是 AI 超分辨率模型 |
| 免费公共云服务 | 可能排队、限流、变更接口或停服；不保证额度 |
| 付费视频接口 | 明确选择并使用自己的 Key；免费通道失败不会自动转付费 |

头像遮挡和跟踪可能失败，不应作为未经检查的隐私匿名化保证。请预览整个输出，尤其遮挡、快速移动或侧脸位置。

## 常见问题

**路径包含中文**：本预览版请把程序和数据根移到英文路径，再启动。素材文件本身的兼容性还取决于容器、解码器和具体功能。

**下载失败或 SHA 不一致**：检查网络。不要关闭证书验证或略过哈希。若某个缓存文件损坏，仅移走错误提示指定的文件，再重试。

**存在不完整 runtime**：先保留其中的 `userdata`，把 `runtime` 改名后重新安装，不要直接删除已有工程。

**能启动但不能导入/导出**：运行 `scripts/check-runtime.ps1`，报告媒体格式、操作步骤与错误。试用普通短 H.264/AAC MP4 缩小问题范围。

**连接助手后无法完成命令**：检查所选 AI 的登录或 Key、模型名称、工具调用支持、网络和额度。先用一个片段执行简单命令，核对时间线后再增加复杂度。

# 选择自己的 AI

LocalCut 的剪辑工具与 AI 服务分开。切换服务后仍使用同一套原生工程操作、状态校验和撤销机制。模型需要支持工具调用；能聊天不代表能可靠地编辑时间线。

## 在 LocalCut 内发指令

点击 **连接 AI**，选择服务。云端服务填写完整接口地址、实际可用的模型名称或接入点 ID，以及自己的 API Key。预设只减少地址输入，不代表已经开通该服务或包含免费额度。

| 选项 | 完整接口地址 / 连接方式 |
| --- | --- |
| Codex | 已安装并登录的本机 Codex，沿用自己的账号 |
| DeepSeek | `https://api.deepseek.com/chat/completions` |
| 通义千问 | `https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions`；其他地域使用其对应地址 |
| 豆包（火山方舟） | `https://ark.cn-beijing.volces.com/api/v3/chat/completions` |
| Ollama 本地 | `http://127.0.0.1:11434/v1/chat/completions` |
| 自定义兼容接口 | 服务商提供的完整 Chat Completions 地址 |

API Key 仅保留在本次 LocalCut 会话中，关闭程序后需要重新填写。切换服务或接口主机时清除旧 Key；服务名、地址和模型名称可以记住。不要把 Key 发在 Issue、截图或提交记录里。

兼容 API 的“接口已配置”只表示参数已接受，第一次发送指令才会请求模型。认证失败、模型不支持工具、超时和额度不足都会停止本轮，不自动换到另一个付费服务或重放修改。取消不会撤销已经成功执行的操作，需要时使用撤销按钮。

豆包通过火山方舟的官方 API 调用。豆包聊天应用的登录或免费使用资格不等于开通 API。模型名称或接入点 ID 以你在平台实际开通的为准。[火山方舟 Chat API](https://docs.volcengine.com/docs/ark/chat-api)

## 使用本地 Ollama

自行安装 Ollama、下载支持工具调用的本地模型，并保持服务运行；在 LocalCut 填写已经安装的准确模型名称。LocalCut 不自动下载大模型，以免意外占用大量磁盘。

使用本地模型无需模型 API 费用，但占内存、磁盘和计算时间。Ollama 也能连接云模型，选择云模型时不要当作离线推理。参见 [Ollama 兼容接口说明](https://docs.ollama.com/api/openai-compatibility)。

本次 CPU 实机测试中，Qwen 3.5 4B 因可用内存不足中止；2B 能识别完整工具清单并调用读取工具，但把实际 6 秒的工程时长算错。接口兼容不保证编辑可靠，默认助手仍为 Codex；本地模型应先逐项核对结果，再决定是否用于实际剪辑。

## 在 WorkBuddy 里控制 LocalCut

1. 打开 LocalCut，导入素材，点击 **连接 AI → WorkBuddy（MCP）**。
2. 复制软件展示的 MCP JSON，添加到 WorkBuddy 的自定义 MCP 配置。
3. 保持 LocalCut 打开，在 WorkBuddy 中说：

   ```text
   使用 LocalCut 工具读取当前工程。把素材库的视频按顺序放到空时间线，保留原声。
   完成后告诉我结果，暂时不要导出。
   ```

4. 在 LocalCut 预览，继续描述修改或导出要求。

配置也保存在数据根的 `config/localcut-workbuddy-mcp.json`。它指向随运行包提供的 `localcut-mcp.exe`，无需额外安装 Node 或 Python。程序与数据目录保持不变时配置可以复用；移动目录后重新复制配置。

桥接仅在主动启用后运行，使用当前 Windows 用户可访问的本地管道，不监听公网端口、不需要云服务器。一次只执行一个工具任务。切换到其他 AI 或在配置窗口停止外部控制后，MCP 不再接收剪辑操作。

WorkBuddy 自身使用哪个模型、是否需要账号以及额度，由 WorkBuddy 和所选服务决定。LocalCut 没有包含或转移 WorkBuddy 的会员权益。[WorkBuddy 官方连接器文档](https://open.workbuddy.cn/docs/connector)

## 从简单要求开始

先使用仓库的 `examples/localcut-demo.mp4`，要求“读取工程”“顺序排列”“第 2 秒分割”，核对画面、时长和原声，再尝试复杂任务。较小的本地模型可能漏读状态或生成错误参数；软件会拒绝无效操作，但不能保证每个模型都能正确理解任意要求。

实际验证范围见 [VALIDATION.md](VALIDATION.md)。协议夹具通过与某个平台真实账号联测是两类证据。

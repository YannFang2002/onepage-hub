# OnePage Agent Remote — MVP v0.1

> 先只做三件事：**状态页、输入文字、发送**。
> 其它功能（审批、语音、多会话、配对、BLE）全部往后放。

---

## 1. 范围

### 做

1. **状态页**：显示主机连接、Agent/仓库、运行状态、最后一条回复
2. **输入文字**：打开设备键盘，输入一句话
3. **发送**：把文字发给主机，显示发送中/成功/失败，并显示回复
4. **最小设置**：主机地址/端口

### 不做

- 审批 / allow / deny
- 语音 / 麦克风
- 多会话 / 切换 Agent
- 配对码 / device secret
- BLE / 中继
- 回复分页、二维码、诊断页

---

## 2. 用户流程

```text
桌面 → Agent 遥控
        │
        ▼
   状态页（连接/Agent/最后回复）
        │ Confirm
        ▼
   键盘输入文字
        │ Confirm
        ▼
   发送中…
        │
        ├─ 成功 → 显示回复
        └─ 失败 → 显示错误 + 重试
```

---

## 3. 页面

### 3.1 状态页（默认）

```text
┌──────────────────────────────────┐
│ Agent 遥控            WiFi  ▮▮▮  │
├──────────────────────────────────┤
│ 主机: 192.168.1.10:8765          │
│ 状态: 已连接 / 未连接 / 未配置    │
│                                  │
│ Agent:  DSH · onepage-hub        │
│ 运行:  空闲 / 运行中 / 出错       │
│                                  │
│ 最后回复:                        │
│ 「测试已通过，3 个用例全部成功」  │
│                                  │
│ 确认=写消息   返回=桌面           │
└──────────────────────────────────┘
```

- 每 5 秒拉一次 `/status`
- 未配置主机 → 提示「按确认配置主机」
- 未连接 → 显示错误和最后已知状态
- 最后回复截断显示，最多 3 行

### 3.2 输入页

- 复用现有 `KeyboardEntryActivity`
- 单行文本，最长 256 字符
- Confirm 发送；Back 取消
- 发送中显示「发送中…」，禁止重复按
- 成功返回状态页并显示回复
- 失败显示错误码和「重试/取消」

### 3.3 设置页（最小）

- 主机地址（IP）
- 端口
- 连接测试
- 保存到 `/system/.crosspoint/agent.json`

> MVP 可以先不做设置页，用 `agent.json` 预置；但主机地址变化时要能改。

---

## 4. 按键

| 页面 | Confirm | Back |
|---|---|---|
| 状态页 | 打开输入 | 返回桌面 |
| 输入页 | 发送 | 取消 |
| 设置页 | 保存/测试 | 返回 |

MVP 不需要 Up/Down/Left/Right。

---

## 5. 架构

```text
┌──────────── OnePage（应用） ────────────┐
│ AgentRemoteActivity                     │
│  ├─ 状态页（5s 轮询 /status）           │
│  ├─ KeyboardEntryActivity（输入）       │
│  └─ HTTP 客户端（/status /send）        │
└──────────────────┬──────────────────────┘
                   │ 局域网 HTTP
┌──────────────────▼──────────────────────┐
│ 主机服务（最小）                        │
│  GET  /api/v1/status                    │
│  POST /api/v1/send                      │
└─────────────────────────────────────────┘
```

主机服务有两种实现，MVP 二选一：

- **A. DSH 插件**（正式路径）：桌面 App 内插件，`/send` 注入当前 session，`/status` 读 session 状态
- **B. 本地 mock**（先做 UI）：一个 20 行的 HTTP 服务，`/send` 回显文本，用于设备联调

建议先用 B 把设备页面跑通，再接 A。

---

## 6. 协议（MVP）

### 6.1 状态

```http
GET /api/v1/status
```

```json
{
  "ok": true,
  "agent": "DSH",
  "repo": "onepage-hub",
  "state": "idle",
  "lastReply": "测试已通过",
  "updatedAt": 1776507293
}
```

`state`: `idle | running | error | unknown`

### 6.2 发送

```http
POST /api/v1/send
Content-Type: application/json

{"text":"跑一下单测"}
```

```json
{"ok": true, "reply": "3 个用例全部通过", "pending": false}
```

- `pending: true` 时表示已接收、回复稍后取；MVP 可以先同步返回
- 失败：`{"ok":false,"error":"host_unreachable"}`

### 6.3 鉴权

- MVP 可选：`X-Agent-Token`，值写在 `agent.json`
- 只监听局域网
- 不做配对

---

## 7. 设备端实现

### 7.1 文件

| 文件 | 作用 |
|---|---|
| `src/activities/agent_remote/AgentRemoteActivity.{h,cpp}` | 状态页、输入、发送 |
| `src/network/AgentClient.{h,cpp}` | `/status` `/send` |
| `src/agent/AgentConfig.{h,cpp}` | `agent.json` |
| `src/apps/AppRegistry.cpp` | `AppId::AgentRemote = 5` |
| `src/components/icons/agent32.h` | 图标 |
| i18n | `STR_AGENT_*` |

### 7.2 配置

`/system/.crosspoint/agent.json`：

```json
{"host":"192.168.1.10","port":8765,"token":"","lastReply":""}
```

- 首次进入无配置 → 设置页
- 保存后立即重连

### 7.3 网络

- 复用 `WifiCredentialStore` / `WiFi.begin()`
- `esp_http_client` GET/POST
- 超时 5 秒；状态轮询 5 秒
- 失败显示错误，不阻塞 UI

### 7.4 输入

- `KeyboardEntryActivity` 返回字符串
- 空字符串不发送
- 发送中忽略重复按键
- 成功/失败后返回状态页

---

## 8. 主机端实现

### 8.1 最小 DSH 插件（A）

- 包名：`dsh-onepage-remote`
- 安装：`dsh plugin --profile desktop add <tgz>`，重启桌面 App
- `/status`：从 `session/event` 维护最近 Agent/状态/最后回复
- `/send`：`agent.followup(text)` 注入当前 session
- 端口 8765，监听局域网

### 8.2 Mock（B）

- 单文件 Node/Python，`/status` 返回固定状态
- `/send` 回显 `"收到: <text>"`
- 用于设备页面/网络联调，不接 DSH

---

## 9. 分阶段

| 阶段 | 内容 | 验收 | 估时 |
|---|---|---|---|
| **M0** | Mock 服务 + 设备状态页/输入/发送 | 设备输入文字 → mock 返回回声 | 1 天 |
| **M1** | DSH 插件 `/status` `/send` | 设备发送 → 真实 DSH session 收到 | 1–2 天 |
| **M2** | 设置页、连接测试、错误/重连 | 改主机地址可保存；断网有提示 | 1 天 |

**M0 完成即可看到完整交互；M1 才算真正控制 Agent。**

---

## 10. 验收

- 进入应用能看到状态页
- 未配置主机时能进入设置并保存
- 输入文字能发送，发送中不重复
- 成功显示回复；失败显示错误并可重试
- 断网/主机不在时应用不卡死
- 连续发送 10 次无崩溃/泄漏
- 休眠唤醒后状态页恢复

---

## 11. 后续（本轮不做）

- 审批（allow/deny）
- 语音输入
- 多会话
- 配对/secret
- BLE / 中继
- Claude Code / Codex

---

## 12. 待定

1. MVP 主机先用 **Mock** 还是直接做 **DSH 插件**？
2. 发送是同步等回复，还是先返回 `pending` 再轮询？
3. 输入单行 256 字符够不够？
4. 鉴权 MVP 先不做，还是加一个 `X-Agent-Token`？
5. 状态页除了 Agent/状态/最后回复，还要显示什么？

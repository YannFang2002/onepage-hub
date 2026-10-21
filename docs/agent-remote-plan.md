# OnePage Agent 遥控器方案（v0.1）

> 目标：把 onepage-hub 做成一台「AI Agent 专用遥控终端」——像小米遥控器 2 Pro 那样，
> 一个语音键说话，一组实体键操控 Agent 干活。墨水屏作为 Agent 的状态板与回复屏。
>
> 状态：方案设计，未开始实现。前置能力（PDM 录音）已完成。

---

## 0. 一句话方案

**WiFi 直连 Agent 服务为主链路，按键映射为控制层，BLE HID 外设模式作为可选增强。**

语音走「按住说话 → WAV → HTTP POST → ASR → Agent」，按键走「本地键位表 → 动作 ID → HTTP /action」，
回复走「轮询或 WS 推送 → 墨水屏渲染」。整机不常连网，按下说话键才拉起 WiFi。

---

## 1. 现状盘点：能复用什么

| 能力 | 现状 | 结论 |
|---|---|---|
| PDM 录音 | `lib/Mic/PdmMic`（16 kHz mono，CIC-3 软件抽取），`RecorderActivity` 已产出合法 WAV | **直接复用**，改缓冲策略即可 |
| 录音缓冲 | PSRAM 480 KB / 15 s（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`） | 够用；多轮会话要落盘而非常驻 |
| WiFi | STA/AP 双模、多 SSID 凭据存储（`WifiCredentialStore`）、扫码入网 | **直接复用** |
| HTTP | `HttpDownloader` 只有 `fetchUrl`（GET，带超时/CA/重定向） | **需新增 POST**（语音体 480 KB） |
| JSON | 工程内 `JsonParser` / ArduinoJson | 复用 |
| 二维码 | `util/QrUtils` | 复用（配对码、Agent 返回的链接） |
| Web 设置页 | `CrossPointWebServer` + `/api/settings` | 复用（把 Agent 地址/密钥做成设置项） |
| OTA | `OtaUpdater` / `OtaBootSwitch` | 复用 |
| BLE | `BleHidHost` = **Central + HID Host**（收翻页器按键） | 语音链路**用不上**；HID 外设需另写 |
| 按键 | `Back / Confirm / Left / Right / Up / Down / Power`，共 7 个物理键 | 够映射 |
| 扬声器 | 未驱动（`voice-mic-plan.md` 仍列为待确认） | **按无扬声器设计**，输出只有屏幕 |

### 两条硬约束

1. **内部 SRAM 仅剩约 48 KB**（实测 `Free: 48114, MaxAlloc: 38900`）。
   NimBLE 外设角色 + HID 服务约需 20–40 KB，且只能放内部 RAM（不能进 PSRAM）。
2. **Flash 分区**：2026-10-21 起 App 单槽 7.94 MB，当前占用 68%，余量 2.6 MB。
   新增功能不受限，但 BLE 相关组件会显著增重。

---

## 2. 路线取舍

| | A. WiFi 直连 Agent | B. BLE HID 外设（真遥控器） |
|---|---|---|
| 设备角色 | HTTP 客户端 | BLE Peripheral + HID Device |
| 语音上行 | 480 KB 一次性 POST，1–2 s | 16 kHz PCM = 32 KB/s，BLE 实吞吐 20–60 KB/s，**勉强**且易丢包 |
| Agent 控制 | 自定义 API，语义自由 | 只能模拟键盘/消费类按键，语义受限 |
| 主机侧依赖 | 跑 Agent 服务的那台机器 | 需要配对 + 一个把 HID 事件转成 Agent 指令的守护进程 |
| 免配置 | 要配 WiFi | 系统级配对即可，通用性强 |
| 功耗 | WiFi TX 峰值 ~150 mA | BLE 常连 ~几 mA，**待机优势明显** |
| 现有代码 | 80% 可复用 | 角色相反，基本从零 |
| SRAM 风险 | 低 | **高**（48 KB 余量） |

**结论：A 做主线，B 推迟到最后一期且只做「键盘遥控」不做「BLE 传音频」。**

理由：BLE 传 15 秒音频在 MTU 和连接间隔约束下既慢又不稳，而且无论如何都要在主机侧写一个守护进程
把事件转给 Agent——既然注定要写守护进程，不如直接走 HTTP，语义还能是 `approve`/`reject` 这种
意图级动作，而不是 `KeyCode 0x42`。

B 的真正价值不是语音，而是**通用性**：不配 WiFi 也能当 PC 键盘遥控器用。所以它适合作为增强模式。

---

## 3. 架构

```
┌─────────────────────────── OnePage (ESP32-C61) ───────────────────────────┐
│                                                                           │
│  ┌──────────────┐   ┌──────────────────┐   ┌──────────────────────────┐  │
│  │ PdmMic       │──▶│ VoiceBuffer      │──▶│ AgentClient              │  │
│  │ 16k mono PCM │   │ PSRAM 480KB/15s  │   │  POST /voice (WAV)       │  │
│  └──────────────┘   └──────────────────┘   │  GET  /poll              │  │
│         ▲                                  │  POST /action            │  │
│         │ hold-to-talk                     └───────────┬──────────────┘  │
│  ┌──────┴───────┐   ┌──────────────────┐              │                  │
│  │ KeyMapper    │──▶│ ActionBus        │──────────────┘                  │
│  │ keymap.json  │   │ 语义动作 ID      │                                 │
│  └──────────────┘   └──────────────────┘                                 │
│         ▲                                                                │
│  ┌──────┴───────┐   ┌──────────────────┐   ┌──────────────────────────┐  │
│  │ 7 物理键      │──▶│ AgentActivity    │──▶│ E-Ink 800×480 1-bit      │  │
│  └──────────────┘   │ 状态机           │   │ 状态 + 回复分页          │  │
│                     └──────────────────┘   └──────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────┘
                                  │ WiFi (按需拉起)
                    ┌─────────────▼──────────────┐
                    │ Agent 服务 (Hermes/OpenClaw │
                    │  + onepage-agent-channel)  │
                    │  ASR → LLM/工具 → 回复      │
                    └────────────────────────────┘
```

---

## 4. 交互设计

### 4.1 按键（默认键位，可在设置里改）

| 按键 | 短按 | 长按（≥1 s） |
|---|---|---|
| Confirm | 发送当前录音 / 重录 | **按住说话**（松手即发送） |
| Back | 返回 / 退出 Agent | 中断当前回合（cancel） |
| Left | 上一条消息 | — |
| Right | 下一条消息 | — |
| Up | 回复页向上翻 | — |
| Down | 回复页向下翻 | — |
| Power | 系统电源（不改语义） | — |

对 Agent 的语义动作（`POST /action`）：

`new_session` `cancel_turn` `approve` `reject` `scroll_up` `scroll_down` `prev_message` `next_message`

> 设计要点：**动作是意图级的**，不是键值级的。这样换键位不需要改服务端。

### 4.2 状态机

```
Idle ──按住Confirm──▶ Listening ──松手──▶ Uploading ──▶ Thinking ──▶ Reply
 ▲                        │(>15s 自动停)        │失败              │
 │                        └──Back──▶ Idle       ▼                  │
 └────────────── Back ◀────────────── Error ◀───────────────────────┘
```

### 4.3 屏幕（墨水屏特有约束）

- **不做动画、不做秒表、不做波形**。每次只做状态级刷新（半刷 ~1.3 s，快刷 ~0.6 s）。
- Listening：静态「● 聆听中… 松手发送」+ 一个 1 Hz 更新的 1-bit 进度条（局部刷新）。
- Thinking：静态提示，可选显示上一轮的 `user_text`。
- Reply：正文分页 + 底部路径条显示「第 n/m 页 · 回合 k」。
- Error：原因 + 重试/返回提示。
- 无扬声器 → 所有反馈只能上屏，长耗时操作必须在 2 s 内先给一个状态变化。

---

## 5. 协议（沿用 `voice-mic-plan.md` 已定义的，加一个 /action）

| 接口 | 方法 | 说明 |
|---|---|---|
| `/api/onepage/health` | GET | 探活，用于设置页「测试连接」 |
| `/api/onepage/hello?device_id=` | GET | 配对：返回 `pairing_code` 或 `device_secret` |
| `/api/onepage/voice` | POST | `Content-Type: audio/wav`，头带 `X-Device-Id` / `X-Device-Secret`，≤1 MB；返回 `{user_text, reply, pending, turn}` |
| `/api/onepage/poll?device_id=` | GET | `pending=true` 时轮询，取 `{reply, turn}` |
| `/api/onepage/action` | POST | **新增**：`{action:"approve", session:"..."}` → `{ok:true, state:"..."}` |
| `/api/onepage/ws` | WS | **可选**：服务端推送，替代轮询 |

设备本地配置 `/system/.crosspoint/agent.json`：

```json
{
  "url": "http://192.168.1.20:8765",
  "deviceId": "ONEPAGE_8856A6CFD928",
  "secret": "<base64 混淆，复用 ObfuscationUtils>",
  "keymap": {
    "Confirm":   { "hold": "voice",        "tap": "resend" },
    "Back":      { "tap": "exit",          "hold": "cancel_turn" },
    "Left":      { "tap": "prev_message" },
    "Right":     { "tap": "next_message" },
    "Up":        { "tap": "scroll_up" },
    "Down":      { "tap": "scroll_down" }
  },
  "wifiOnDemand": true,
  "maxSeconds": 15
}
```

---

## 6. 代码落地

新增（都在 `src/` 下，不碰阅读器内核）：

| 文件 | 作用 |
|---|---|
| `src/network/HttpUploader.h/.cpp` | POST 二进制体（`esp_http_client_open` + 分块 `write`），补齐 `HttpDownloader` 只能 GET 的缺口 |
| `src/agent/AgentClient.h/.cpp` | `/voice` `/poll` `/action` 封装 + JSON 解析 + 配对 |
| `src/agent/AgentConfig.h/.cpp` | 读写 `/system/.crosspoint/agent.json`（复用 `ObfuscationUtils` 存 secret） |
| `src/agent/KeyMapper.h/.cpp` | 键位表 → 语义动作，长按/短按判定 |
| `src/activities/agent/AgentActivity.h/.cpp` | 状态机 + 渲染 + hold-to-talk |
| `src/agent/WifiOnDemand.h/.cpp` | 按需拉起/断开 WiFi，带回退与超时 |

改动（很小）：

- `src/apps/AppDescriptor.h`：`AppId` 追加 `Agent = 5`
- `src/apps/AppRegistry.cpp`：`kApps` 追加一行 + 工厂
- `lib/I18n/translations/{english,chinese_simplified,chinese_traditional}.yaml`：`STR_AGENT_*` 一组键，再跑 `scripts/gen_i18n.py`
- `lib/Mic/PdmMic`：无需改动（录音已验证可用）

复用但要注意的：

- `RecorderActivity` 的 `skipLoopDelay()` 全速锁逻辑要一起带过来——CIC 抽取在 10 MHz 下会丢样。
- 录音结束判定沿用 `finalizeRecording()` 的最小 0.5 s / 最大 15 s。

---

## 7. 分阶段计划

| 阶段 | 内容 | 验收 | 估时 |
|---|---|---|---|
| **P0** | `HttpUploader` + `AgentClient` + 配置读写 + 设置页「测试连接」 | 串口触发一次上传，屏幕显示 ASR 原文 | 1–2 天 |
| **P1** | `AgentActivity`：hold-to-talk、状态机、回复渲染分页 | 按住说一句 → 屏幕出现 Agent 回复 | 2–3 天 |
| **P2** | `KeyMapper` + `/action` 打通（approve/reject/翻页） | 按键能驱动 Agent 侧行为 | 1–2 天 |
| **P3** | 会话落盘 `/system/.crosspoint/agent/`、离线回看、二维码渲染 | 重启后可回看历史回合 | 1–2 天 |
| **P4**（可选） | BLE HID 外设模式：配对 PC 当键盘/消费键遥控 | 配对后按右键 = PageDown | 2–3 天，**取决于 SRAM** |
| **P5**（可选） | WiFi 常连 + WS 推送替代轮询 | 服务端主动推送能即时上屏 | 1–2 天 |

**建议先做 P0+P1**，这两步做完就已经是「能用的语音 Agent 遥控器」了。

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| **内部 SRAM 仅 ~48 KB** | 加 BLE 外设可能 OOM | P0–P3 完全不碰 BLE；P4 前先实测 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`，不够就放弃 |
| WiFi 与 BLE 共存抢射频 | 两者同时活跃时丢包/断连 | 分时使用：BLE 模式不联网；语音回合内不扫描 BLE |
| 功耗 | 1000 mAh 撑不住常连 | **按需 WiFi**：按下说话键才连，回合结束断开。一次回合 ~20 s × ~200 mA ≈ 1.1 mAh |
| 墨水屏刷新慢 | 状态反馈迟钝、残影 | 只做状态级刷新；1 Hz 进度条用局部刷新；回复分页不做滚动动画 |
| `HttpDownloader` 无 POST | 语音传不上去 | 新增 `HttpUploader`，不要硬改既有 GET 路径（阅读器 OTA/OPDS 依赖它） |
| 明文 HTTP + device_secret | 局域网可嗅探 | 文档写明仅限可信局域网；后续可上 HTTPS（C61 有 PSRAM，够放 CA bundle） |
| 无扬声器 | 无法播报 | 回复只上屏；错误必须可读，不能只靠图标 |
| Agent 回复过长 | 一屏放不下 | 分页 + 「第 n/m 页」；超长回复截断并落盘，可上翻查看 |

---

## 9. 需要你拍板的几点

1. **Agent 服务在哪跑？** 局域网某台机器（Hermes/OpenClaw），还是公网可访问？决定要不要 HTTPS。
2. **hold-to-talk 还是按两下？** 我按「按住说话、松手发送」设计（最像遥控器语音键）。
3. **要不要 BLE 键盘模式？** 如果目标只是控制 Agent，P4 可以永久砍掉，省 30+ KB SRAM。
4. **回复要不要语音播报？** 若板子其实有扬声器（目前固件未驱动），可以补一条 TTS 下行，但流式播放与 I2S 会和麦克风争外设。
5. **单轮 15 秒够不够？** 长问题可以改成 30 秒（PSRAM 要 960 KB，仍充裕）。

---

## 参考

- 既有草案：[`docs/voice-mic-plan.md`](./voice-mic-plan.md)（协议与 Agent 服务调研）
- 录音实现：`lib/Mic/PdmMic.{h,cpp}`、`src/activities/recorder/RecorderActivity.{h,cpp}`
- 网络基础：`src/network/HttpDownloader.{h,cpp}`、`src/network/CrossPointWebServer.cpp`
- 按键与电源：`src/MappedInputManager.h`、`lib/hal/HalPowerManager.{h,cpp}`
- 应用注册：`src/apps/AppRegistry.cpp`（新增桌面应用只需一行表项）

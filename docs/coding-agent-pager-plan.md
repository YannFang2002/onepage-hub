# 编码 Agent 遥控终端方案（v0.1）

> 目标：让 OnePage 成为 **DSH / Codex / Claude Code** 这类编码 Agent 的**远程寻呼机与审批终端**。
> 与 [agent-remote-plan.md](./agent-remote-plan.md) 的区别：那版面向 Hermes/OpenClaw 这类**对话** Agent，
> 这一版面向**在代码仓库里干活、需要人拍板**的**编码** Agent。两者产品定位不同，不是替代关系。
>
> 状态：方案设计，未开始实现。本机已装 `claude` / `codex 0.163.0` / `dsh 0.1.1-rc.2`，接口均已实测。

---

## 0. 结论

**能实现，但产品定位要换。**

编码 Agent 的核心矛盾不是「我懒得打字」，而是：

- 任务一跑十几分钟，人不可能一直盯屏幕
- **关键节点必须有人批准**：要执行命令、要改文件、要装依赖、要推送
- 事后要能一眼看到「它刚才干了什么」

所以这块硬件的价值是 **寻呼机 + 审批器 + 状态牌**，语音是附加能力：

| 能力 | 价值 | 说明 |
|---|---|---|
| **远程审批** | ★★★★★ | 人不在电脑前也能按「允许/拒绝」，这是最难被替代的 |
| **状态牌** | ★★★★☆ | 墨水屏零功耗常显「哪个仓库、在跑什么、卡在哪」 |
| 语音下指令 | ★★★☆☆ | 只适合短指令（「跑测试」「提交」「解释这个报错」）；长 prompt 含代码，语音输入很痛苦 |
| 键盘遥控 | ★★☆☆☆ | 有 Codex `remote-control` 官方入口后，自研 BLE HID 的必要性下降 |

---

## 1. 三家接口实测结果

| | DSH 0.1.1-rc.2 | Codex 0.163.0 | Claude Code |
|---|---|---|---|
| 一次性执行 | `dsh --profile headless "任务"` | `codex exec --json "任务"` | `claude -p --output-format stream-json` |
| 流式输出 | 需实测 | `--json`（事件流） | `stream-json`，还支持 `--include-partial-messages` |
| 双向流 | — | `app-server proxy`（stdio ↔ 控制 socket） | `--input-format stream-json`（可双向管道） |
| 会话续接 | `--resume <session>` | `resume` / `fork` / `queue` / `agents` | `--resume <id>` / `--bg`（后台跑并返回 id） |
| 常驻服务 | `dsh web --host --port`，背后是 `/api`，有 **browser-trust fence** | `codex app-server daemon` + `remote-control start/stop/pair`（带配对码、`--json`） | 无官方 daemon |
| 审批机制 | approval policy（ask/never），走 `/api` | `--sandbox` + 审批事件 + `remote-control` 配对 | **`PreToolUse` hook，可阻塞并返回 allow/deny** |
| 稳定性 | `/api` 是 GUI 后端，非稳定契约 | `app-server` / `remote-control` 标注 **[experimental]** | hook 是正式功能，最稳 |

### 三个关键判断

1. **Codex 已经内建了远程控制入口**：`codex remote-control start` + `pair` 生成短期配对码，`--json` 输出。
   这说明官方就在做「第三方遥控」，值得优先对接，而不是自造 HID。
2. **Claude Code 的 `PreToolUse` hook 是实现「远程批准」最干净的路径**：hook 收到工具调用 JSON，
   可以阻塞等待，再返回 `{"permissionDecision":"allow"|"deny"}`。桥接层只要在这个 hook 里向设备要答案即可。
3. **DSH 的 `/api` 有 browser-trust fence**：`--trusted-host <authority>` 必须把设备访问时用的
   `IP:port` 加进去，否则设备请求会被拒。这点不实测很容易踩。

---

## 2. 架构：主机侧统一网关

三家协议各不相同，不可能让 48 KB SRAM 的 MCU 分别适配。所以引入一个主机侧网关：

```
┌─────────────┐   HTTP/长轮询    ┌──────────────────────────────────────┐
│  OnePage    │◀────────────────▶│ onepage-pager (运行在开发机上)        │
│  ESP32-C61  │   /api/v1/*      │                                      │
│             │                  │  ┌────────────┐  ┌────────────────┐ │
│  7 键       │                  │  │ 归一化模型 │  │ 会话/审批状态机 │ │
│  墨水屏     │                  │  │ task/event │  │ pending/decision│ │
│  PDM 麦克风 │                  │  └─────┬──────┘  └───────┬────────┘ │
└─────────────┘                  │        │                 │          │
                                 │  ┌─────▼─────┬─────▼─────┬─────▼───┐ │
                                 │  │ claude    │ codex     │ dsh     │ │
                                 │  │ adapter   │ adapter   │ adapter │ │
                                 │  └─────┬─────┴─────┬─────┴─────┬───┘ │
                                 └────────┼───────────┼───────────┼─────┘
                                          │           │           │
                                    hook 回调     app-server    /api
                                    (PreToolUse)  控制 socket   (trusted-host)
```

网关职责（这些都必须放在主机侧）：
- 管 session id、工作目录、仓库
- 归一化三家的事件模型
- 维护「待审批队列」与超时策略
- **安全边界**：设备只能批准它被授权批准的东西

---

## 2.5 上位机是必须的吗？—— DSH 专项实测

**必须有，而且不是取舍问题。** DSH / Codex / Claude Code 都是跑在带完整操作系统的机器上的
Node / Rust 程序，ESP32-C61（320 KB SRAM、无 MMU、无 OS）一家也跑不起来。设备只能是**瘦终端**。

但「上位机」有两种含义，要分开看：

- **宽泛**：跑 agent 的那台电脑 —— 本来就在跑
- **具体**：除了 agent 之外，**还要不要再单独跑一个自己写的服务**

答案是分家的：**Claude Code 必须另起；DSH 可以不用。**

### DSH 的三个可接入面（本机实测）

| 面 | 实测结果 | 能否做遥控器 |
|---|---|---|
| `dsh web`（桌面应用，`127.0.0.1:4336`） | 常驻 HTTP；`/api` 一律 **401**，且有 **browser-trust fence**（`--trusted-host <authority>`） | 能，但 `/api` 是 GUI 的后端契约，非稳定接口 |
| `dsh --profile headless "任务"` | **只打印最终 assistant message 就退出**，无流式、无审批、无 session | **不能** |
| `~/.dsh/sessions/<项目>/session-<id>/session.v4.jsonl.zstd` | zstd 压缩的 JSONL，按项目分目录、按 session 分文件 | 只能做**只读状态牌**，无法回传审批 |

补充：`~/.dsh/storages/` 下有 `session_projcache.json`、`workspace.json`，可用来枚举「当前在哪些仓库有活跃会话」。

结论：**DSH 想做到完整遥控，现实路径是让 `dsh web` 对外监听并直连 `/api`**：

```bash
dsh web --host 0.0.0.0 --port 4336 --trusted-host 192.168.1.23:4336
```

代价是把浏览器用的 `/api` 当设备协议用 —— 鉴权（401 的 token 机制，多半在 `window.__DSH_BOOT__` 里）
和 payload 形状都面向 GUI，版本升级容易破。

### DSH 的最优解：写一个 DSH 插件，而不是外挂 shim

DSH 本身是**插件化**的（`dsh plugin --profile <name> add <package>`，`~/.dsh/.agent-presets`、
`skins`、`task-board` 都是插件产物）。这意味着最干净的做法是：

> **把设备服务做成一个 DSH 插件**，与 agent 同进程，直接订阅审批/会话事件，并对外开一个给设备用的 HTTP 端口。

好处：
- 不依赖非稳定的 `/api` 契约，插件能拿到内部事件
- 不需要第二个常驻进程（DSH 进程就是上位机）
- 审批可以被插件直接拦截并阻塞，和设备做真实往返

这条只在 DSH 上成立 —— Codex 和 Claude 没有等价的可扩展宿主，所以仍然需要独立 shim。

### 三家对比：上位机厚度

| | DSH | Codex | Claude Code |
|---|---|---|---|
| 现成常驻服务 | ✅ `dsh web` | ⚠️ `app-server`（本地控制 socket，**非 HTTP**） | ❌ 无 |
| 可扩展宿主 | ✅ **插件机制** | ⚠️ MCP / 实验性 feature | ⚠️ hooks |
| 设备直连可行 | ⚠️ 勉强（401 + trust fence） | ❌ 需要 shim 暴露 HTTP | ❌ 必须 hook 回调 |
| 推荐上位机 | **DSH 插件**（0 额外进程） | 薄 shim | 薄 shim（hook 接收端） |

### 开源同类项目实测（2026-10 检索）

> web_search / web_fetch 走 DeepSeek 的 tools 端点，当前 API key 失效（401），
> 以下改由 `api.github.com` 公开检索 API + `raw.githubusercontent.com` 直接取得，均为真实数据。

| 项目 | ★ | 做法 | 与本方案关系 |
|---|---|---|---|
| **[op7418/m5-paper-buddy](https://github.com/op7418/m5-paper-buddy)** | 164 | M5Paper V1.1 墨水屏 + **Python daemon** + **Claude Code 插件 + PreToolUse hook**；多会话面板、全屏审批卡、PUSH 同意 / DOWN 拒绝 | **几乎同一个产品**，架构可直接照搬 |
| [niclasvestlund-YT/vibepulse](https://github.com/niclasvestlund-YT/vibepulse) | 212 | ESP32-S3 屏 + **PC 上纯 stdlib Python 服务**；"NEEDS YOU" 提醒可点按回答 | 同一模式，偏额度/状态看板 |
| [overwirehq/claude-code-telegram](https://github.com/overwirehq/claude-code-telegram) | 2815 | Telegram bot 远程操作 Claude Code | 宿主侧 bridge + IM 前端 |
| [JessyTsui/Claude-Code-Remote](https://github.com/JessyTsui/Claude-Code-Remote) | 1287 | 邮件/Discord/Telegram 远程控制 + 通知 | 同上 |
| [Emanuele-web04/remodex](https://github.com/Emanuele-web04/remodex) | 3344 | Codex 远程控制 | Codex 侧同类 |
| [giuliastro/harness-remote](https://github.com/giuliastro/harness-remote) | 420 | Codex/Claude/OpenCode 的"原生 session 控制面" | 多 agent 统一网关的现成参考 |

**结论：三个独立实现，全部是「PC 侧 daemon + 设备做瘦终端」。没有一个把 agent 塞进设备。**
我们的判断被完全印证。

### m5-paper-buddy 的关键接线（可直接抄）

daemon 是单个 Python 文件（944 行，仅用 stdlib `http.server`），链路：

```
Claude Code hook ──POST──▶ daemon :9876/hook ──serial/BLE──▶ M5Paper
                                ▲                                │
                                └────── permission ack ───────────┘
```

hook 配置合并进 `~/.claude/settings.json`，所有事件统一 POST 到本地端口：

```json
"PreToolUse": [{
  "matcher": ".*",
  "hooks": [{"type": "command",
    "command": "curl -sS --max-time 40 -X POST --data-binary @- http://127.0.0.1:9876/hook 2>/dev/null || echo '{}'"}]
}]
```

三个值得抄的细节：

1. **`--max-time 40`**：只有 `PreToolUse` 给 40 秒（等人按键），其余事件 3 秒超时。分离得很干净。
2. **`|| echo '{}'`**：daemon 没起来时 hook 变成 no-op，Claude Code 照常工作 —— **优雅降级**，这点很重要。
3. **设备侧协议是"一行一个 JSON"**（line-based JSON），不是设备发 HTTP。daemon 负责所有 HTTP，MCU 只做读行/写行。

### 和我们方案的差异（需要你决定）

| | m5-paper-buddy | 我们的 OnePage |
|---|---|---|
| 传输 | USB 串口（默认）/ BLE NUS | **WiFi**（已配好 SSID）或 USB |
| 定位 | 桌面伴侣，插着线用 | **可带走的寻呼机** |
| 屏幕 | 4.7" 540×960 + 触摸 | 4.26" 800×480 无触摸 |
| 输入 | 3 键 + 触摸 | **7 键** |

它选串口/BLE 是因为定位是"桌面伴侣"，零配置最省事。我们要做「人离开工位也能收到审批」，
所以 **WiFi 是差异化点**，但代价是要处理断线重连与功耗 —— 建议两条都做：USB 优先、WiFi 兜底。

---

共同点：**没有任何项目让设备直连模型或托管 agent**，全部是「设备=瘦终端，宿主=大脑」。
这和我们上面的判断一致。


---

## 2.6 传输选型：BLE 首发，WiFi 预留，不做 USB

**已定：不用 USB 作为运行期传输。**（烧录/救砖仍走 USB，属开发行为，不影响产品形态。）
**首版只做 BLE 通道；WiFi 在界面里预留但置灰，等 BLE 链路跑通后再补。**

### 现有配置已经够用（实测）

`framework-arduinoespressif32-libs/esp32c61` 的 `sdkconfig.h` 里：

```
CONFIG_BT_ENABLED                   1
CONFIG_BT_NIMBLE_ENABLED            1
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL    1    ← BLE 外设角色已编译进来
CONFIG_BT_NIMBLE_ROLE_CENTRAL       1
CONFIG_BT_NIMBLE_MAX_CONNECTIONS    3
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU  256   ← 每次 notify 可带 ~244 B
CONFIG_SOC_BLE_50_SUPPORTED         1    ← 2M PHY 可用
CONFIG_ESP_COEX_SW_COEXIST_ENABLE   1    ← 互斥使用后此项实际用不上
```

即：**不需要改 sdkconfig 就能直接做 BLE 外设。**

### 两条通道的分工（首发只启用前者）

| 通道 | 承载 | 状态 |
|---|---|---|
| **BLE（GATT / NUS 风格 + ADPCM）** | 状态、审批回执、短指令、语音 | ✅ **首版** |
| WiFi（TCP + ADPCM） | 长距离场景兜底 | ⏳ 预留，界面置灰 |
| USB | **仅开发**：烧录、救砖、串口日志 | 不作为运行期链路 |

### 必须注意的三点

1. **SRAM 仍然是最大风险**：空闲时内部 RAM 余量约 48 KB。NimBLE 外设 + 1 条连接约需 15–25 KB。
   若不够，先砍 `CONFIG_BT_NIMBLE_ROLE_CENTRAL`（翻页器功能）腾空间。
   BLE 缓冲只能放内部 RAM，进不了 PSRAM。
   **只跑 BLE 不启动 WiFi 协议栈**，本身就省下一大块。
2. **不做射频共存**：WiFi 协议栈完全不启动（不调 `esp_wifi_*`），所以不存在时间片抢占问题。
3. **语音可以过 BLE，但不能裸传 PCM**（修正 v0.1 里"绝不过 BLE"的说法）：
   裸 PCM 是 16 kHz × 16 bit = **32 KB/s**，15 秒就是 480 KB，按 BLE 实测 20–40 KB/s 要传 12–24 秒，
   体验不可接受。但加上"压缩 + 流式"后完全可行，见下节 2.7。


### 需要新写的部分

现有 `src/bluetooth/BleHidHost` 是 **Central + HID Host**（去收翻页器按键），**角色相反、不能复用**。
需要新增：

| 文件 | 内容 |
|---|---|
| `lib/Ble/NusServer.{h,cpp}` | NimBLE peripheral + 自定义 GATT（RX write / TX notify）+ 配对绑定 |
| `lib/Ble/LineProtocol.{h,cpp}` | 一行一个 JSON 的编解码与缓冲 |
| `src/pager/BleTransport.{h,cpp}` | 与 WiFi 传输统一到同一个 `Transport` 接口 |

配对用 passkey 上屏（复用电墨水屏文本渲染），用户输入到 PC 端 `bleak`/系统配对框。
估 400–600 行，2–3 天。

---

## 2.7 语音能不能走 BLE？能，但要靠压缩 + 流式

### 先算清吞吐

C61 的 BLE 能力（实测配置）：`CONFIG_SOC_BLE_50_SUPPORTED=1`、`CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=1`、
`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256`。即 **BLE 5.0 + 2M PHY + MTU 256**，
每次 notify 可带约 244 字节有效载荷。

保守估算（PC 侧普通蓝牙适配器，非实验室极限）：

| 配置 | 实用吞吐 |
|---|---|
| 1M PHY + MTU 256 + DLE | 12–30 KB/s |
| **2M PHY + MTU 256 + DLE** | **25–60 KB/s** |

### 三种编码下的传输耗时

| 编码 | 码率 | 15 s 数据量 | @30 KB/s 耗时 |
|---|---|---|---|
| 16 kHz PCM 裸传 | 32 KB/s | 480 KB | **16 s（不可接受）** |
| **IMA ADPCM 4:1** | 8 KB/s | 120 KB | **4 s（可接受）** |
| G.711 μ-law 2:1 | 16 KB/s | 240 KB | 8 s（勉强） |

### 三个关键手段

1. **IMA ADPCM 4:1 压缩**
   MCU 端编码器约 20 行、CPU 占用可忽略（比 CIC 抽取轻几个量级）。
   **CSV/WAV 兼容性问题由 daemon 解决**：设备只发 ADPCM 块，daemon 解码成标准 16 kHz PCM WAV 再送 ASR。
   反正 daemon 本来就在主机上跑，这一步零成本。
2. **边录边传（流式）**
   不要"录完再传"。每 200 ms 推一个 ADPCM 块，用户说完时 90% 已经传完，**尾部延迟只剩 1–2 秒**。
3. **缩短时长**
   一句指令 5 秒通常够。5 s ADPCM = 40 KB ≈ **1.3 秒传完**。

### 结论：BLE 传语音可行，但要接受两个代价

| 代价 | 说明 | 对策 |
|---|---|---|
| **距离短** | BLE 约 10 m（隔墙更短），WiFi 有 30–50 m | 要"走到另一个房间"仍需要 WiFi；同屋用 BLE 足够 |
| **2M PHY 依赖对端** | PC 蓝牙适配器要支持 2M PHY（绝大多数支持） | 实测确认；不支持时自动回落 1M + 更长传输 |

补充：ESP32-C61 **只有 BLE（无 Classic BT / 无 BR/EDR）**，所以 A2DP/HFP 那条路不存在，
LE Audio（LC3 over ISO）也不在本芯片能力内 —— **GATT 自定义流是唯一可行路径**，这也正是下面方案的做法。

### 传输架构定稿：双通道互斥，运行期只开一个射频

**原则：BLE 通道与 WiFi 通道是两条等价、互斥的链路，同一时刻只允许一条存活。**
不是"共存分摊"，是"二选一"。

```
                    ┌──────────────────────────────┐
                    │   Transport (统一接口)        │
                    │   send(line) / onLine(cb)     │
                    └───────┬──────────────┬────────┘
                            │              │
                   ┌────────▼───────┐  ┌───▼────────────┐
                   │ BleTransport   │  │ WifiTransport  │
                   │ GATT + ADPCM   │  │ TCP + ADPCM    │
                   └────────┬───────┘  └───┬────────────┘
                            │              │
                    ┌───────▼──────────────▼────────┐
                    │ RadioArbiter（互斥仲裁）        │
                    │ 保证任一时刻最多一个射频在跑     │
                    └───────────────────────────────┘
```

#### 通道选择：进入应用时手动选，选定后不自动跳

**不做自动回退。** 通道在进入应用时由用户选一次，之后一直用这条，直到用户主动改。

```
进入 Pager 应用
      │
      ▼
┌─────────────────────────────┐
│  选择连接方式                 │
│                             │
│   ▸ 蓝牙（BLE）              │
│     Wi-Fi（未实现，置灰）      │
│                             │
│   确认=连接   返回=退出        │
└─────────────────────────────┘
      │
      ├── BLE  ──▶ 常驻广播 + 自动重连
      └── WiFi ──▶ P0 阶段不实现（接口预留）
```

选择结果写入 `/system/.crosspoint/agent.json` 的 `channel` 字段，下次进入默认选中上次的选项。

#### BLE 通道内部：自动重连，不需要用户干预

"自动"体现在**通道内部**，而不是通道之间：

| 场景 | 行为 |
|---|---|
| 设备开机 / 进入应用 | 立即开始广播，等 PC 连入 |
| PC 断开（关机、蓝牙关闭、走远） | **持续重新广播**，连接参数回到默认 |
| PC 回来 | 自动重连（复用已绑定的 bond，不需要重新配对） |
| 连上后重连风暴 | 指数退避：1 s → 2 s → 4 s → 8 s，上限 8 s |
| 正在传输录音时断链 | 本轮作废，屏幕提示「蓝牙断开」，重连后需重新说 |

状态机：

```
IDLE ──进入应用──▶ SELECT_CHANNEL
SELECT_CHANNEL ──选 BLE──▶ BLE_ADVERTISING
BLE_ADVERTISING ──PC 连入──▶ BLE_CONNECTED
BLE_CONNECTED ──断链──▶ BLE_ADVERTISING（指数退避）
SELECT_CHANNEL ──选 WiFi──▶ WIFI_UNAVAILABLE（P0 提示"暂未实现"）
```

要点：
- **广播不设超时**，一直播到连上为止（电池代价用长广播间隔抵消，如 1–2 s）
- **绑定信息存 NVS**，重连不需要重新输 passkey
- **不做省电断连**：一旦连上就保持，断链才重新广播

#### 射频生命周期

P0 只有 BLE，所以：

| 进入 BLE 通道 | 说明 |
|---|---|
| `nimble_port_init()` + GATT 注册 + 开始广播 | 只初始化 NimBLE |
| **不调用任何 `esp_wifi_*`** | WiFi 协议栈完全不启动，省 SRAM |

这也让 P0 的 SRAM 压力最小：只有一个 BLE 协议栈在工作。

#### WiFi 选项的处理

`Wi-Fi` 在 P0 显示为**置灰 + 提示「暂未实现」**。这样界面结构已经是最终形态，
后续接 WiFi 只是把置灰项变成可选项，不用改导航逻辑。

> 注：若以后要恢复"自动回退"，只需在 `BLE_ADVERTISING` 上挂一个超时转移，
> 界面和状态机都不用动。

#### 这个设计带来的三个实际好处

1. **SRAM 峰值降低**：两个协议栈不同时初始化，48 KB 余量下的压力显著缓解。
2. **功耗模型清晰**：BLE 常连约几 mA，WiFi 常连几十 mA；同一时刻只有一种。
3. **不需要射频调优**：不用处理 WiFi/BLE 时间片抢占导致的音频抖动。

#### 对主机侧 daemon 的要求

PC 没有这个约束（蓝牙和 WiFi 通常是独立适配器），所以 **daemon 必须同时监听两路**：
BLE 侧等设备连入，WiFi 侧监听 TCP 端口。设备连到哪条，就用哪条的状态机。

#### 一个必须说清的连带后果

既然**互斥**，那么设备在 BLE 通道上时**没有 WiFi 可以兜底**，语音就只能走 BLE。
所以 **ADPCM 压缩不再是"优化项"，而是 BLE 通道的必备项**。

建议：**两条通道都用 ADPCM**（而不是 WiFi 走裸 PCM、BLE 走 ADPCM）。
理由：设备端只维护一条音频路径，代码更少、测试更简单；延迟也让 daemon 统一解码。


---

## 3. 三种集成深度（决定工期与体验）

### Depth 1 — 一次性执行（最简，每家 1 天）
网关按请求启动 `dsh headless` / `codex exec` / `claude -p`，解析流式 JSON 推到设备。

- ✅ 一天能跑通，能看结果
- ❌ **无法中途审批**。headless 模式下要么自动拒绝，要么预先放开权限（`--dangerously-bypass-*` 属高危）
- 定位：只读的「任务执行器 + 结果查看器」

### Depth 2 — 会话守护进程（推荐起点，每家 2 天）
- Codex：`codex remote-control start` + `pair`，走 app-server
- DSH：`dsh web --host 0.0.0.0 --port 4336 --trusted-host <设备IP:端口>`
- Claude：`claude --bg` 拿 id，后续 `--resume <id>` 续

- ✅ 能拿会话状态、能续、能中断
- ⚠️ Codex / DSH 侧都标注实验性或非稳定契约，版本升级会破

### Depth 3 — 审批拦截（真正的杀手功能，Claude 2–3 天）
Claude Code 的 `PreToolUse` hook 里，把工具调用（命令、文件路径、diff 摘要）发给网关，
网关转给设备，**阻塞等待**设备按键，再把 `allow`/`deny` 回填给 hook。

```
Claude 要执行 rm -rf build/
  → PreToolUse hook (stdin: 工具名+参数)
  → 网关 /api/v1/approvals (id=42)
  → 设备刷新屏幕：「claude · myrepo 想执行 rm -rf build/ [确认=允许 返回=拒绝]」
  → 用户按确认
  → 网关回 {"permissionDecision":"allow"}
  → Claude 继续执行
```

- ✅ 这是唯一「人不在电脑前也能安全放行」的方案
- ⚠️ 阻断期间 CLI 会话是挂起的，要有超时与「默认拒绝」兜底
- Codex 侧对应的是审批事件 + `--sandbox` 策略；DSH 侧待实测

---

## 4. 设备端交互

### 4.1 按键

| 按键 | 有审批时 | 无审批时 |
|---|---|---|
| Confirm | **允许** | 语音对讲（按住说话） |
| Back | **拒绝** | 退出/返回 |
| Up / Down | 翻看详情（命令原文 / diff / 日志） | 切换任务 |
| Left / Right | 上一个 / 下一个待审批项 | 切换 agent（claude/codex/dsh） |
| 长按 Back | **中断当前任务** | — |
| Power | 系统电源（不改语义） | 同左 |

安全约束（必须实现）：
- **超时自动拒绝**（默认 60 s），绝不默认放行
- 危险动作强制二次确认：`rm -rf`、`git push --force`、`curl|sh`、写工作区外路径
- 「永远允许此类」默认**不提供**，避免误按造成长期授权

### 4.2 屏幕（一屏说清四件事）

```
┌──────────────────────────────────────────┐
│ claude · onepage-hub      [2 待审批]     │
├──────────────────────────────────────────┤
│ 任务: 修复录音采样率，跑通单测            │
│ 状态: 等待批准                            │
│                                          │
│ ▸ Bash: rm -rf build/                    │
│   /Users/me/proj/build                   │
│                                          │
│ 确认=允许    返回=拒绝    上下=详情       │
└──────────────────────────────────────────┘
```

刷新策略沿用现有约束：只做状态级刷新，不做动画；详情翻页用局部刷新。

### 4.3 通知
无扬声器、无振动马达 → 只能靠屏幕变化。因此：
- 设备**不常连网**，改为「会话期间保持长轮询」（见风险：功耗）
- 待审批出现时立刻半刷一次（~1.3 s），这是唯一的「提醒」

---

## 5. 协议（设备 ↔ 网关）

```
GET  /api/v1/state?agent=claude       当前快照：任务、状态、待审批数
GET  /api/v1/events?since=<cursor>    长轮询（25 s），返回增量事件
GET  /api/v1/approval/<id>            审批详情（命令原文、路径、diff）
POST /api/v1/decision                 {id, decision:"allow"|"deny"}
POST /api/v1/interrupt                {agent, session}
POST /api/v1/prompt                   {agent, text} 或 {agent, audio(base64)}
```

鉴权：`X-Device-Secret`（复用现有 `ObfuscationUtils` 存本地），网关只监听局域网地址。

---

## 6. 代码落地

### 主机侧（新建独立仓库，不进固件）
```
onepage-pager/
├── server/            HTTP + 长轮询 + 审批状态机
├── adapters/
│   ├── claude.js      -p / --resume / PreToolUse hook 回调
│   ├── codex.js       exec --json / remote-control pair
│   └── dsh.js         --profile headless / web /api
├── hooks/claude-pretooluse.js
└── config.json        device secret、仓库白名单、危险命令规则
```

### 固件侧（onepage-hub，复用度很高）
| 文件 | 作用 |
|---|---|
| `src/network/HttpUploader.h/.cpp` | POST 与 JSON body（补齐 `HttpDownloader` 只有 GET 的缺口） |
| `src/pager/PagerClient.h/.cpp` | `/state` `/events` `/decision` `/prompt` 封装 |
| `src/activities/pager/PagerActivity.h/.cpp` | 审批界面 + 状态牌 + 按键映射 |
| `src/pager/ApprovalGuard.h/.cpp` | 超时拒绝、危险命令二次确认（**设备侧也要有一道**） |
| `src/apps/AppRegistry.cpp` | 加一行 `AppId::Pager = 5` |

- 复用：`PdmMic`（已验证）、`WifiCredentialStore`、`QrUtils`（配对码上屏）、`JsonParser`、`ObfuscationUtils`
- 复用注意：录音的 `skipLoopDelay()` 全速锁逻辑要带过来，否则 CIC 抽取在 10 MHz 丢样

---

## 7. 分阶段

**P0 起只做 BLE 通道，WiFi 不在首版范围内。**

| 阶段 | 内容 | 验收 | 估时 |
|---|---|---|---|
| **P0-a** | `NusServer`（NimBLE 外设 + 自定义 GATT + 绑定） + `LineProtocol`（一行一 JSON） | PC 用 `bleak` 连上，能双向收发 JSON 行 | 2 天 |
| **P0-b** | ADPCM 编码 + 边录边传 + daemon 侧解码 | PC 收到完整语音并还原成可播放 WAV | 2 天 |
| **P0-c** | 通道选择页（BLE / WiFi 置灰） + BLE 自动重连 + 状态牌 UI | 断链后自动重连，屏幕正确显示连接状态 | 1–2 天 |
| **P1** | claude adapter + `PreToolUse` 审批闭环 | 设备按确认 → 命令真的执行；按返回 → 被拒绝 | 2–3 天 |
| **P2** | Codex adapter（`remote-control pair` + app-server） | 同样的审批流在 codex 上跑通 | 2 天 |
| **P3** | DSH adapter（插件形态优先，其次 `web /api`） | 三家的状态牌统一 | 2 天 |
| **P4** | 语音短指令打通到 `/prompt` | 按住说「跑测试」→ 任务被下发 | 1–2 天 |
| P5（可选） | **WiFi 通道**：TCP transport + 通道选单解除置灰 | 同一套 UI 在 WiFi 下工作 | 2–3 天 |

**建议先做 P0-a**，它是整条链路里唯一没有现成代码可抄的部分，且做完就能用桌面端脚本独立验证，
不需要等 agent 侧的适配器。

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| **误批准导致真实命令执行** | 删文件、force push | 超时默认拒绝；危险命令二次确认；设备侧与网关侧**双重**校验；不提供「永久允许」 |
| 网关暴露审批接口 | 局域网内谁都能批准 | `X-Device-Secret` + 仅监听局域网 + 仓库白名单；可选只绑 VPN 地址 |
| Codex `app-server` / `remote-control` 实验性 | 版本升级即破 | adapter 层隔离，锁版本，失败时降级到 `exec --json`（只读） |
| DSH `/api` 非稳定契约且带 trust fence | 设备请求被拒 / 升级后失效 | 先实测 `--trusted-host`；DSH adapter 单独降级路径 |
| Claude hook 阻塞期间会话挂起 | 用户没注意就卡住 | 60 s 超时拒绝 + 设备端醒目提示 + 网关可查询挂起时长 |
| 设备常连网耗电 | 1000 mAh 撑不住 | 只在「有活跃会话」时长轮询；无会话时断网，定时（如 60 s）短连一次拉状态 |
| 三家事件模型差异大 | 归一化成本高 | 只归一化 4 个字段：`agent / repo / status / pending_approval`，其余原样透传文本 |
| 语音难输入代码 | 体验差 | 语音定位为短指令；代码/长 prompt 建议在电脑上打字，设备只做审批 |

---

## 9. 需要你拍板

已定（本轮）：
- ~~传输~~ → **BLE 首发，WiFi 预留，不做 USB**
- ~~通道切换~~ → **进入应用时手动选，通道内部自动重连，不做自动回退**
- ~~语音走不走 BLE~~ → **走，用 ADPCM 4:1 + 边录边传**

仍待定：

1. **先接哪家？** 建议 Claude Code（hook 最稳，审批闭环最快出效果）。若你主要用 DSH，反过来也行，但 `/api` 要先实测。
2. **审批粒度**：只批准「执行命令」，还是也包括「改文件」「装依赖」「网络请求」？
3. **要不要保留上一个方案（对话 Agent）？** 两个方案在固件里可以共存成两个桌面应用。
4. **网关用 Node 还是 Python？** 建议 Python：m5-paper-buddy 的 daemon 是可直接对照的 Python 实现，且 `bleak`/`pyserial` 生态成熟。
5. **设备名怎么显示？** BLE 广播名建议 `OnePage-Pager`，配对时墨水屏同时显示 6 位 passkey。


---

## 参考

- 上一版方案：[`docs/agent-remote-plan.md`](./agent-remote-plan.md)（对话 Agent 方向）
- 录音基础：`lib/Mic/PdmMic.{h,cpp}`、`src/activities/recorder/RecorderActivity.{h,cpp}`
- 网络与配置：`src/network/HttpDownloader.{h,cpp}`、`lib/Serialization/ObfuscationUtils.{h,cpp}`
- 应用注册：`src/apps/AppRegistry.cpp`
- 本机实测命令：`claude --help`、`codex app-server --help`、`codex remote-control --help`、`dsh web --help`

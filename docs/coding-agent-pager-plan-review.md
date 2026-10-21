# 编码 Agent 遥控方案审核（DSH 专项）

> 审核对象：[`coding-agent-pager-plan.md`](./coding-agent-pager-plan.md) v0.1
> 审核环境：本机 DSH **0.2.1-rc.2**（桌面端），Windows；对照了 app.asar 内的核心源码与 CLI 帮助。
> 结论：**方向对，DSH 审批拦截不仅可行，而且是 DSH 的一等能力；但方案的 DSH 接入路径、传输顺序和“上位机”形态需要改写。**

---

## 1. 总结论

1. **审批拦截已证实可行**：DSH 的审批是一个 Cordis **waterfall 事件** `approval/request`，
   插件可以 `ctx.on('approval/request', ...)` 拦截、阻塞等待，并返回
   `allowed-once | rejected | cancelled | unavailable`。`@deepseek-ai/dsh-acp` 就是这么做的。
2. **但方案选的接入点不对**：`dsh web --trusted-host` 属于 **web profile**；
   用户实际在用的是 **desktop profile**，它被 Electron 独占、内嵌 HTTP 服务固定绑
   `127.0.0.1:0`（随机端口、仅回环）。局域网设备**到不了**这个服务，也拿不到 `/api`。
3. **BLE 首发与 DSH 插件存在运行时冲突**：DSH 插件是 Node；Windows 上可靠的 BLE 栈在
   Python/`bleak` 一侧。要么加 Python sidecar，要么把首版传输换成 WiFi HTTP。
4. **审批请求里没有命令原文**：`ApprovalRequestEvent` 只有 `toolName/callId/reason/signal`，
   命令、路径、diff 要去 `session/event` 的 `tool/call` 里按 `callId` 关联。
5. **桌面 GUI 本身也是审批 answerer**：插件和设备要跟 GUI 抢同一个 `approval/request`，
   谁先返回谁生效，另一个会挂到超时。必须先定“谁优先”。

---

## 2. 事实核对（方案 vs 实际）

| # | 方案里的说法 | 实际（DSH 0.2.1-rc.2） | 影响 |
|---|---|---|---|
| 1 | 本机 `dsh 0.1.1-rc.2` | `dsh --version` = **0.2.1-rc.2** | 接口有变化，需按 0.2.x 重测 |
| 2 | headless：只打印最终消息，无流式、无 session | `dsh headless --json` 输出 NDJSON 运行事件；`--session-id` 可续接已有 session | “Depth 1 只能只读”判断需更新 |
| 3 | DSH 走 `dsh web --host ... --trusted-host`，设备请求 `/api` | `dsh web` 确有 `--host/--port/--trusted-host/--tls-*`；但 **desktop profile** 启动参数是 `--host 127.0.0.1 --port 0`，且 `dsh --profile desktop` 被 Electron 独占 | 设备无法直连桌面端 `/api`；插件路由也在回环上 |
| 4 | `/api` 有 401 + browser-trust fence | 实测 `http://127.0.0.1:1753/` **和** `/api/*` 都返回 401；需要 GUI 的启动 token/会话 | 设备侧要额外实现 token，且只对 web profile 有意义 |
| 5 | 插件能拿到内部事件、审批可被插件拦截并阻塞 | **成立**：`ctx.on('approval/request', (req, next) => ...)`；`next()` 表示放行给下一个 answerer | 这是方案里最有价值的一条，应升级为 DSH 主线 |
| 6 | `dsh plugin --profile <name> add <pkg>` | 成立；插件进 `~/.dsh/profiles/<name>/node_modules`，desktop 由 Electron 加载 | 插件形态可落地 |
| 7 | 插件可注册 HTTP 路由 | 成立：`ctx.inject(['webServer'], ctx => ctx.webServer.register(...))`，桌宠插件即如此 | 但桌面端 webServer 仅回环；需要插件自建 LAN 服务 |
| 8 | 审批事件里有命令/路径 | 只有 `agent/toolName/callId?/reason?/displayReason?/signal?`；**没有 arguments** | 审批屏的命令原文要自行关联 `tool/call` |
| 9 | 本机 BLE 配置已够做外设 | 已核对 `sdkconfig.onepage`：`BT_NIMBLE_ROLE_PERIPHERAL=y`、`ROLE_CENTRAL=y`、`ATT_PREFERRED_MTU=256`、`50_FEATURE_SUPPORT=y`、`SOC_BLE_50_SUPPORTED=y` | 外设可行；但 central 也在编译，且翻页器开机可能已启动 central |
| 10 | AppId::Pager = 5 | 当前枚举：Reader 0 / Settings 1 / FileTransfer 2 / Recorder 3 / FileManager 4，Pager=**5** 正确 | 无需改 |
| 11 | `~/.dsh/storages/session_projcache.json` | 实际是目录 `session_projcache/` + `workspace.json`；session 在 `~/.dsh/sessions/<proj>/session-<id>/session.v4.jsonl.zstd` | 文档小错，不影响方案 |
| 12 | 空闲内部 SRAM 约 48 KB | 可信但需实测；当前固件静态 RAM 37.5%，翻页器 central 若开启会再占内部 RAM | P0 前应实测，不能只按 48 KB 设计 |

---

## 3. 已验证的好设计（保留）

- **fail-closed**：审批超时/无 answerer 一律拒绝，符合 DSH 的 `unavailable/rejected` 语义。
- **allowed-once**：DSH 本身只支持一次性授权，没有“永久允许”，与方案一致。
- **危险动作二次确认、设备侧与网关侧双重校验**：方向正确。
- **手动选通道、通道内自动重连**：比自动回退清晰。
- **ADPCM 4:1 + 边录边传**的算术合理（15 s PCM 480 KB → ADPCM 120 KB → 约 4 s）。
- **设备只做瘦终端**的判断正确：DSH/Codex/Claude Code 都跑在主机上。

---

## 4. 必须修正的 6 点

### P0-1 接入点：不要走桌面端 `/api`，插件要自建 LAN 服务

- 桌面端 DSH 的 webServer 固定 `127.0.0.1:0`（见 `dsh-desktop-host` 的 `runProfile` 参数），
  设备在局域网里访问不到；`--trusted-host` 是 `dsh web` 的旗标，不是桌面 App 的。
- **推荐**：插件在 Electron host 进程内用 `node:http` 自建监听（例如 `0.0.0.0:8765`），
  自己实现 `X-Device-Secret`、来源 IP 白名单、只暴露 pager 路由。
- `dsh web` 只用于“专用常驻主机”形态（另一台机器/另一个进程），不能指望它看到桌面 App 里
  正在跑的实时 session。

### P0-2 传输顺序：DSH 首版用 WiFi HTTP，BLE 往后放

- DSH 插件是 Node；Windows 上没有成熟的 Node BLE central/peripheral 栈，方案里验证用的
  `bleak` 是 Python。两者之间要么加 sidecar + IPC，要么换传输。
- 设备端 WiFi/HTTP 基础设施已存在，插件端 `node:http` 是零依赖。**先用 WiFi HTTP 把
  审批闭环跑通**，再决定是否为产品形态补 BLE sidecar。
- BLE 的价值（低功耗、免配网）仍在，但不应成为验证“审批”这一核心价值的前置条件。

### P0-3 审批抢占：GUI 和插件会同时应答

- `dsh-client-ui-approval` 通过 `ctx.remote.$on('approval/request')` 注册了 answerer；
  打开桌面 GUI 时，它和 pager 插件都会收到请求。
- Cordis waterfall 是“先返回者获胜”，另一个会一直等到超时/取消。轻则重复弹窗，重则设备
  批准了但 GUI 已经拒绝/反之。
- **必须先定策略**：
  - A. 设备优先：插件先应答，GUI 的请求被“已处理”状态覆盖（需要插件能通知 UI 的能力，或接受 GUI 面板悬挂）；
  - B. GUI 优先：插件只在 `--pager` 远程模式/无人值守时接管；
  - C. 同一时刻只允许一个 answerer：插件检测 GUI 客户端是否连接，决定是否 `next()`。
- 建议首版用 **B + 明确开关**（设置里“设备接管审批”），避免和 GUI 抢。

### P1-1 审批详情：按 callId 关联 tool/call

- `approval/request` 没有命令参数。插件需订阅 `session/event`：
  - `tool/call` → 记录 `{callId, name, arguments}`；
  - `approval/asked` → 用 `callId` 找到参数，拼成设备卡片（命令、路径、diff 摘要）。
- 设备侧只显示插件给的结构化字段，不要在 MCU 上解析原始 JSON 参数之外的东西。

### P1-2 形态：DSH 用插件，不要外挂 `onepage-pager/adapters/dsh.js`

- 方案 §6 把 DSH 放在独立 daemon 的 adapter 里；但只有**进程内插件**才能拦截桌面 App
  的实时审批。
- 正确拆法：
  - `dsh-onepage-pager`（Node 插件，进 desktop profile）：审批 answerer + 状态事件 + LAN HTTP；
  - `onepage-pager`（可选，独立仓库）：只服务 Claude/Codex，或作为 `dsh web` 专用主机的外壳。
- 两边的设备协议保持一致（`/state`、`/events`、`/decision`）。

### P2 版本与 headless 认知更新

- 方案基于 0.1.1-rc.2；当前 0.2.1-rc.2。`headless --json`/`--session-id` 已存在，
  “Depth 1 无流式/无 session”要改写。
- headless + pager 插件也可以工作（插件注册 answerer），适合 CI/无人值守场景；
  但 headless 没有 GUI，反而没有“抢审批”的问题，是很好的验证环境。

---

## 5. 推荐的 DSH-first 架构

```text
DSH 桌面 App（Electron host, profile=desktop）
└── 插件 dsh-onepage-pager（Node，随 App 启动）
    ├── ctx.on('approval/request')      审批 answerer（超时→rejected，fail closed）
    ├── ctx.on('session/event')         状态 + tool/call 参数关联
    ├── node:http  0.0.0.0:8765         设备专用 LAN 服务（X-Device-Secret）
    └── 路由：/state /events /approval/:id /decision /interrupt /prompt
                         ▲
                         │ WiFi HTTP（首版）
                  OnePage PagerActivity
                         │
                  （后续）BLE NUS + ADPCM，同一套 line-JSON
```

### 审批闭环（关键路径）

1. Agent 触发审批 → DSH 发 `approval/request`。
2. 插件检查策略/开关：不接管则 `next()` 交给 GUI；接管则进入下一步。
3. 插件用 `callId` 从 `tool/call` 取出命令/路径/diff，生成审批卡片。
4. 通过 LAN HTTP 推给设备（或设备长轮询取），设备显示并等待按键。
5. 设备 `POST /decision` → 插件返回 `allowed-once` 或 `rejected`。
6. 60 s 无响应 / 断链 → `rejected`（或 `unavailable`），绝不默认放行。
7. 若请求的 `signal` 已 abort → 返回 `cancelled`。

### 设备端

- 首版：`PagerActivity` + WiFi HTTP 客户端（复用 `WifiCredentialStore`、`JsonParser`、
  `HttpDownloader` 的 TLS/超时经验；新增 POST）。
- 设备侧二次校验：`rm -rf`、`git push --force`、写工作区外路径 → 强制二次确认。
- 录音/语音作为 P4，不作为首版依赖；先证明“审批”这一杀手功能。

### 插件安装

```powershell
dsh plugin --profile desktop add "C:\path\dsh-onepage-pager-0.1.0.tgz"
# 重启桌面 App（desktop profile 由 Electron 独占，不能 dsh --profile desktop 启动）
```

---

## 6. 修订后的阶段（DSH 优先）

| 阶段 | 内容 | 验收 | 估时 |
|---|---|---|---|
| **P0** | `dsh-onepage-pager` 插件骨架：LAN HTTP + `/state` + `approval/request` 控制台应答 + 超时拒绝 | PC 上 curl 能看到审批、能 allow/deny，DSH 真实执行/拒绝 | 1–2 天 |
| **P1** | 设备 `PagerActivity`（WiFi HTTP）：审批卡片、tool/call 详情、确认/返回、60 s 超时 | 设备按键 → DSH 命令执行/拒绝 | 2–3 天 |
| **P2** | 状态牌 + 多 session/agent + GUI 抢占策略（设备接管开关） | 打开 GUI 时行为可预期 | 1–2 天 |
| **P3** | BLE NUS + ADPCM（Python sidecar 或 Node BLE），同一 line-JSON | 断 WiFi 也能审批 | 2–3 天 |
| **P4** | 语音短指令 `/prompt`、Claude/Codex adapter | 按需 | 2–4 天 |

> 与方案的差别：把“BLE P0-a”换成“插件+WiFi P0”，把 DSH adapter 从独立 daemon 提前为进程内插件，
> 把 Claude-first 改为 DSH-first。

---

## 7. 需要你拍板

1. **部署形态**：pager 插件进桌面 App（推荐，能看实时 session），还是单独跑 `dsh web` 常驻主机？
2. **首版传输**：WiFi HTTP（推荐，先验证审批）还是坚持 BLE 首发（需接受 Python sidecar）？
3. **GUI 抢占**：设备接管审批时，GUI 是让位（插件优先）还是继续弹（GUI 优先）？
4. **审批范围**：只 Bash/命令，还是也包含写文件、装依赖、网络请求？
5. **超时语义**：60 s 后 `rejected` 还是 `unavailable`？（建议 `rejected`，用户能看到“已拒绝”）
6. **设备密钥**：`X-Device-Secret` 首次怎么下发（二维码/手输/配置文件）？

---

## 8. 审核结论

- 方案的**产品判断**（寻呼机+审批器+状态牌）和**安全设计**（fail-closed、allowed-once、
  双重校验）都成立，可以保留。
- 方案的**DSH 接入**部分约 60% 可复用：审批 waterfall 的判断是对的，但接入点、传输顺序、
  daemon 形态、审批详情、GUI 抢占这五处必须改。
- **审批拦截是真实可做的**，而且比方案原以为的更干净（不需要 `/api` token/trust fence）。
- 建议按第 6 节的 P0 先做插件 + curl 验证，再上设备；这样 1–2 天就能确认“设备按键真的能
  放行/拒绝 DSH 的命令”，而不是先花一周做 BLE。

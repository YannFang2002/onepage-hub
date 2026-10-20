# OnePage 麦克风 / 语音助手方案（草案 v0.1）

> 状态：方案评审中，未开始实现。
> 目标：在 onepage-hub（Arduino/PlatformIO, ESP32-C61）上加入 PDM 麦克风采集，
> 接入 `@movecall/onepage-agent-channel` 的语音 Agent 通道。

---

## 0. 结论摘要

1. **硬件有麦克风，固件没有驱动**：PDM MIC，`CLK=GPIO7`、`DIN=GPIO3`，供电与 SD/EPD 共用 `GPIO27`
   （现有代码只定义了引脚，并在深睡前把时钟拉低、数据脚置 Hi-Z）。
2. **C61 没有硬件 PDM2PCM**，必须走 I2S RAW PDM + 软件 CIC 抽取。官方 BSP
   `MoveCall/bsp_onepage_c61` 的 `board_mic.c` 已在同款板子上实现并调好参数，直接移植即可。
3. **Agent 通道的音频契约**：16 kHz 单声道 16-bit PCM WAV，`POST /api/onepage/voice`，
   ≤1 MB，带 `X-Device-Id` / `X-Device-Secret`；服务端 ASR 后立即返回 `pending=true`，
   设备再 `GET /poll` 取 Agent 回复。
4. **最大风险**：BSP 头文件明确注明其 PDM RX 驱动要求 **ESP-IDF ≥ 6.1-dev**，并指出 5.5.x
   的 PDM RX 驱动有 bug；本工程当前是 **IDF 5.5.2**。阶段 0 必须先做驱动自检，再谈上层。
5. 建议按 5 个阶段推进：驱动自检 → 录音/WAV → 配对/上传/ASR → 轮询/渲染 → 产品化。

---

## 1. 调研结论

### 1.1 npm 包 `@movecall/onepage-agent-channel@0.2.1`

- 定位：Hermes / OpenClaw 的 **安装器 + 平台适配器**，不是固件包。
- 内容：`channels/hermes/adapter.py`（57 KB）、`channels/openclaw/adapter.js`（36 KB）、
  `installer/bin/onepage-channel.js`、协议文档。
- README 声称仓库里有 `firmware/`，但 npm tarball 未包含；GitHub 仓库
  `movecall/onepage-agent-channel` 返回 404（私有或改名），所以固件侧只能参考 BSP。
- 服务端流程：HTTP :8765 → ASR（DashScope `fun-asr-flash-2026-06-15`）→
  Hermes/OpenClaw Agent → JSON 回复（文本 / Markdown / QR / 1-bit 位图）。
- 设备端 API：

| 接口 | 方法 | 说明 |
|---|---|---|
| `/api/onepage/health` | GET | 健康检查 |
| `/api/onepage/hello?device_id=...` | GET | 配对；未配对返回 8 位配对码，已配对返回 `device_secret` |
| `/api/onepage/voice` | POST | 上传音频；需 `X-Device-Id` + `X-Device-Secret`；`Content-Type: audio/wav`；≤1 MB |
| `/api/onepage/poll?device_id=...` | GET | 取 Agent 异步回复（outbox） |
| `/api/onepage/media/<id>` | GET | 取 1-bit 位图（424×700，与本机 800×480 不匹配，暂不做） |

- `/voice` 返回：`user_text`、`reply`、`pending`、`turn`、`history`。
  识别成功时 `pending=true`、`reply=""`，随后轮询 `/poll` 拿 `reply`。
- 鉴权分层：可选全局 Bearer token → 设备配对 → `device_secret`（HMAC 存储）。

### 1.2 官方 BSP 的麦克风实现（`MoveCall/bsp_onepage_c61`）

文件：`board_mic.c`（2026-08-12 首版，至今未改）。核心参数：

| 项 | 值 |
|---|---|
| 引脚 | `PDM_CLK=GPIO7`、`PDM_DIN=GPIO3` |
| 过采样时钟 | 2.048 MHz |
| 原始数据 | 16-bit word，**成对重复**，取 stride=2 去重 |
| 抽取 | 3 阶 CIC，R=128 → 16 kHz PCM |
| 后处理 | DC blocker → 过驱动增益 120000 → 1 阶低通（α=11/16）→ 软限幅 |
| DMA | `dma_desc_num=8`、`dma_frame_num=640`（约 10 KB，约 20 ms） |
| 单次读取 | `RAW_READ_WORDS=2048`（短解码间隙，防 DMA 饿死） |
| API | `board_mic_init/start/read/stop`，`BOARD_MIC_SAMPLE_RATE=16000` |

BSP 注释里两条硬约束：

1. **不能丢样本**：一旦丢样就会出现“音调变快 / 男声 / 嘶嘶声”；采集必须与 SD 写入解耦。
2. **DMA 不能开太大**：32 KB 曾把 Wi-Fi 的内部 RAM 挤爆，10 KB 是实测可用值。

### 1.3 当前工程状态（onepage-hub）

- 麦克风：只有 `PDM_CLK/PDM_DIN` 宏和深睡静默，**没有 I2S/PDM 初始化、没有采集任务**。
- 可复用：
  - Wi-Fi：`WifiCredentialStore` + `WiFi.begin()`（活动里已有“连接 Wi-Fi”子流程）。
  - HTTP：`esp_http_client`（`HttpDownloader` 已封装超时/CA/重定向，需补 POST）。
  - JSON：ArduinoJson 7（已在依赖里）。
  - PSRAM：`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 已有使用先例。
  - UI：Activity 生命周期、GfxRenderer、UITheme、i18n、桌面 AppRegistry。
- 工具链：pioarduino `55.03.37` = ESP-IDF **5.5.2** + Arduino 3.3.7。
  可选的邻近版本：
  - `55.03.312` = ESP-IDF **5.5.5** + Arduino 3.3.12
  - `61.04.00-RC1` = ESP-IDF **6.1** + Arduino 4.0.0-RC1（RC，2026-09-23）

### 1.4 关键风险：IDF 5.5.x 的 PDM RX

- BSP `include/board.h` 原话：*“C61 has no hardware PDM2PCM; requires ESP-IDF >= 6.1-dev (5.5.x PDM RX driver bug).”*
- 实测对照：5.5.2 / 5.5.4 / 5.5.5 的 `components/esp_driver_i2s/i2s_pdm.c` **字节完全相同**
  （SHA256 一致），说明 5.5.x 分支没有针对该问题的修复，**升 5.5.5 预计无效**。
- 因此阶段 0 必须先验证；若失败，按以下顺序兜底：
  1. 升级到 `61.04.00-RC1`（IDF 6.1，与 BSP 对齐）——改动大、RC 风险，需完整回归；
  2. 在 6.x 分支定位 PDM RX 修复并 backport 到本地 `framework-espidf`，
     用项目已有的 `scripts/patch_*.py` 预构建机制打补丁；
  3. 仅把麦克风采集做成独立的 ESP-IDF 6.1 验证工程，确认硬件后再决定主线怎么走。

---

## 2. 总体设计

```text
┌──────────────────────────────────────────────────────────────┐
│ VoiceActivity（桌面 App）                                    │
│  状态机：IDLE → RECORDING → UPLOADING → THINKING → REPLY/ERR │
└───────────────┬───────────────────────────┬──────────────────┘
                │                           │
        ┌───────▼────────┐          ┌───────▼─────────────┐
        │ PdmMic + WavBuf│          │ VoiceClient         │
        │ I2S PDM RX     │          │ /hello /voice /poll │
        │ CIC3 → 16k PCM │          │ JSON + secret       │
        └───────┬────────┘          └───────┬─────────────┘
                │                           │
        ┌───────▼───────────────────────────▼─────────────┐
        │ 现有基础设施：Wi-Fi / esp_http_client / NVS     │
        └─────────────────────────────────────────────────┘
```

### 2.1 关键设计决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 音频格式 | 16 kHz mono 16-bit PCM WAV | Hermes 只保证 WAV；OpenClaw 虽支持 IMA ADPCM，但 v1 统一 PCM 最稳 |
| 缓冲 | PSRAM 480 KB（15 s） | 2 MB PSRAM 足够；不阻塞 SD；失败可回退 SD 文件 |
| 录音时长 | 默认 15 s，上限 30 s | 32 KB/s；1 MB 上限约 32.7 s，15 s 留足余量 |
| 上传方式 | 录完一次性 POST | 实现简单、WAV 头完整；流式上传留作后续优化 |
| 触发 | 桌面新增「语音助手」应用，确认键开始/停止 | 不动现有按键语义；后续可加长按说话 |
| 显示 | 只在状态切换时刷新 | 墨水屏刷新慢，秒级计时会狂闪 |
| 配置 | `/system/.crosspoint/voice.json`（url/device_id/secret） | 不污染读者设置；后续在 Web 设置页暴露 |

---

## 3. 阶段 0：驱动 bring-up（最高优先级）

### 3.1 交付物

- `lib/Mic/PdmMic.h/.cpp`：从 BSP `board_mic.c` 移植，保留全部参数。
  - `bool begin()` → `i2s_new_channel` + `i2s_channel_init_pdm_rx_mode`（RAW slot）
  - `bool start()/stop()`
  - `int read(int16_t* pcm, size_t maxSamples, uint32_t timeoutMs)`
- 一个临时自检入口（串口命令或隐藏页面）：录 5 s，写 `/voice_test.wav` 到 SD。

### 3.2 移植要点

- Arduino 工程直接包含 IDF 头：`driver/i2s_pdm.h`、`driver/i2s_common.h`。
- BSP 是 C 代码，C++ 下嵌套指定初始化器需要改写为逐字段赋值（C++20 限制）。
- 保留：2.048 MHz / stride 2 / CIC R=128 / gain 120000 / LPF 11/16 / 软限幅 /
  DMA 8×640 / `RAW_READ_WORDS=2048`。
- 采集任务：优先级 5、栈 4 KB、`i2s_channel_read` 超时 200 ms；采集期间**不写 SD**。

### 3.3 验收标准（Go/No-Go）

1. 5 s 录音样本数 ≈ 80,000（±1%），无整段丢失。
2. PC 上播放清晰、无变调/男声/爆音；440 Hz 参考音频率正确。
3. 连续 10 次录音，PSRAM/heap 无泄漏，Wi-Fi 仍能正常连接。
4. 深睡唤醒后再次录音正常。

> 若第 1/2 条失败 → 触发 1.4 的兜底路线，先解决工具链/驱动，不进入上层开发。

---

## 4. 录音与缓冲

- 缓冲：`heap_caps_malloc(15 * 16000 * 2 + 44, MALLOC_CAP_SPIRAM)`（约 480 KB），
  失败则提示并回退到 SD 临时文件（录完再读）。
- WAV 头：44 字节 RIFF/PCM，采集结束后回填 `RIFF`/`data` 长度。
- 采集结束条件：再次按确认键 / 达到 15 s / 缓冲满。
- 最短时长 0.5 s；结束后计算峰值与 RMS，过低直接提示“没有检测到声音”，不上传。
- 每次录音前 `start()` 复位 CIC/DC/LPF 状态（BSP 的 `decode_reset`）。

---

## 5. 网络与协议

### 5.1 设备身份与配对

- `device_id`：`ONEPAGE_` + MAC 12 位十六进制（如 `ONEPAGE_8856A6CFD928`），首次生成后存 NVS/JSON。
- 首次使用：`GET /hello?device_id=...`
  - `paired=false` → 屏幕大字显示 `pairing_code` 与 `approve_command`；
    每 3 s 重新 `/hello`，直到 `paired=true` 并拿到 `device_secret`。
  - `device_secret` 存 `/system/.crosspoint/voice.json`（base64 混淆，复用现有 ObfuscationUtils）。
- 后续请求带 `X-Device-Secret`；收到 `403 device_secret_required` 则重新配对。

### 5.2 上传

- `POST {url}`，头：`Content-Type: audio/wav`、`X-Device-Id`、`X-Device-Secret`、
  `Content-Length`；body 为 WAV（头 + PSRAM 数据）。
- 用 `esp_http_client_open(client, contentLength)` + `esp_http_client_write()` 分块发送；
  HTTPS 走 CA bundle，局域网 HTTP 直接连。
- 超时 30 s；失败重试 1 次（仅网络错误，不重发 4xx）。

### 5.3 轮询

- 解析 `pending`；为 `true` 时每 800 ms `GET /poll`，总超时 30 s。
- 解析 `reply` / `messages`；空轮询继续；超时提示“Agent 未响应”。
- `reply` 中的 `QR:<payload>` 行：阶段 3 先按普通文本显示，阶段 5 再接二维码渲染。

### 5.4 错误映射

| 情况 | 处理 |
|---|---|
| 403 `device_not_paired` | 进入配对页，显示 pairing_code |
| 403 `device_secret_required` | 清除本地 secret，重新配对 |
| 413 `audio_too_large` | 缩短录音时长后重试 |
| 401 | 提示检查全局 token |
| 网络失败/超时 | 提示检查 Wi-Fi/服务器，可重试 |

### 5.5 配置

- `/system/.crosspoint/voice.json`：`url`、`deviceId`、`deviceSecret`。
- 默认 url 为空 → 首次进入语音页提示“请先在 Web 设置里配置语音服务器地址”。
- 后续在 Web 设置页增加“语音助手”卡片（复用现有 settings 页面生成逻辑）。

---

## 6. UI / UX

- 桌面新增第 4 个应用「语音助手」（`AppRegistry` 加一行 + 图标）。
- 页面状态：
  1. `READY`：提示“按确认开始说话”，显示服务器/配对状态。
  2. `RECORDING`：静态提示“录音中… 再按确认结束”，不刷秒表；15 s 自动结束。
  3. `UPLOADING`：“正在识别…”。
  4. `THINKING`：显示 `user_text` + “Agent 思考中…”。
  5. `REPLY`：渲染 `reply`（简单换行/分页，复用现有文本绘制）。
  6. `ERROR`：错误原因 + 重试/返回。
- 新增 i18n 键（`STR_VOICE_*`），至少补英文/简体/繁体，其余语言回落英文。
- 录音期间禁止 EPD 复位/深度刷新，避免共享电源轨掉电。

---

## 7. 资源与性能预估

| 项 | 预估 |
|---|---|
| PSRAM | 480 KB 缓冲（15 s） |
| 内部 RAM | I2S DMA ≈10 KB + 任务栈 4 KB + HTTP 缓冲 ≈6 KB |
| CPU | CIC 解码约 1 M bit 迭代/s，160 MHz 单核下 <5% |
| Flash | 驱动 + 活动 + 文案 ≈ 10–15 KB |
| 录音功耗 | 麦克风 + CPU，预计几 mA 量级，远小于 Wi-Fi TX |
| 端到端延迟 | 局域网：录音 15 s + 上传 1–3 s + ASR 1–2 s + Agent 2–10 s |

---

## 8. 分阶段计划

| 阶段 | 内容 | 产出 | 验收 | 预估 |
|---|---|---|---|---|
| P0 | PDM 驱动移植 + 5 s 自检录音 | `lib/Mic`、`/voice_test.wav` | 第 3.3 节 4 条 | 1–2 天 |
| P1 | 录音缓冲/WAV/时长/静音检测 | 录音模块 | PC 可播放、样本数正确 | 1 天 |
| P2 | 配对 + 上传 + ASR 文本显示 | `VoiceClient`、配对页 | 说话后屏幕出现 `user_text` | 1–2 天 |
| P3 | `/poll` + 回复渲染 | 对话页 | Agent 回复上屏 | 1 天 |
| P4 | 桌面 App、配置、i18n、错误态 | 可发布功能 | 断网/未配对/超时均有提示 | 1 天 |
| P5 | 可选：QR、图片、流式上传、ADPCM | — | 按需 | — |

---

## 9. 风险清单

| 风险 | 影响 | 对策 |
|---|---|---|
| **IDF 5.5.2 PDM RX bug** | 采不到正确数据/根本起不来 | P0 先验证；升级 IDF 6.1 或 backport 修复 |
| 丢样本 | 变调、嘶嘶声 | 保留 BSP DMA/解码参数；采集与 SD 写解耦 |
| PSRAM 分配失败 | 无法录音 | 失败回退 SD 文件；提示用户 |
| Wi-Fi 与 I2S 共存 | 录音时断网/丢包 | DMA 保持 10 KB；先录后传 |
| GPIO27 共享电源轨 | 录音中 EPD 复位导致 MIC/SD 掉电 | 录音期间禁止 EPD reset；深睡前先停 MIC |
| 音频格式不被 ASR 接受 | 识别失败 | v1 严格 PCM WAV 16k mono；不冒险用 ADPCM |
| e-ink 刷新慢 | 状态反馈迟钝 | 只做状态级刷新，不做秒表/波形 |
| 明文 HTTP + secret | 局域网被嗅探 | 文档注明；后续支持 https/全局 token |
| 服务器 424×700 图片契约 | 图片显示尺寸不符 | 图片功能暂缓，文本/QR 优先 |

---

## 10. 待确认问题

1. 触发方式：桌面 App + 确认键切换（推荐），还是长按某个键说话？
2. 服务器地址：先 Web 设置页配置，还是固件里给一个默认地址？
3. 录音时长 15 s 是否够用？是否需要 30 s？
4. 板子是否有扬声器（TTS 播放）？目前方案只做“说”，不做“听”。
5. 是否接受为麦克风把工具链升级到 IDF 6.1（Arduino 4.0.0-RC1）？
6. 图片/二维码是否在首版范围内？

---

## 附：关键代码骨架（示意）

```cpp
// lib/Mic/PdmMic.h
class PdmMic {
 public:
  bool begin();
  bool start();
  bool stop();
  int read(int16_t* pcm, size_t maxSamples, uint32_t timeoutMs);
 private:
  i2s_chan_handle_t rx_ = nullptr;
  // CIC / DC / LPF 状态
};

// 录音结束
size_t buildWavHeader(uint8_t* out, uint32_t pcmBytes);
// POST
bool voicePost(const std::string& url, const std::string& deviceId,
               const std::string& secret, const uint8_t* wav, size_t len,
               std::string& responseJson);
```

---

### 参考

- npm：`@movecall/onepage-agent-channel@0.2.1`（README、`docs/usage_flow_simulation.md`、
  `channels/hermes/adapter.py`、`channels/openclaw/adapter.js`）
- BSP：<https://github.com/MoveCall/bsp_onepage_c61>（`board_mic.c`、`include/board.h`）
- 本工程：`lib/hal/HalGPIO.{h,cpp}`、`src/network/HttpDownloader.*`、`src/apps/AppRegistry.cpp`

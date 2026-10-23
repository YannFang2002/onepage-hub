# OnePage OTA 方案

## 0. 结论先行

底子已经齐了：双 OTA 分区、`otadata`、回滚保护、`FirmwareFlasher`、`HttpDownloader`、
SD 卡升级页全都在仓库里。**OTA 需要新增的只有两件事**：

1. 设备端一个「检查更新 / 下载 / 写入」的入口页；
2. 主机桥两个只读接口，让设备能拿到镜像。

预计：桥 ~80 行，固件 ~250 行，一个 UI 页。

---

## 1. 现状盘点

### 1.1 分区（已就绪，不需要重新分区）

```text
nvs        0x9000    20 KB
otadata    0xe000     8 KB
app0      0x10000   7.94 MB   ← 当前运行
app1     0x800000   7.94 MB   ← 备用槽
coredump 0xFF0000    64 KB
```

- 16 MB flash，两个 app 槽各 7.94 MB；
- 当前固件 5.45 MB，占单槽 **69%**，余量充足；
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（回滚已开）。

### 1.2 已有代码

| 组件 | 作用 |
|---|---|
| `src/network/FirmwareFlasher.h` | `validateImageFile()` 校验镜像；`flashFromSdPath()` 写入下一槽并切 `otadata` |
| `src/network/HttpDownloader.h` | `downloadToFile(url, dest, progress)` 流式下载，支持 http/https |
| `src/activities/settings/SdFirmwareUpdateActivity` | SD 卡升级 UI（已可用） |
| `src/system/OtaBootSwitch.cpp` | 直接改 `otadata` 选槽 |

### 1.3 缺口

- 没有任何地方调用 `esp_ota_mark_app_valid_cancel_rollback()`：
  开了 `ROLLBACK_ENABLE` 时，新固件以 `PENDING_VERIFY` 启动，**不标记有效就会被 bootloader 回滚**。
  这一条必须先补，否则 OTA 升完重启就回到旧版本。
- 没有版本比较逻辑（`esp_app_get_description()->version` 可读 `onepage.version`）。

---

## 2. 入口对比

先说清网页端的现状（这是本次确认的重点）：

- 设备自带 web UI：`HomePage / FilesPage / FontsPage / SettingsPage`（`src/network/html/`）；
- `/upload` 接收任意文件，**没有扩展名白名单**，书籍/字体都走它，几十 MB 没问题；
- 但**没有任何固件/OTA 路由**，`CrossPointWebServer.cpp` 里只有文件、设置、字体、OPDS、WiFi；
- 设备端已有 `SdFirmwareUpdateActivity`：文件浏览器（只列 `.bin`）→ `validateImageFile` → `flashFromSdPath` → 重启。

所以「网页上传 + 设备端点一下」这条路**今天就能用，零开发**。

| 方案 | 链路 | 优点 | 缺点 |
|---|---|---|---|
| **A. SD 卡** | 拷 `.bin` 到 SD → 设备上调升级页 | 已可用 | 要拔插卡 |
| **D. 网页上传（新，最省事）** | 浏览器上传 `.bin` → 设备 OTA 槽 | 复用现有 web UI 与上传通道；局域网即可 | 需设备 web 服务在跑（当前只在「网页传书」里启动） |
| **B. 局域网桥** | 设备向主机桥拉取镜像 | 可一键推、可批量、可自动检查版本 | 需要主机常开 |
| **C. 云端直连** | 设备 → GitHub Release | 无需主机 | 国内直连不稳，证书包占空间 |

推荐路线：**先做 D（半天），再补 B（一天）**；C 不做。A 作为兜底一直保留。


---

## 3. 方案 D：网页上传 OTA（推荐先做）

```text
浏览器  ──POST /api/ota (multipart)──▶  设备 web server
                                        │ ① 流式校验镜像头
                                        │ ② 边收边写下一 OTA 槽
                                        │ ③ 全部成功 → 切 otadata
                                        ▼
                                     自动重启进入新固件
```

### 3.1 设备端改动

1. `CrossPointWebServer` 增加路由：

```cpp
server->on("/api/ota", HTTP_POST,
           [this] { handleOtaPost(); },     // 结束后回状态
           [this] { handleOtaUpload(); });  // 流式分片
```

2. 上传流不落 SD 也可以直接写 flash（省一次 5.4 MB 写卡），但**先落 SD 再校验**更稳：
   与现有 `flashFromSdPath` 完全复用，失败时 SD 上的残片无副作用。

```cpp
// 落盘到 SD 暂存
/system/.crosspoint/ota/firmware.bin

// 收完再统一校验 + 写入
firmware_flash::validateImageFile(path, slotSize);
firmware_flash::flashFromSdPath(path, progressCb, this, true);
```

3. `HomePage.html` 增加「固件升级」卡片：文件选择 + 进度条 + 版本对比
   （读 `/api/status` 里的 `version`，与上传的 bin 里 `esp_app_desc_t.version` 比较）。

4. web 服务当前只在 `CalibreConnectActivity`（网页传书）里启动。
   OTA 需要一个常驻开关，建议：设置里加「网页服务」独立项，或在「固件升级」页启动服务并显示访问地址。

### 3.2 版本号

统一用 `onepage.version`（`platformio.ini`，当前 `1.4.1`），编译期通过 `CROSSPOINT_VERSION`
注入 `esp_app_desc_t.version`，设备与网页都读它，不引入第二套版本规则。

---

## 4. 方案 B：局域网桥拉取（第二阶段）

```text
┌──────────┐   ①GET /api/v1/ota/latest      ┌──────────────┐
│  OnePage │ ─────────────────────────────▶ │ 主机桥        │
│  设备     │ ◀─── {version,size,sha256} ─── │ (server.py)  │
│          │                                │              │
│          │   ②GET /api/v1/ota/image       │ 缓存:         │
│          │ ─────────────────────────────▶ │ ota/*.bin    │
│          │ ◀──── 5.4 MB 流式 ──────────── │              │
└──────────┘                                └──────┬───────┘
       │                                           │
       │ ③落盘 SD → 校验 → 写 app1 → 切 otadata      │ 可选：从
       ▼                                           │ GitHub 拉取
   ④重启进入新固件 → ⑤标记 valid                     │
```

### 4.1 桥端接口（`bridge/server.py`）

```http
GET /api/v1/ota/latest
→ 200 {"ok":true,"version":"1.4.2","size":5712320,
        "sha256":"ab12…","url":"/api/v1/ota/image",
        "built_at":"2026-10-23T09:00:00Z"}

GET /api/v1/ota/image
→ 200 application/octet-stream   （支持 Range，便于断点续传）
```

实现要点：

- 镜像来源二选一，按配置决定：
  - `ota.dir`：本地目录（开发期直接把 `firmware.bin` 丢进去）；
  - `ota.repo` + `ota.token`：GitHub Release，桥拉取后**缓存 10 分钟**，设备永远只连局域网；
- `sha256` 在桥启动/文件变化时计算并缓存；
- 仅允许局域网访问（复用现有 `isLocalRequest` 的网段判断，但**放开到私网**，不要 loopback-only，否则设备访问不到）。

### 4.2 设备端流程

```
菜单 →「固件升级」
  ① GET /api/v1/ota/latest
     - 与 esp_app_get_description()->version 比较，相同则提示「已是最新」
     - 电量 <20% 且未接 USB → 拒绝并提示
  ② 下载到 SD：/system/.crosspoint/ota/firmware.bin
     - HttpDownloader::downloadToFile(url, path, progress)
     - 显示百分比 + 已下载/总量
  ③ 校验
     - 大小一致
     - sha256 一致（mbedtls_sha256 流式计算）
     - firmware_flash::validateImageFile(path, slotSize)
  ④ 写入
     - firmware_flash::flashFromSdPath(path, progress, ctx, /*alreadyValidated=*/true)
  ⑤ ESP.restart()
```

## 5. 回滚与自证（两种方案共用）

```cpp
// 在启动流程末尾（首帧渲染之后、进入主界面之前）
const esp_partition_t* running = esp_ota_get_running_partition();
esp_ota_img_states_t state;
if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
    state == ESP_OTA_IMG_PENDING_VERIFY) {
  esp_ota_mark_app_valid_cancel_rollback();
}
```

若要更保守，可延后到「WiFi 连上 + 主界面渲染成功」之后再标记，让启动即崩溃的镜像自动回滚。

---

## 6. 失败矩阵

| 阶段失败 | 结果 | 设备状态 |
|---|---|---|
| 检查更新失败 | 提示网络错误 | 旧固件正常运行 |
| 下载中断 | 缓存文件残缺，未写 flash | 旧固件正常运行 |
| sha256 / 镜像校验失败 | 不切换 `otadata` | 旧固件正常运行 |
| 写入中掉电 | app1 残缺，`otadata` 仍指向 app0 | 重启回旧固件 |
| 新固件启动崩溃 | bootloader 见 `PENDING_VERIFY` 未确认 | **自动回滚旧固件** |
| 新固件正常 | 标记 valid | 下次正常启动 |

关键点：`otadata` 只在写入**全部成功**后才切换，所以任何中断都不会变砖。

---

## 7. 安全

| 项 | 做法 |
|---|---|
| 传输 | 局域网明文 http 可接受；跨网段建议 https + CA bundle（`HttpDownloader` 已支持） |
| 完整性 | sha256（桥计算、设备校验），防止截断/串包 |
| 来源 | 桥侧可加 `X-Onepage-Token`，设备配置里存同一个值（可选） |
| 降级 | 默认拒绝版本号变小；调试可用 `--allow-downgrade` |
| 签名 | 可选：开启 Secure Boot v2 / `esp_app` 签名，彻底防伪造（会增加一次密钥管理成本） |

---

## 8. 实施顺序

### 阶段 0（零开发，今天可用）

1. 浏览器打开设备网页 → Files → 上传 `firmware.bin` 到 SD；
2. 设备：设置 → 固件升级 → 选中该 bin → 确认。

### 阶段 1（方案 D，半天）

1. **补回滚标记**（10 行）——否则 OTA 升完会自己回滚；
2. 设备 web server 加 `POST /api/ota`（流式落 SD → 校验 → 写槽 → 重启，~120 行）；
3. `HomePage.html` 加「固件升级」卡片（文件选择 + 进度 + 版本对比，~80 行）；
4. web 服务的独立开关（现在只在「网页传书」里启动）。

### 阶段 2（方案 B，一天）

1. 桥加 `/api/v1/ota/latest` + `/api/v1/ota/image`（~80 行）；
2. 设备端 `OtaUpdateActivity`：检查 → 下载 → 校验 → 写入 → 重启（~250 行）；
3. 联调：本地目录放 bin → 设备升级 → 故意推坏镜像验证自动回滚。

---

## 9. 待确认

1. ~~版本号~~ → 已定：`onepage.version`（当前 1.4.1）；
2. 镜像来源：先做「桥读本地目录」，还是一步到位接 GitHub Release？
3. 是否允许降级（把设备刷回旧版本）？
4. 网页端 OTA 是否需要密码保护（现在网页服务是局域网裸奔）？

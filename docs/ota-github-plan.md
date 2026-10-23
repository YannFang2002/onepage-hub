# 从 GitHub / Gitee 拉取固件的 OTA 方案

## 1. 先说结论

| 问题 | 答案 |
|---|---|
| 设备直连 GitHub 行不行？ | **不行/不稳定**。`raw.githubusercontent.com` 和 `objects.githubusercontent.com`（Release 附件）在国内经常被重置，设备端没有代理，TLS 握手直接失败。 |
| 能不能用 Gitee 替代？ | **可以，而且更适合**。Gitee 的 `raw` 与「发行版附件」国内直连可用；但要注意速率限制与文件大小限制，且需要你先建一个镜像仓库。 |
| 最稳的做法？ | **多源回退**：Gitee 优先 → GitHub 直连 → 自建/局域网源。设备端只认一个「清单地址」，清单里写清每个源的 URL 与 sha256。 |

---

## 2. 设备端需要什么

设备只会做三件事，与源是谁无关：

```text
GET <manifest>            → { version, url, sha256, size }
GET <url>                 → 二进制流
写入 OTA 槽 → 切 otadata → 重启
```

所以「换源」= 换 manifest 与 url，**固件逻辑不用改**。这一点很重要：先把多源做成配置，后面加源不需要重新烧固件。

---

## 3. 清单格式（建议）

```json
{
  "version": "1.4.2",
  "size": 5719840,
  "sha256": "9f2c…",
  "url": "https://gitee.com/<you>/onepage-fw/raw/master/firmware.bin",
  "mirrors": [
    "https://ghproxy.net/https://github.com/<you>/onepage-hub/releases/download/v1.4.2/firmware.bin",
    "https://cdn.jsdelivr.net/gh/<you>/onepage-hub@v1.4.2/firmware.bin"
  ],
  "notes": "修复录音丢帧"
}
```

设备按 `url` → `mirrors[]` 顺序尝试，任何一个下载成功后都用 `sha256` 校验；全失败才报错。

---

## 4. 三种源的具体做法

### A. Gitee（推荐，国内最稳）

1. 在 Gitee 建一个 **公开仓库** `onepage-fw`；
2. 每次发版：把 `firmware.bin` 和 `manifest.json` 提交进去（或用「发行版」传附件）；
3. 设备读取：

```text
https://gitee.com/<you>/onepage-fw/raw/master/manifest.json
https://gitee.com/<you>/onepage-fw/raw/master/firmware.bin
```

注意事项：
- Gitee **raw 有频率限制**（同 IP 短时间大量请求会被限），单个设备一天几次没问题；
- 单文件建议 < 100 MB（5.4 MB 完全没问题）；
- 若用「发行版附件」，下载 URL 形如 `https://gitee.com/<you>/onepage-fw/releases/download/v1.4.2/firmware.bin`，同样需要登录态时改用 raw 文件。

### B. GitHub + 加速前缀（备用）

```text
https://ghproxy.net/https://github.com/<you>/onepage-hub/releases/download/v1.4.2/firmware.bin
https://cdn.jsdelivr.net/gh/<you>/onepage-hub@v1.4.2/firmware.bin
```

- `ghproxy.net`、`gh-proxy.com` 这类反代可用性会变，不稳定，只作回退；
- jsDelivr 对 **Release 附件不支持**，只能代理仓库内文件（所以要把 bin 提交进仓库或 gh-pages）。

### C. 自建 / 局域网（最可控）

- 你已有「设备互联」网页服务，主机侧再放一个目录即可；
- 或者用任意 HTTP 服务器（Nginx / Python http.server）+ 内网地址；
- 优点：不依赖外网、速度最快；缺点：设备不在同一网络时不可用。

---

## 5. 落地步骤

| 阶段 | 内容 | 工作量 |
|---|---|---|
| 1 | 设备端加「源地址」配置项（默认 Gitee 清单），从清单下载并校验 | 半天 |
| 2 | 清单支持 `mirrors[]` 顺序回退 | 2 小时 |
| 3 | 设备端「检查更新」入口（本地已有 /api/ota 与刷写链路） | 2 小时 |
| 4 | 发版脚本：编译 → 算 sha256 → 写 manifest → 推 Gitee (+ GitHub) | 1 小时 |

> 设备端刷写链路（`validateImageFile` + `flashFromSdPath` + otadata 切换 + 回滚确认）已经验证可用，这里只是把「文件从哪来」换掉。

---

## 6. 安全

- **sha256 必须校验**：镜像来自公网，任何中间人都可能导致坏刷；
- 版本号单调递增（拒绝降级），除非显式开启；
- 可选：对 manifest 做签名（设备内置公钥），彻底防篡改；
- 设备端不保存任何 token：公开仓库不需要凭据；私有仓库不建议（密钥会被提取）。

---

## 7. 结论

**用 Gitee 做主源，GitHub 做回退，局域网源做兜底。** 设备端只认「清单 + 多镜像」，这样以后换源、加源都不需要重新烧固件。

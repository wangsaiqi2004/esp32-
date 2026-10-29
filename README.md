# ESP32-S3 校园网 NAT 路由器（ePortal 门户自动登录版）

在 [`martin-ger/esp32_nat_router`](https://github.com/martin-ger/esp32_nat_router) v2.4.22 基础上做的三个针对性修改，
用于「校园网只允许少量设备在线，但自己有一堆设备」的场景：ESP32 先自己接入校园网并完成网页认证，
再开热点做 NAT，让所有设备共用校园网的一个身份出去。

界面保持**纯英文原版**，没有做汉化。

## 硬件

| 项目 | 型号 |
|---|---|
| 主控 | ESP32-S3 N16R8（16MB flash / 8MB PSRAM，rev v0.2） |
| 串口 | CH340 → COM5（只负责烧录；应用日志走 USB-JTAG 原生口） |
| 屏幕 | 1.54" 240×240 IPS，ST7789，8 针 SPI（**本固件未驱动**） |

> ESP32-S3 的 bootloader 烧在 `0x0`，不是 ESP32 的 `0x1000`。照网上 ESP32 教程刷会开机失败。

## 相对上游的三个改动

| # | 改动 | 文件 | 效果 |
|---|---|---|---|
| 1 | **关闭 WiFi 省电**（`esp_wifi_set_ps(WIFI_PS_NONE)`） | `main/esp32_nat_router.c` | 吞吐从 2.6 Mbps 提升到 **64–79 Mbps**（约 25–30 倍） |
| 2 | **修复 `/scan` 页崩溃**（4KB 固定栈缓冲 → 按 AP 数 malloc + 边界钳制） | `components/http_server/http_server.c` | 扫描到 20 个 AP 时不再撑爆 httpd 任务栈、整站挂死 |
| 3 | **新增 ePortal 门户自动登录**（独立组件 + `/portal` 配置页） | `components/portal_login/`、`components/http_server/http_server.c` | 上行会话过期后自动重新认证，不用人工开网页登录 |

### 1. 为什么必须关省电

上游全项目**没有任何一处调用 `esp_wifi_set_ps()`**，走的是 ESP-IDF 默认 `WIFI_PS_MIN_MODEM`。
单射频 AP+STA 中继场景下，射频会在信标间隔休眠，下发给 AP 侧客户端的包要排队等它醒来，
吞吐被严重压制。实测同一台电脑、同一个源、单流：

| | 速度 |
|---|---|
| 默认（恢复出厂固件） | 327 KB/s ≈ 2.6 Mbps |
| 关闭省电后 | 8.0–9.9 MB/s ≈ 64–79 Mbps |

### 2. `/scan` 的崩溃原因

```c
char scan_html[4096];   // 每行结果约 600–700 字节
```

每个扫描结果行包含 SSID + 4 根信号柱的 HTML + dBm + 信道 + 加密方式 + 连接按钮。
校园环境通常扫出 20 个 AP，19 行就超过 4KB，把 httpd 任务的栈踩坏，
**整个内置 Web 服务器停摆**（热点还在、能连，但所有页面都打不开），必须重启才能恢复。
上游原版同样存在此问题。

修复：改为按实际 AP 数量在堆上分配、每次追加做边界钳制、扫描期间自动刷新从 2s 放宽到 5s
（避免反复扫描占死射频）。

### 3. ePortal 门户自动登录

协议是从学校门户自己的前端脚本逆向出来的，详见 [`docs/eportal-protocol.md`](docs/eportal-protocol.md)。

行为：

- 每 30 秒探测一次真实外网（两个 `generate_204` 端点，须返回 204 才算在线）
- 判定断网后自动 POST 登录，成功后每 240 秒发一次 `keepalive` 保活
- 登录失败**递增退避** 45s → 90s → 180s → 360s → 600s 封顶，避免撞锁账号
- 凭据存 NVS，网页上不回显

配置入口：`http://192.168.4.1/portal`（也监听上游网口，校园网内可用 STA IP 直接访问）

## 编译

需要 PlatformIO + ESP-IDF 5.5.x（首次会自动下载约 8.5GB 工具链）。

```bash
# 关键：必须清掉 PYTHONPATH，否则某些环境注入的 sitecustomize 会把构建系统半路杀掉
unset PYTHONPATH
export PIP_INDEX_URL=https://mirrors.aliyun.com/pypi/simple/

# 并行构建容易挂死，用单线程
pio run -e esp32-s3-devkitc-1 -j 1
```

其他踩过的坑：

- `pypi.org` 在校园网内不通，必须换镜像源
- 中文 Windows 下 git 输出的报错是 GBK，组件管理器按 UTF-8 解码会直接崩 → `esp_wireguard` 依赖改成本地组件（`components/esp_wireguard/`）
- 应用分区上限 1536KB，当前固件 1369KB，**余量仅 167KB**

## 刷机

```bash
# 整片刷（含 bootloader / 分区表）
esptool.py --port COM5 --baud 460800 --chip esp32s3 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size detect \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0xf000  ota_data_initial.bin \
  0x20000 firmware.bin

# 只刷应用（保留 NVS 里的全部配置，推荐）
esptool.py --port COM5 --baud 460800 --chip esp32s3 write-flash \
  --flash-mode dio --flash-freq 80m 0x20000 firmware.bin
```

`firmware/firmware_esp32s3_v3_portal.bin` 是编译好的产物，1351→1369KB。

## 使用

1. 连热点（默认 `ESP32_NAT_Router`，开放）
2. 打开 `http://192.168.4.1` → `Getting Started`
3. `Uplink (Internet)` 填校园网 SSID；若校园网是**开放网络**（连上后弹网页认证），
   密码留空并勾 `Open network (no password)`
4. `Save & Reboot`
5. 任一设备打开认证页手动登录一次 → 所有下游设备即可共享（NAT 后校园网只看到 ESP32 一个 MAC）
6. 想让它在会话过期后自动重登：打开 `http://192.168.4.1/portal`，填学号密码并启用

## 已知限制

- 单射频 AP+STA 分时复用，802.11n 单流理论上限约 80–90 Mbps；作者标称 15 Mbps 是保守值
- 最多 8 个无线客户端（每客户端约 5KB RAM），开满 Web/VPN/PCAP 等功能会更快耗尽内存
- 上行信号弱会直接拖慢速度：实测 `-69 dBm` 时明显受限，挪位置/换天线收益大于改固件
- `/portal` 里的 `queryString`（`wlanuserip` 部分）是按 ePortal 惯例构造的，
  未用真实密码验证；若登录被拒，页面状态行会显示门户返回的原始信息，按提示调整
- 屏幕（ST7789）未驱动，应用分区余量不足以塞 LVGL

## 上游与许可

核心 NAT / 防火墙 / WireGuard / Web UI 均来自
[`martin-ger/esp32_nat_router`](https://github.com/martin-ger/esp32_nat_router)（MIT 风闻，仓库未声明 LICENSE 文件）。
本仓库仅在其之上追加上述三处改动，同样沿用上游的姿态。

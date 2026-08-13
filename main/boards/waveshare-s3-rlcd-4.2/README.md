# Waveshare ESP32-S3-RLCD-4.2（本仓库唯一生产板）

基于 Waveshare ESP32-S3-RLCD-4.2 的桌面信息屏固件：五个信息页 + 小智语音 + AI 额度监控 + 局域网后台。

> 完整功能说明见 **[docs/功能文档.md](../../../docs/功能文档.md)**；本文件只描述板级硬件、引脚、构建烧录与故障排查。

---

## 硬件特性

### 主控
- **ESP32-S3-WROOM-1-N16R8**：双核 Xtensa LX7 @ 240MHz，16MB Flash + 8MB PSRAM，Wi-Fi 802.11 b/g/n + BLE 5.0

### 显示屏
- **400×300 反射式单色 LCD（RLCD）**：1-bit 黑/白、超低功耗、阳光下可读，SPI 40 MHz
- 自写驱动 `rlcd_driver.{h,cc}`：PSRAM 帧缓冲 + LUT，SPI 按 1 KB 分片、`ESP_ERR_NO_MEM` 重试 3 次

### 传感器
- **SHTC3** 温湿度（I2C，±0.2°C / ±2% RH）
- **PCF85063** RTC（I2C，纽扣电池备份，断电保持时间）

### 音频
- **ES8311** 解码（喇叭输出）、**ES7210** 麦克风输入、**MAX98357A** D 类功放

### 存储与电源
- SD 卡槽（SDMMC，CLK→GPIO38、CMD→GPIO21、D0→GPIO39，挂载点 `/sdcard`）
- 锂电池接口 + USB-C 充电，ADC 电量检测（GPIO4，3:1 分压）

### 按键
- BOOT（GPIO0）、USER（GPIO18）

---

## 按键功能

| 按键 | 场景 | 功能 |
|---|---|---|
| BOOT 单击 | 启动阶段 | 进入 Wi-Fi 配网 |
| BOOT 单击 | 综合页 | 开始 / 停止小智对话 |
| BOOT 单击 | AI 额度页 | 立即刷新账号额度 |
| BOOT 单击 | 七日天气页 | 立即刷新天气 |
| USER 单击 | 任意 | 切下一页（AI 多子页时先翻子页） |
| USER 双击 | 任意 | 刷新全部数据（天气/时间/传感器） |
| USER 长按 | 任意 | 综合页滚动显示系统信息（CPU/内存/电池/Wi-Fi） |

连续 5 分钟无操作进入省电模式（UI 刷新 1s→5s），任意按键唤醒。

---

## 页面速览

`overview` 综合页 · `calendar` 日历页 · `forecast` 七日天气页 · `quota` AI 额度页 · `todo` 待办页

页面 ID 是稳定配置值（存 NVS），不要因标题文案变化而修改。启停与排序在局域网后台配置。

---

## 编译与烧录

### 环境
- ESP-IDF **v5.5.2**，Python 3.8+

### 步骤

```bash
# 1. 加载 ESP-IDF 环境（路径按实际安装位置修改）
. /path/to/esp-idf-v5.5.2/export.sh

# 2. 进入项目并配置
cd xiaozhi-esp32
idf.py set-target esp32s3
idf.py menuconfig
#    确认：Board Type = Waveshare ESP32-S3-RLCD-4.2
#          分区表 = partitions/v2/16m_rlcd_quota.csv（16MB 自定义表）
#          Flash 大小 = 16 MB

# 3. 编译（关注 app 分区余量，目前仅约 6%）
idf.py build

# 4. 烧录 + 监视
ls /dev/cu.usbmodem*
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

### 注意

- **自定义分区表**（含 256KB `quota_nvs`）：更换分区表后必须**完整烧录**，不能只 OTA；完整烧录布局见 `docs/waveshare-s3-rlcd-4.2-project-handbook.md §9`。
- 不要漏烧 `generated_assets.bin`（字体/表情/图标），否则显示缺资源。
- 烧录后串口观察 ≥60 秒，关注 `abort` / `reboot` / `ESP_ERR_NO_MEM` / `rlcd spi tx failed` / `minimal sram`。

### 常用命令

```bash
idf.py flash          # 仅烧录（不重编）
idf.py monitor        # 仅监视串口
idf.py erase-flash    # 全片擦除（丢失 Wi-Fi、后台密码、全部业务数据）
```

---

## 故障排查

### 屏幕不显示
1. 查 SPI 引脚：MOSI→GPIO12、SCK→GPIO11、CS→GPIO40、DC→GPIO5、RST→GPIO41
2. 看串口日志 `初始化 RLCD 屏幕` 是否成功
3. 检查供电是否稳定

### 启动几十秒后重启 / `ESP_ERR_NO_MEM`
- 大概率是 LVGL 对象过多耗尽**内部 SRAM**（PSRAM 充足不代表 SPI/DMA 用的内部 heap 充足）。
- 串口搜 `minimal sram`，健康值应 ≥ 20 KB；UI 改动必须控制对象数量（用少量多行 `lv_label` 替代大量小对象）。

### Wi-Fi 连不上
1. 确认 2.4 GHz 网络（ESP32 不支持 5 GHz）
2. 后台「Wi-Fi」区块可直接切换/新增网络；或 BOOT 进入配网（BluFi App / 热点 `Xiaozhi-XXXX` → `http://192.168.4.1`）
3. 仍不行：`idf.py erase-flash` 后重新配网

### 时间不准
1. 确认已联网（NTP 同步需要网络）
2. USER 双击手动刷新
3. 断电时间丢失 → 检查 RTC 纽扣电池

### 天气不更新
1. 后台「天气」确认城市与高德 Web 服务 Key 已保存（Key 不回显）
2. 用后台「天气诊断」看最近一次请求的 HTTP 状态与结果
3. 七日预报来自 Open-Meteo（无需 Key），失败会自动降级高德三日

### 额度不刷新
1. 后台账号卡片看错误信息与 stale 标记；失败会自动重试 3 次（2s/4s/8s）
2. 需翻墙的供应商（如 Codex）确认代理已启用且代理监听局域网地址（非 127.0.0.1）
3. 用后台「代理诊断」逐级检查 TCP → CONNECT → TLS

### 后台 401 / 登录循环
- API 必须用 `/api/login` 返回的真实 `sid` Cookie，写请求还要带同会话的 `X-CSRF-Token`；curl 用 `-c`/`-b` 保存回放 Cookie，不要手写。
- 设备重启会话会从 NVS 恢复；Cookie 被禁、IP 变化也会导致重新登录。

---

## 相关文档

- [功能文档（完整功能说明）](../../../docs/功能文档.md)
- [项目手册（烧录验收、维护经验）](../../../docs/waveshare-s3-rlcd-4.2-project-handbook.md)
- [Waveshare 官方 Wiki](https://www.waveshare.com/wiki/ESP32-S3-RLCD-4.2)
- [上游小智项目](https://github.com/78/xiaozhi-esp32)

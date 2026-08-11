# RLCD 代理、城市天气与视觉修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复认证代理 TLS 隧道兼容性，将天气配置简化为国内省市选择，并修正日历当天标记和主页状态区黑白关系。

**Architecture:** 保留现有自定义 HTTP CONNECT transport，但让 TLS 配置对齐 ESP-IDF 原生 HTTPS 的安全参数，并把失败阶段传回额度卡片。WeatherManager 改为使用免 Key 的 Open-Meteo 单请求接口，后台只保存经过校验的省市名和经纬度；屏幕仍消费同一个 WeatherData 快照。日历用透明细框标记当天，主页状态区融入黑色顶栏。

**Tech Stack:** ESP-IDF 5.5.2、C++17、mbedTLS、LVGL、cJSON、嵌入式 HTML/CSS/JavaScript、Python unittest。

---

### Task 1: 锁定回归契约

**Files:**
- Modify: `tests/host/rlcd_ui_source_contract_test.py`
- Modify: `tests/host/rlcd_manager_safety_test.cc`

- [ ] **Step 1: 写代理、城市天气、日历细框和主页同色状态区的失败断言**
- [ ] **Step 2: 运行 Python 与 C++ 宿主机测试，确认因新行为尚未实现而失败**
- [ ] **Step 3: 保留失败输出作为 RED 证据**

### Task 2: 修复代理 TLS transport

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_proxy_transport.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_proxy_transport.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_manager.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`

- [ ] **Step 1: 明确启用 REQUIRED 证书校验、SNI、HTTP/1.1 ALPN 和 TLS 读取超时**
- [ ] **Step 2: 在 transport 内记录 TCP、CONNECT、TLS 和 HTTP 阶段错误，不记录认证头**
- [ ] **Step 3: 对握手 EOF 进行一次全新 TCP/CONNECT/TLS 重试**
- [ ] **Step 4: 后台增加单账号“测试代理”操作并显示脱敏结果**
- [ ] **Step 5: 运行定向测试确认转绿**

### Task 3: 城市化免 Key 天气

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/data_update_task.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/README.md`

- [ ] **Step 1: 将天气配置收敛为 province、city、latitude、longitude 并校验中国范围**
- [ ] **Step 2: 通过 Open-Meteo 一次请求解析当前天气与七日预报**
- [ ] **Step 3: 在后台提供省份和城市联动下拉，移除 ID、Host 和 Key 输入**
- [ ] **Step 4: 保持 WeatherData 快照接口不变，网络失败继续显示上次成功数据**
- [ ] **Step 5: 运行城市配置和源码契约测试确认转绿**

### Task 4: 修复真机视觉问题

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/calendar_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/weather_ui.cc`

- [ ] **Step 1: 删除当天实心覆盖标签，改为透明背景、1 像素黑框且仅包围日期行**
- [ ] **Step 2: 状态区改为黑底，图标 recolor 和电量文字统一为白色**
- [ ] **Step 3: 人工核对 400×300 坐标、边界和黑白层级**
- [ ] **Step 4: 运行 UI 源码契约测试确认转绿**

### Task 5: 构建、烧录与交付

**Files:**
- Verify: `build-codex/xiaozhi.bin`

- [ ] **Step 1: 运行 C++ UBSan、Python 契约、内嵌 JavaScript node --check 和 git diff --check**
- [ ] **Step 2: 使用 ESP-IDF 5.5.2 完整编译并记录固件大小与 SHA-256**
- [ ] **Step 3: 烧录 `/dev/cu.usbmodem2101` 并监控启动、页面切换、天气与代理日志**
- [ ] **Step 4: 确认无崩溃、watchdog、重启或明显 SRAM 回退**
- [ ] **Step 5: 仅暂存本轮文件，保留三个 dataless-backup 未跟踪文件并提交**


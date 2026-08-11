# RLCD 排版、认证代理与设备控制 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复真机日历和天气的列错位，支持带账号密码的 HTTP CONNECT 代理，并精简后台预览后增加设备音量与健康状态控制。

**Architecture:** 屏幕的多列内容由一个空格对齐标签改为七个固定宽度标签，保留低 LVGL 对象数。代理 URL 解析和 Basic 认证编码放入无 ESP 依赖的纯 C++ 边界，传输层只消费解析结果。后台设备 API 使用现有会话和 CSRF 校验，音量变更通过主任务调度。

**Tech Stack:** ESP-IDF 5.5.2、C++17/23、LVGL 9、esp_http_server、mbedTLS、内嵌 HTML/CSS/JavaScript、Python unittest。

---

### Task 1: 锁定真机排版契约

**Files:**
- Modify: `tests/host/rlcd_ui_source_contract_test.py`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/calendar_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/forecast_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h`

- [ ] 先添加日历/天气七列和中文年月契约，运行 `python3 -m unittest tests/host/rlcd_ui_source_contract_test.py` 确认失败。
- [ ] 用 7 个宽度 56–57 px 的列标签替换空格拼接，今日仍只用 1 个反色对象。
- [ ] 右上角输出 `YYYY年 · MM月`，重跑契约测试转绿。

### Task 2: 认证 HTTP CONNECT 代理

**Files:**
- Create: `main/boards/waveshare-s3-rlcd-4.2/managers/proxy_auth.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_proxy_transport.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_proxy_transport.h`
- Modify: `tests/host/rlcd_manager_safety_test.cc`

- [ ] 先写虚构凭据的 URL 解析、百分号解码、Basic 头断言，编译确认因缺少新边界而失败。
- [ ] 实现 `http://user:password@host:port` 与无认证 URL 解析，拒绝控制字符和非法端口。
- [ ] CONNECT 请求有认证时添加 `Proxy-Authorization: Basic ...`，日志仅保留主机和端口。

### Task 3: 代理凭据不回显且留空保留

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_manager.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_manager.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`
- Modify: `tests/host/rlcd_ui_source_contract_test.py`

- [ ] 添加 API 不输出 userinfo、`has_proxy_auth`、`proxy_endpoint` 和留空保留契约，先确认失败。
- [ ] GET 仅返回脱敏端点和布尔状态；PUT 中新 URL 覆盖，留空保留，显式清除才删除。
- [ ] 后台代理输入改为密码框，不将已保存凭据放入 DOM。

### Task 4: 精简后台并添加设备控制

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`
- Modify: `main/audio/audio_codec.cc`
- Modify: `tests/host/rlcd_ui_source_contract_test.py`

- [ ] 先添加预览 DOM/函数不存在、`/api/device`、音量范围与 0 音量可持久化契约，确认失败。
- [ ] 删除两组预览 HTML、CSS 和 JavaScript，保留账号、页面、天气、日历、待办操作面。
- [ ] 增加需登录的设备 GET/PUT API：运行时间、当前/历史最低内存、IP、音量 0–100。
- [ ] 增加音量滑块、静音和恢复，音量变更在主任务中执行并使 0 成为有效持久值。

### Task 5: 完整验证和真机回归

**Files:**
- Verify: `build-codex/xiaozhi.bin`

- [ ] 运行 Python 源码契约、C++ 主机契约、UBSan、内嵌 JavaScript `node --check` 和 `git diff --check`。
- [ ] 使用 ESP-IDF 5.5.2 完整编译，检查固件大小和应用分区余量。
- [ ] 完整烧录已识别的 ESP32-S3，串口检查页面切换、代理错误、音量和 watchdog/重启。
- [ ] 暂存时排除 `*.dataless-backup`和构建产物，提交并更新项目记忆。

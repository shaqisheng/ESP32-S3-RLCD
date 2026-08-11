# RLCD 仪表盘重设计实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 400×300 单色 RLCD 的主页、日历、天气和 AI 页统一为已确认的黑白编辑式布局，并在 AI 页显示动态后台管理地址。

**Architecture:** 保留现有 `CustomLcdDisplay` 更新接口，重写三个页面的 LVGL 对象布局。日历和天气继续使用少量多行标签，主页移除嵌套卡片；后台地址由纯格式化函数生成，并在额度页渲染时从 `WifiManager` 读取 IP。

**Tech Stack:** ESP-IDF 5.5.2、C++、LVGL、Waveshare ESP32-S3-RLCD-4.2、主机 C++ 契约测试。

---

### Task 1: 锁定格式和页面结构

**Files:**
- Modify: `tests/host/rlcd_manager_safety_test.cc`
- Create: `tests/host/rlcd_ui_source_contract_test.py`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/manager_safety.h`

- [ ] 在 C++ 测试中断言 `FormatAdminAddress("192.168.2.71")` 返回 `管理 · http://192.168.2.71:8080/admin`，空 IP 和 `0.0.0.0` 返回 `管理后台 · 等待网络`。
- [ ] 在源码契约测试中断言三个页面包含 `EDITORIAL_*_V2` 标记，主页不再创建 `time_card`、`calendar_card` 和 `memo_card`，AI 页使用 `WifiManager::GetInstance().GetIpAddress()`。
- [ ] 运行测试并确认因格式化函数和新布局尚不存在而失败。
- [ ] 实现最小格式化函数并让 C++ 测试通过。

### Task 2: 重构主页

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/weather_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.cc`

- [ ] 使用 184px 黑色信息区和 116px 白色底栏替代四张圆角卡片。
- [ ] 将时间、日期和天气作为上半区主视觉；底栏保留小智状态与三条待办。
- [ ] 调整对话长文本定位逻辑，使其适配新的底栏尺寸。
- [ ] 运行源码契约测试确认旧卡片结构已删除。

### Task 3: 重构日历和天气

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/calendar_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/forecast_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h`

- [ ] 日历使用 74px 黑色月份标题区、星期行和单一六周网格标签。
- [ ] 用一个黑色背景对象和一个白色日期标签动态标记今天，避免 42 格对象树。
- [ ] 天气使用 116px 黑色当前天气区和单一七列预报标签，不增加七组控件。
- [ ] 运行源码契约测试确认编辑式布局标记与对象预算。

### Task 4: AI 后台地址

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/quota_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h`

- [ ] 新增 `quota_admin_label_`，在页头显示动态 `http://<IP>:8080/admin`。
- [ ] 将刷新时间放在地址下方；卡片区域下移并保持 1～4 项弹性布局。
- [ ] 运行 C++ 与源码契约测试。

### Task 5: 编译、烧录和观察

**Files:**
- Verify: `sdkconfig`
- Verify: `build/xiaozhi.bin`

- [ ] 安装 ESP-IDF 5.5.2 的 esp32s3 工具链并导出环境。
- [ ] 清理旧绝对路径构建缓存后重新配置并编译。
- [ ] 检查 `xiaozhi.bin` 大小、分区余量和 `git diff --check`。
- [ ] 自动识别 `/dev/cu.*` 开发板串口，执行完整 flash。
- [ ] 观察启动日志，确认无 `abort`、`ESP_ERR_NO_MEM`、watchdog 或循环重启。

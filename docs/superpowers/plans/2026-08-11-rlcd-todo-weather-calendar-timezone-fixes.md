# RLCD 待办、七日天气、日历节日与时区修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复后台待办不同步、天气仅三天、日历标题裁切与传统节日缺失，以及 OTA 造成的八小时时间跳变。

**Architecture:** 保持高德提供国内城市实时天气，利用后台城市目录已有经纬度调用 Open-Meteo 获取七日预报，并在失败时保留高德三日数据作为降级。待办变更由 HTTP 线程调度到应用主任务刷新 LVGL；日历传统节日和节气采用本地纯算法，不依赖网络节假日源；OTA 只按 Unix epoch 设置系统时间，不叠加显示时区。

**Tech Stack:** ESP-IDF v5.5.2、C++17、LVGL、cJSON、ESP HTTP Client、NVS、Python 源码契约测试、宿主机 C++/UBSan。

---

## 文件边界

- `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`：后台待办编辑交互与变更后的主任务 UI 刷新调度；天气城市坐标随配置保存。
- `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.{h,cc}`：高德实时天气、Open-Meteo 七日预报、三日降级和诊断状态。
- `main/boards/waveshare-s3-rlcd-4.2/managers/calendar_manager.{h,cc}`：传统农历节日与二十四节气文本解析。
- `main/boards/waveshare-s3-rlcd-4.2/calendar_ui.cc`、`custom_lcd_display.h`：年月标题拆分和日期格详情优先级。
- `main/ota.cc`：服务器 Unix 时间戳语义修正。
- `tests/host/rlcd_ui_source_contract_test.py`：跨模块启动与 UI 源码契约。
- `tests/host/rlcd_manager_safety_test.cc`：纯日期规则与状态测试。

### Task 1：后台待办变更后立即刷新硬件

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`
- Test: `tests/host/rlcd_ui_source_contract_test.py`

- [ ] **Step 1：写失败契约**

新增测试，要求 POST、PUT、DELETE 成功路径都调用同一个主任务刷新函数，并要求后台提供编辑按钮：

```python
def test_admin_todo_mutations_schedule_immediate_display_refresh(self):
    admin = (BOARD / "managers/admin_server.cc").read_text()
    self.assertIn("ScheduleTodoDisplayRefresh", admin)
    self.assertGreaterEqual(admin.count("ScheduleTodoDisplayRefresh();"), 3)
    self.assertIn("editTodo(", admin)
```

- [ ] **Step 2：运行失败测试**

Run: `python3 -m unittest tests.host.rlcd_ui_source_contract_test.RlcdUiSourceContractTest.test_admin_todo_mutations_schedule_immediate_display_refresh`

Expected: FAIL，当前代码没有 `ScheduleTodoDisplayRefresh`。

- [ ] **Step 3：实现安全刷新调度**

在板级后台实现统一函数，HTTP 回调不直接操作 LVGL：

```cpp
void ScheduleTodoDisplayRefresh() {
    Application::GetInstance().Schedule([]() {
        auto* display = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (display) display->RefreshMemoDisplay();
    });
}
```

在待办创建、更新、删除成功后调用该函数。后台列表为每项增加“编辑”按钮，继续复用 `PUT /api/todos/{id}` 更新内容、日期和时间。

- [ ] **Step 4：验证待办契约**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Expected: 所有源码契约通过。

### Task 2：保留高德实时天气并补齐七日预报

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.cc`
- Test: `tests/host/rlcd_ui_source_contract_test.py`

- [ ] **Step 1：写失败契约**

```python
def test_weather_combines_amap_current_with_open_meteo_seven_day_forecast(self):
    admin = (BOARD / "managers/admin_server.cc").read_text()
    weather = (BOARD / "managers/weather_manager.cc").read_text()
    self.assertIn("latitude:city[1]", admin)
    self.assertIn("longitude:city[2]", admin)
    self.assertIn("api.open-meteo.com/v1/forecast", weather)
    self.assertIn("forecast_days=7", weather)
    self.assertIn("parseOpenMeteoForecastJson", weather)
    self.assertIn("forecast_count = 7", weather)
```

- [ ] **Step 2：运行失败测试**

Run: `python3 -m unittest tests.host.rlcd_ui_source_contract_test.RlcdUiSourceContractTest.test_weather_combines_amap_current_with_open_meteo_seven_day_forecast`

Expected: FAIL，当前只调用高德 `extensions=all` 并限制为三天。

- [ ] **Step 3：扩展配置但不增加手工输入**

为 `WeatherConfig` 增加经纬度，默认使用苏州坐标；后台保存城市时直接提交目录中的 `city[1]`、`city[2]`。固件校验纬度 `[-90,90]`、经度 `[-180,180]` 并写入 NVS。

```js
body:JSON.stringify({
  province:province,
  city:city[0],
  latitude:city[1],
  longitude:city[2],
  amap_adcode:adcode,
  amap_key:key,
  clear_amap_key:el("weatherClearAmapKey").checked,
  refresh_interval_minutes:minutes
})
```

- [ ] **Step 4：实现七日请求与解析**

实时天气继续请求高德 `extensions=base`。七日预报请求：

```text
https://api.open-meteo.com/v1/forecast
  ?latitude=<lat>&longitude=<lon>
  &daily=weather_code,temperature_2m_max,temperature_2m_min
  &timezone=Asia%2FShanghai&forecast_days=7
```

严格校验 `daily.time`、`daily.weather_code`、最高温和最低温四个数组均至少七项；WMO code 映射为现有单色图标支持的“晴、云、雾、雨、雪、雷雨”。温度四舍五入为整数文本。

- [ ] **Step 5：保留三日降级并完善诊断**

Open-Meteo 失败时才调用高德 `extensions=all`，保留三日数据并让 `update()` 返回失败，使后台任务五分钟后重试七日预报。诊断文本明确区分“高德实时成功 / 七日预报成功”与“七日失败 / 已降级三日”，不输出高德 Key。

- [ ] **Step 6：运行天气契约**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Expected: 所有源码契约通过，且原有 Key 不回显契约继续通过。

### Task 3：修复年月标题并显示传统节日和节气

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/calendar_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/calendar_manager.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/calendar_manager.cc`
- Test: `tests/host/rlcd_ui_source_contract_test.py`
- Test: `tests/host/rlcd_manager_safety_test.cc`

- [ ] **Step 1：写失败契约与日期样例**

契约要求年月拆成不会裁切的两个标签，并要求日期格调用传统日历文本：

```python
def test_calendar_header_and_traditional_festivals_are_complete(self):
    ui = (BOARD / "calendar_ui.cc").read_text()
    header = (BOARD / "custom_lcd_display.h").read_text()
    self.assertIn("calendar_year_label_", header)
    self.assertIn("calendar_month_label_", header)
    self.assertIn('"%d年"', ui)
    self.assertIn('"%d月"', ui)
    self.assertIn("TraditionalDayText", ui)
```

宿主机规则测试至少锁定：

```cpp
assert(TraditionalDayText(2026, 8, 19) == "七夕节");
assert(TraditionalDayText(2026, 8, 7) == "立秋");
assert(TraditionalDayText(2026, 8, 23) == "处暑");
```

- [ ] **Step 2：运行失败测试**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Expected: FAIL，当前只有单个 205px 的 48px 标题，并且没有传统节日接口。

- [ ] **Step 3：拆分年月标题**

用大号数字标签显示 `2026年`，在其右侧使用较小中文字体显示 `8月`，给右侧农历副标题保留固定区域。禁止依赖自动溢出或滚动，保证一月至十二月都完整显示。

- [ ] **Step 4：实现本地传统日历文本**

复用现有公历转农历结果，内置至少：春节、元宵节、龙抬头、端午节、七夕节、中元节、中秋节、重阳节、腊八节。增加 1900–2049 二十四节气计算，返回两至三字节气名。

日期格详情优先级固定为：

```text
调休数据“休/班” > 传统节日 > 二十四节气 > 农历日期
```

这样 2026-08-19 显示“七夕节”，同时保留法定节假日/调休标记。

- [ ] **Step 5：运行日历测试**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Run: `clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined -fno-omit-frame-pointer -I main/boards/waveshare-s3-rlcd-4.2/managers tests/host/rlcd_manager_safety_test.cc -o /tmp/rlcd_manager_safety_test && /tmp/rlcd_manager_safety_test`

Expected: 所有测试通过，UBSan 无输出。

### Task 4：修复 OTA 服务器时间重复加时区

**Files:**
- Modify: `main/ota.cc`
- Test: `tests/host/rlcd_ui_source_contract_test.py`

- [ ] **Step 1：写失败契约**

```python
def test_ota_keeps_server_timestamp_as_utc_epoch(self):
    ota = (ROOT / "main/ota.cc").read_text()
    server_time = ota[ota.index('cJSON *server_time'):ota.index('has_new_version_ = false')]
    self.assertNotIn("ts +=", server_time)
    self.assertNotIn("timezone_offset->valueint", server_time)
    self.assertIn("tv.tv_sec = (time_t)(ts / 1000)", server_time)
```

- [ ] **Step 2：运行失败测试**

Run: `python3 -m unittest tests.host.rlcd_ui_source_contract_test.RlcdUiSourceContractTest.test_ota_keeps_server_timestamp_as_utc_epoch`

Expected: FAIL，当前代码将 `timezone_offset` 再次加到 epoch。

- [ ] **Step 3：按 epoch 语义设置系统时间**

`timestamp` 直接转换为 `timeval`；`timezone_offset` 只作为服务器元数据，不参与 `settimeofday`。本地显示仍由 `TZ=CST-8` 和 `localtime_r` 处理。

- [ ] **Step 4：运行完整定向测试**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Run: `clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined -fno-omit-frame-pointer -I main/boards/waveshare-s3-rlcd-4.2/managers tests/host/rlcd_manager_safety_test.cc -o /tmp/rlcd_manager_safety_test && /tmp/rlcd_manager_safety_test`

Run: `git diff --check`

Expected: 全部通过。

### Task 5：编译、烧录与真机验收

**Files:**
- Build artifact: `build-codex/xiaozhi.bin`

- [ ] **Step 1：完整固件编译**

Run: `ninja -C build-codex`

Expected: `xiaozhi.bin` 生成且应用分区未溢出。

- [ ] **Step 2：烧录真机**

Run: `ESPPORT=/dev/cu.usbmodem2101 ninja -C build-codex flash`

Expected: bootloader、应用、分区表、OTA 数据和资源分区全部通过哈希校验。

- [ ] **Step 3：真机验收**

逐项验证：

1. 后台新增、编辑、勾选完成、删除待办后，综合页在接口成功返回后立即变化。
2. 天气页七列均有日期、天气、最高温和最低温；后台诊断显示高德实时与 Open-Meteo 七日两阶段结果。
3. 日历标题完整显示“2026年8月”；2026-08-19 显示“七夕节”，8月节气显示“立秋”“处暑”。
4. NTP 后 OTA、MQTT 正常连接，串口不再出现约 28800 秒时间跳变与 RTC 恢复日志。
5. 连续观察不少于 90 秒，无重启、看门狗或重复天气首刷。

## 方案依据

- 高德官方天气文档规定预报数组只有当天、第二天、第三天，因此不能仅靠调整解析数量得到七天。
- Open-Meteo 官方 Forecast API 默认提供七天、最多十六天，支持按经纬度获取 `weather_code` 和每日最高/最低温。
- 当前后台城市目录已经保存经纬度，采用混合数据源无需增加新的用户输入或密钥。


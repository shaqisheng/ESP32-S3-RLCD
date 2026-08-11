# RLCD QWeather Calendar Proxy Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复主页状态图标、将月历重构为参考图的独立日期格，恢复和风天气城市配置，并给后台提供可复现的代理连通性诊断。

**Architecture:** 主页状态栏不再使用带白色不透明底的 RGB565 位图，而采用 LVGL 符号标签；日历以 42 个独立单元替换按列拼接的多行文本。WeatherManager 持久化省市、和风城市 ID 和脱敏 API Key，直接请求和风天气接口；额度代理的诊断复用真实 HTTP CONNECT/TLS transport，向后台返回不含凭据的阶段结果。

**Tech Stack:** ESP-IDF v5.5.2、LVGL、esp_http_client、mbedTLS、cJSON、嵌入式 HTML/CSS/JavaScript、Python 源码契约测试。

---

### Task 1: 写入页面与诊断的失败契约

**Files:**
- Modify: `tests/host/rlcd_ui_source_contract_test.py`
- Test: `tests/host/rlcd_ui_source_contract_test.py`

- [ ] **Step 1: 写失败测试**

```python
def test_overview_uses_symbol_labels_not_opaque_rgb565_icons(self):
    source = (BOARD / "weather_ui.cc").read_text()
    self.assertIn("overview_wifi_symbol_", source)
    self.assertIn("LV_SYMBOL_WIFI", source)
    self.assertNotIn("wifi_icon_img_ = lv_image_create(status_strip)", source)

def test_calendar_uses_42_independent_day_cells(self):
    source = (BOARD / "calendar_ui.cc").read_text()
    self.assertIn("REFERENCE_MONTH_CALENDAR_V5", source)
    self.assertIn("calendar_day_cells_", source)
    self.assertIn("for (int cell_index = 0; cell_index < 42; ++cell_index)", source)

def test_qweather_and_proxy_diagnostics_are_exposed_without_secrets(self):
    weather = (BOARD / "managers/weather_manager.cc").read_text()
    admin = (BOARD / "managers/admin_server.cc").read_text()
    self.assertIn("devapi.qweather.com", weather)
    self.assertIn('"/api/proxy-diagnostic"', admin)
    self.assertIn("proxyDiagnostic", admin)
```

- [ ] **Step 2: 验证失败**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Expected: 新增三项断言失败，因为当前代码仍使用图像对象、列拼接日历和 Open-Meteo。

### Task 2: 修复主页图标与重构日历单元

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/weather_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/calendar_ui.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h`

- [ ] **Step 1: 用文本符号替换不透明位图**

```cpp
overview_wifi_symbol_ = lv_label_create(status_strip);
lv_label_set_text(overview_wifi_symbol_, LV_SYMBOL_WIFI);
lv_obj_set_style_text_color(overview_wifi_symbol_, lv_color_white(), 0);
overview_battery_symbol_ = lv_label_create(status_strip);
lv_label_set_text(overview_battery_symbol_, LV_SYMBOL_BATTERY_FULL);
lv_obj_set_style_text_color(overview_battery_symbol_, lv_color_white(), 0);
```

- [ ] **Step 2: 创建 42 个独立日历格**

```cpp
for (int cell_index = 0; cell_index < 42; ++cell_index) {
    auto& cell = calendar_day_cells_[cell_index];
    cell.root = lv_obj_create(calendar_page_);
    cell.day = lv_label_create(cell.root);
    cell.detail = lv_label_create(cell.root);
}
```

每格固定 `56x34`，日期与农历/节日为独立标签；星期栏固定七列；当日格改为黑色圆形或圆角实心背景，文字反白。周末不使用红色，保持单色屏可读性。

- [ ] **Step 3: 验证 UI 契约**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Expected: 图标和日历相关测试通过。

### Task 3: 恢复和风城市天气配置

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/weather_manager.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/README.md`

- [ ] **Step 1: 将天气配置扩展为和风城市 ID 与 API Key**

```cpp
struct WeatherConfig {
    std::string province;
    std::string city;
    std::string location_id;
    std::string api_key;
};
```

API 读取响应不得返回 `api_key`，只能返回 `has_api_key`。空 Key 更新应保留原值，`clear_api_key: true` 才允许清除。

- [ ] **Step 2: 请求和风实时与七日接口**

```cpp
snprintf(url, sizeof(url),
         "https://devapi.qweather.com/v7/weather/now?location=%s&key=%s",
         config.location_id.c_str(), config.api_key.c_str());
```

七日预报使用同一城市 ID。无 Key 时保留已缓存天气并返回“未配置和风 API Key”。

- [ ] **Step 3: 后台保留省市选择并新增密码型 API Key 输入**

```javascript
body: JSON.stringify({
  province: province,
  city: city[0],
  location_id: city[3],
  api_key: el("weatherApiKey").value
})
```

城市目录每项保留 `城市名、纬度、经度、和风城市 ID`；页面仅展示 API Key 是否已保存。

- [ ] **Step 4: 验证**

Run: `python3 tests/host/rlcd_ui_source_contract_test.py`

Expected: 和风 API 和脱敏 Key 的契约通过。

### Task 4: 实现代理连通性诊断并对照 sub2api 链路

**Files:**
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_proxy_transport.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_proxy_transport.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/quota_manager.cc`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.h`
- Modify: `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc`

- [ ] **Step 1: 定义只返回脱敏信息的诊断结果**

```cpp
struct ProxyDiagnosticResult {
    bool configured = false;
    bool tcp_connected = false;
    bool connect_tunnel_ready = false;
    bool tls_ready = false;
    std::string endpoint;
    std::string stage;
    std::string message;
};
```

- [ ] **Step 2: 复用真实 transport 执行 TCP、CONNECT 与 TLS**

```cpp
ProxyDiagnosticResult DiagnoseQuotaProxy();
```

诊断使用与额度查询相同的 HTTP 代理格式与认证头；响应中不得含用户名、密码、完整 Authorization 或原始 URL。

- [ ] **Step 3: 暴露认证与 CSRF 保护的诊断 API 和后台结果区**

```cpp
httpd_uri_t diagnostic_uri = {
  .uri = "/api/proxy-diagnostic", .method = HTTP_POST,
  .handler = ProxyDiagnosticHandler, .user_ctx = this
};
```

点击“检查代理连通性”后显示 TCP、CONNECT、TLS 三行状态和最终消息。

- [ ] **Step 4: 验证 sub2api 差异结论**

Run: `nc -vz -w 5 <proxy-host> <proxy-port>`

Expected: 若公网端口拒绝而 sub2api 服务部署在代理服务器本机，则诊断应明确显示 TCP 拒绝；服务器需恢复对外认证入口后真机才能通过，不通过降低 TLS 校验来规避。

### Task 5: 提交前验证、烧录和真机检查

**Files:**
- Test: `tests/host/rlcd_ui_source_contract_test.py`
- Test: `tests/host/rlcd_manager_safety_test.cc`

- [ ] **Step 1: 运行定向验证**

```bash
python3 tests/host/rlcd_ui_source_contract_test.py
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined -fno-omit-frame-pointer \
  -I main/boards/waveshare-s3-rlcd-4.2/managers \
  tests/host/rlcd_manager_safety_test.cc -o /tmp/rlcd_manager_safety_test && /tmp/rlcd_manager_safety_test
sed -n '65,156p' main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc | node --check
git diff --check
```

- [ ] **Step 2: 编译和烧录**

```bash
ninja -C build-codex
ESPPORT=/dev/cu.usbmodem2101 ninja -C build-codex flash
```

- [ ] **Step 3: 真机验收**

检查主页右上角为轮廓图标、月历为独立日期格且当日反白圆标、后台城市与 API Key 保存状态、代理诊断阶段结果。


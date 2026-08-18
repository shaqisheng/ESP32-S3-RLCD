# DEVELOPMENT_LOG

> 本文件记录本仓库所有有意义的代码修改。
> **每次代码修改后必须追加一条**（模板见末尾）。
> 倒序排列（最新在最上）。

---

## 2026-08-18 — 后台截图面板新增「复制」按钮（现代 API + copy 事件劫持降级）

- **修改内容**：`admin_server.cc` 屏幕截图面板下载按钮旁新增「复制」按钮（截图成功后与下载一起出现）。复制实现两层：①`navigator.clipboard.write(ClipboardItem)`（现代 API，仅 HTTPS/localhost 可用）；②HTTP 局域网降级——**copy 事件劫持**：`document.addEventListener('copy')` 里 `clipboardData.setData('text/html', '<img src="data:image/png;base64,...">')` + `setData('text/plain', 提示语)` + `preventDefault()`，然后 `execCommand('copy')` 触发。预览图 `<img>` 同步改为 data URL（`readAsDataURL`）承载图像数据。
- **修改原因**：用户需求——截图后一键复制到剪贴板。两次实机回归才定型：
  1. 后台是 HTTP 局域网（`isSecureContext=false`，`navigator.clipboard`/`ClipboardItem` 均为 undefined），现代 API 必然缺席；
  2. 首选的"选中 `<img>` + execCommand"路线实机粘贴只得到 alt 文字"截图预览"——该路线剪贴板只有 text/plain（alt）与指向页面的 HTML 引用（`blob:` URL 离开页面即失效）；
  3. copy 事件劫持是**非安全上下文下唯一可控剪贴板内容的途径**，且 `clipboardData.setData` 只支持文本类型（text/html、text/plain），无法写入原生图片味——富文本粘贴目标（微信/飞书/备忘录等）经 HTML 渲染出图片，纯文本目标得到明确提示语。
- **影响范围**：仅后台截图面板交互；无设备侧变化。
- **测试结果**：
  - idf.py build: ✅
  - 契约测试: ✅ 50/50（断言 `readAsDataURL` + copy 事件劫持存在，防回归）；JS 语法检查: ✅
  - 机制验证: ✅ chromium 模拟 HTTP 环境（clipboard/ClipboardItem 置空）：execCommand 触发后剪贴板 text/html 含完整 `data:image/png;base64` 图像数据、text/plain 为提示语
  - 真机验证: ⏳ 用户两次实测粘贴仍得文本味（第一次 alt 文字、第二次提示语）——判定其粘贴目标只取 text/plain 或其浏览器未把 HTML 味落板；**技术边界：HTTP 页面无法写原生图片味**
  - 兜底方案: ①右键预览图→复制图片（浏览器原生，任何应用可粘）；②`scripts/admin-local-proxy.py`（新增，零依赖 TCP 转发）——经 `http://localhost:8080/admin` 访问获得安全上下文，ClipboardItem 生效，「复制」按钮写原生 PNG
- **回滚方式**：git revert 对应 commit
- **关联文档**：`scripts/admin-local-proxy.py` 头部注释

---

## 2026-08-18 — README 增加界面实拍截图（设备 5 页 + 后台 2 张）

- **修改内容**：
  - `docs/screenshots/`：新增 7 张实拍截图——设备 5 页（`page-overview/calendar/forecast/quota/todo.png`，经 `/api/display/screenshot` 实采）+ 后台 2 张（`admin-overview/admin-quota.png`，playwright 实采）
  - `README.md`：新增「界面预览」章节（设备屏幕 5 图 + 后台 2 图，表格排版，附截图来源说明）
- **修改原因**：用户要求完善 README 并增加截图。拍摄过程：待低电量悬浮条 5 分钟无交互自动隐藏后，切页写操作与截图 GET 连续快速执行（切页会触发悬浮条交互重现，需在其重渲染前完成截图）；3 条示例待办（提交周报/买牛奶/给绿植浇水）拍摄后已删除；设备显示页已恢复为 AI 页。
- **影响范围**：仅文档与静态资源，无代码改动。
- **测试结果**：7 张截图逐张人工核验——页面内容正确、无低电量悬浮条、无敏感信息（仅账号昵称）；`git diff --check` ✅
- **回滚方式**：`git revert` 删除截图与章节即可
- **关联文档**：README.md「界面预览」

---

## 2026-08-18 — 全局低电量提示（所有页面悬浮条 + 后台横幅 + 5 分钟无交互自动隐藏 + 交互重现）

- **修改内容**：
  1. `custom_lcd_display.{h,cc}`：新增 `SetupLowBatteryOverlay()`——在 `lv_layer_top()` 创建 400×22 白底黑边悬浮条（所有页面共享，页面切换不受影响）；`UpdateLowBatteryAlertInternal()` 状态机：放电且 <20% 触发（播一次提示音，对齐原版行为），充电或 ≥25% 复位；可见窗口 = `max(触发时刻, last_activity_ms_)` 起 5 分钟（复用既有 `IDLE_TIMEOUT_MS`/"5 分钟无活动"概念），超时自动隐藏，任何用户活动重新计时显示。**复用 `last_activity_ms_` 作为交互锚点，不新增交互概念**。
  2. `data_update_task.cc`：删除旧的 sensor_label_ 反色告警（`low_battery_alert_active` 及温湿度覆盖保护逻辑），改为每周期调用 `UpdateLowBatteryAlertInternal()`。
  3. 交互接入：硬件按钮（BOOT/USER/双击/长按，本就已调 `NotifyUserActivity`，零改动）；`AdminServer::IsAuthorized` 成功且 `csrf=true`（=浏览器写操作，区别于自动轮询 GET 与 Bearer 自动化）时调 `NotifyUserActivity()`（只写时间戳，无 LVGL 调用，HTTP 线程安全）。
  4. 后台：`/api/device` 新增 `battery_low`（处于低电量区间）与 `low_battery_alert`（提示当前可见）；tab 栏下方新增全局红字横幅 `#lowBattBanner`（随 `status()` 5 秒轮询自动显隐）。
  5. 契约测试新增 `test_global_low_battery_alert_contract`（49/49 通过）。
- **修改原因**：用户需求——电量低于 20% 时所有页面+后台都有提示；无人操作只显示 5 分钟；隐藏后人工操作（后台界面/硬件按钮）再次提示。旧实现只有综合页 sensor_label_ 反色，其他页面无提示。
- **影响范围**：低电量提示从"综合页常驻反色"变为"全局悬浮条限时提示"；提示音保留（仅首次触发）；温湿度行不再被借用。无 NVS/配置变化。
- **测试结果**：
  - idf.py build: ✅（测试阈值版 + 生产版各一次）
  - 契约测试: ✅ 49/49；JS 语法检查: ✅
  - 真机验证（临时阈值 99/101 全链路）：①触发显示：综合页/日历页截图均见底部悬浮条"电量低 95% · 请尽快充电"；②后台横幅 playwright 可见；③5 分钟无交互自动隐藏（日志 `10:12:56 低电量提示隐藏` + API `low_battery_alert:false`）；④后台写操作交互重现（日志 `10:14:07 低电量提示显示` + API `true`）；⑤恢复阈值 20/25 后 94% 电量不误报（API 双 false + 截图无悬浮条）
  - 注意：验证期间发现 AI 唤醒词触发也会刷新用户活动（`data_update_task.cc:533` 既有定义，人声=人为操作），属预期语义；用省电模式关闭 AFE 后完成了确定性隐藏验证
  - app 分区使用率: 95%（无显著变化）
- **回滚方式**：git revert 对应 commit
- **关联文档**：`PROJECT_CONTEXT.md` §5.5（/api/device 字段）

---

## 2026-08-17 — 修复省电模式两处失效/反效问题 + 日志模块下拉按功能分组

- **修改内容**：
  1. **省电修复**（`power_save_manager.cc`、`sdkconfig`/`sdkconfig.defaults`）：
     - `CONFIG_PM_ENABLE=y`——此前未启用，`esp_pm_configure` 静默返回 `ESP_ERR_NOT_SUPPORTED`，"CPU 降频到 80MHz" 从未真正发生（日志里一直是"CPU 降频失败"）。
     - `esp_pm_configure` 失败改打真实错误码（`esp_err_to_name`），不再写猜测文案。
     - 新增 `LogActualPowerState()` 效果回读：进入/退出省电时打印 CPU 实际频率（`rtc_clk_cpu_freq_get_config`）与 WiFi PS 实际状态（`esp_wifi_get_ps`）。
     - 退出省电的 WiFi PS 从 `BALANCED` 改为 `LOW_POWER`——应用的日常基线就是 LOW_POWER（application.cc 激活完成即设置），设 BALANCED 反而比基线更耗电。
     - 核查 application.cc 9 处 SetPowerSaveLevel：会话/音乐/OTA 时短暂升 PERFORMANCE、结束回落 LOW_POWER，与省电模式不冲突，无需改动。
  2. **日志分组**（`admin_server.cc` 内嵌 JS）：日志 tab 模块下拉按功能 `optgroup` 分组（系统/AI 助手/天气/日历/待办/音频/网络/显示/电源·硬件/其他，10 组，`LOG_GROUPS` + `logGroupOf` 映射，未命中进"其他"）。
  3. 契约测试新增 `test_power_save_actually_applies_and_reads_back`，日志契约补分组断言（48/48 通过）。
- **修改原因**：①用户要求验证省电模式是否有实际效果——审查+真机发现 CPU 降频从未生效（esp_pm 未启用）、退出后 WiFi 比应用基线更耗电；②用户要求日志模块下拉具体到功能分组。
- **影响范围**：省电模式开启后 CPU 真正降到 80MHz（主要耗电项）；退出后 WiFi 回到应用基线 MAX_MODEM；日志 tab 下拉出现分组标题。无 NVS 结构变化。
- **测试结果**：
  - idf.py build: ✅（第一次因 `rtc_clk_cpu_freq_get_config` 在 S3 是出参形式编译失败，已修正）
  - 契约测试: ✅ 48/48；JS 语法检查: ✅
  - 真机验证: ✅ 手动开启省电→日志回读 `CPU=80MHz, WiFi PS=MAX_MODEM(2)`；手动退出→回读 `CPU=240MHz, WiFi PS=MAX_MODEM(2)`（应用基线）；浏览器验证下拉分组 `系统(4) | AI 助手(5) | 天气(1) | 音频(12) | 网络(7) | 显示(1) | 电源/硬件(2) | 其他(9)`，按模块筛选联动正常
  - app 分区使用率: 95%（无显著变化）
- **回滚方式**：git revert 对应 commit（注意 sdkconfig 本地未跟踪，需同步改回 `# CONFIG_PM_ENABLE is not set`）
- **关联文档**：无（行为修复 + 既有功能增强，架构文档不涉及）

---

## 2026-08-17 — 后台新增日志查看功能（esp_log 全局钩子 + PSRAM 环形缓冲 + 日志 tab）

- **修改内容**：
  1. 新增 `managers/system_log_buffer.h`（header-only）：`esp_log_set_vprintf` 全局钩子，把全系统日志（全部模块：天气/AI/日历/待办/Wi-Fi/音频/系统等）写入 PSRAM 环形缓冲（256 条 × 约 172B ≈ 45KB PSRAM，不占内部 SRAM）。每条含启动毫秒、级别、TAG、文本；连续重复行折叠为 `repeat` 计数（防 AFE 每 30ms 告警刷屏）；`key=/token=/password=/secret=` 参数值打码为 `****` 防 secret 经日志接口泄露。JSON 序列化在自旋锁外（锁内只拷贝单条，避免临界区 malloc）。
  2. `waveshare-s3-rlcd-4.2.cc` 构造函数首行 `SystemLogBuffer::Install()`（非网络服务，尽早覆盖启动日志）。
  3. `admin_server.{h,cc}`：`GET /api/logs?after=<seq>&limit=N`（Cookie 读鉴权，增量拉取，chunked 发送）；kAdminHtml 新增「日志」tab：模块/级别筛选、3 秒自动刷新开关、手动刷新、黑底终端风日志区（时间|级别|模块|信息），风格与设计稿 `docs/mockups/admin-logs-v1.html` 一致。
  4. 契约测试新增 `test_logs_tab_and_ring_buffer_contract`（47/47 通过）；内嵌 JS 经 `node --check`。
- **修改原因**：用户需求——后台可查看系统所有日志（含且不限于天气/AI/日历），带时间、模块、日志信息。
- **影响范围**：新增只读功能，不改任何现有行为；UART 日志输出不变（钩子先原样透传）。
- **测试结果**：
  - idf.py build: ✅ ×2（第二次为 %lld 修复）
  - 契约测试: ✅ 47/47；JS 语法检查: ✅ node --check
  - 真机验证: ✅ 烧录后串口 60 秒无 abort/reboot（free ~95KB / minimal ~89KB）；`GET /api/logs` 返回合法 JSON（seq/t/lv/tag/text/r 字段齐全，dropped/capacity 正确）；浏览器端到端（playwright 登录 → 日志 tab）：600 行渲染、模块下拉真实 TAG、自动刷新增量追加、截图确认风格
  - 踩坑修复：`CONFIG_NEWLIB_NANO_FORMAT=y` 不支持 `%lld`（输出 "ld" 并错位 varargs）→ epoch 改 `%u` 打印
  - app 分区使用率: 95%（无显著变化）
- **回滚方式**：git revert 对应 commit
- **关联文档**：`PROJECT_CONTEXT.md` §5.5（/api/logs）；`ARCHITECTURE.md` §6.2；设计稿 `docs/mockups/admin-logs-v1.html`

---

## 2026-08-17 — 恢复综合页小智区左分割竖线（533c44a 误删）

- **修改内容**：`weather_ui.cc` 恢复 `kEmotionWidth`(76) 常量与 `assistant_divider`
  （chat_card_ 内 x=76、y=10、1×96 黑色竖线），+7 行。
- **修改原因**：用户反馈综合页下方三块布局的左边分割竖线消失。定位为 533c44a
  （精简概览页 LVGL 对象）把 `assistant_divider` 当"装饰对象"删除——该线是表情区（0-76px)
  与小智文本区（84px 起）的视觉分界，非纯装饰。同次删除的 `todo_rule`（待办标题下横线）
  用户未提及，暂不恢复。
- **影响范围**：综合页下方恢复 表情 | 小智 | 待办 三分块视觉。
- **测试结果**：
  - 契约测试： ✅ 46/46（无此对象断言，无需改动）
  - `git diff --check`: ✅
  - idf.py build: ✅（app 分区余量 5%，0x38e20）
  - 真机验证: ✅ 串口 70 秒无异常，minimal SRAM 58.6KB；截图确认 x=76 竖线恢复，
    下方恢复 表情 | 小智 | 待办 三分块（同屏可见和风实时数据也在正常刷新：晴 30° 东东北风2级）
- **回滚方式**：`git revert` 本次提交
- **关联文档**：无

---

## 2026-08-17 — 天气源整体切换为和风天气新版 API（实时 + 七日单源）

- **修改内容**：
  - `managers/weather_manager.{h,cc}`：高德（实时）+ Open-Meteo（七日）双源 → 和风新版 v1 单源。
    请求改为 `GET https://{qw_host}/weather/v1/current/{lat:.2f}/{lon:.2f}` 与
    `.../daily/{lat}/{lon}?days=7&localTime=true`，认证走 `X-QW-Api-Key` 请求头（不进 URL/日志）。
    配置项 `amap_adcode`/`amap_key` → `qw_host`/`qw_key`；构造时一次性 `EraseKey` 清理 NVS 遗留高德配置。
    解析适配新结构：`condition{text,code}`、`temperature{value,unit}`、湿度 [0,1]×100、
    `wind.direction.compass` 英文方位码→中文；实时接口无时间字段，"上次更新"用本地请求时刻；
    七日日期取 `forecastStartTime` 前 10 字符。响应缓冲 8KB→12KB（PSRAM）。
    失败语义：实时成功但七日失败 → 保留旧预报快照报失败重试，不再降级三日。
  - `forecast_ui.cc`：`WeatherIcon()` 数字分支 WMO 码 → 和风码
    （100 晴 / 101-103 多云 / 104 阴 / 302-304 雷 / 300-399 雨 / 400-499 雪 / 500-515 雾霾沙尘），
    中文关键字兜底不变，MCP 外部写入路径不受影响。
  - `managers/admin_server.cc`：后台"城市天气"面板高德 Key 单字段 → 和风 API Host + API Key 双字段
    （Host 明文回显、Key 只写不读）；`saveWeather`/`loadExtras` 同步；城市目录 lat/lon 复用，adcode 不再使用。
  - `waveshare-s3-rlcd-4.2.cc` 注释、契约测试 4 项重写（46/46 通过）。
- **修改原因**：高德免费版预报只有 3 天、Open-Meteo 为境外服务；目标单源国产化。
  和风新版每月 5 万次免费、文档齐全。注意旧公共域名 devapi/api.qweather.com 2026 年起已 403，
  必须用控制台分配的个人 API Host。
- **影响范围**：`/api/weather` 契约变化（`has_amap_key`→`has_qw_key`、新增 `qw_host` 回显）；
  **升级后需在后台重新配置和风 API Host + Key 否则天气不更新**；设备屏 UI 结构与字段格式不变。
- **测试结果**：
  - 契约测试： ✅ 46/46（4 项天气断言重写 + gzip 解压防回归断言）
  - 官方示例 JSON 字段路径校验（Python 模拟 cJSON 遍历）: ✅ current/daily 全部字段可取
  - 图标代码区间 vs 官方 weather-conditions.csv(55 个代码）: ✅ 仅 900/901/999 落 Unknown（预期）
  - `git diff --check`: ✅；新增长行已全部收至 100 列内
  - clang-format: ⚠️ 未整文件执行——HEAD 文件本身不符合任何 clang-format 19-22 版本的输出
    （`.clang-format` 含 19+ 专属键 `ExceptShortType`），整文件格式化会产生数百行无关 diff，
    违反 §10.3；本次改为手写贴合现有风格。如需统一格式化应另开 chore 提交。
  - idf.py build: ✅（app 分区余量 5%，0x38e80）
  - 真机验证: ✅ 烧录后串口 75 秒无异常（stack overflow/abort/NO_MEM/SPI 均无），
    minimal SRAM 58.9KB；POST /api/weather/refresh 触发后诊断返回
    "和风实时天气与七日预报更新成功"；七日页截图确认 7 天数据/图标/中文风向全部正确
  - **真机抓出两个文档之外的问题**：
    1. 和风网关**强制 gzip**（实测无视 `Accept-Encoding: identity`，连 401 错误页都 gzip），
       首版固件全部解析失败。修复：手工解析 gzip 头 + ROM `tinfl` 裸 deflate 解压到 16KB PSRAM 缓冲。
    2. 直接调 `tinfl_decompress_mem_to_mem` 导致 **activation 任务栈溢出重启循环**
       （该帮助函数把 ~3.3KB 的 `tinfl_decompressor` 状态结构放任务栈）。
       修复：改用低层 `tinfl_decompress` + 状态结构一次性分配 PSRAM（构造函数随缓冲区一起）。
    两个修复均已烧录验证；契约测试已加 gzip 断言防回归。
- **回滚方式**：`git revert` 本次提交；NVS 中被 EraseKey 的高德配置需重新在后台填写
- **关联文档**：ARCHITECTURE.md（架构图/目录/NVS 表三处）、PROJECT_CONTEXT.md（§5.2 模块表、§5.5 API 表、§6 功能列表）

---

## 2026-08-17 — Info 页内存改"使用/全部" + 后台 SRAM 显示与 info 页对齐

- **修改内容**：
  1. `info_ui.cc` 内存区：`SRAM/PSRAM` 从"可用/总量 + 可用百分比"改为"使用/总量 + 使用百分比"，状态词（正常/紧张/危险）保留、仍按可用字节判定（设计稿经用户确认）。
  2. `managers/admin_server.cc` `/api/device`：`free_heap`/`minimum_free_heap` 从 `esp_get_free_heap_size()`（SRAM+PSRAM 混合值，约 7.3MB）改为 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` / `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`，与 info 页和串口 SystemInfo 日志同源。
  3. `tests/host/rlcd_ui_source_contract_test.py` 同步 4 个过期契约（AGENTS §7.2 补漏）：overview 状态条合并为 `overview_status_label_` 后更新两个 overview 测试；AI 卡片进度条下方去掉周标签改用 `"%d%% %s %s"` 紧凑格式；天气 adcode 改为城市下拉自动生成（`cityCatalog`）后删除"高德城市 adcode（必填）"旧断言。
- **修改原因**：①用户要求 info 页内存按"使用/全部"展示；②后台设备控制的"可用 SRAM/最低 SRAM"显示约 7.3MB，与 info 页（纯内部 SRAM）严重不一致——根因是 `esp_get_free_heap_size()` 返回全部 heap 混合值（info_ui.cc:103 注释早就标注过这个坑，后台踩了同一个）。
- **影响范围**：info 页内存行显示格式；后台设备控制两个 SRAM 字段的数值（变小到真实的几十 KB，危险/紧张标红判定从此真正生效）；无行为逻辑变化。
- **测试结果**：
  - idf.py build: ✅
  - 契约测试: ✅ 46/46（修复前 3 FAIL + 1 ERROR）
  - 真机验证: ✅（烧录后 60 秒无 abort/reboot；串口 free 66019/minimal 59123；`/api/device` free_heap=67.7KB、minimum=55.0KB 与串口一致；info 页截图显示 `SRAM 正常 320KB/387KB (82%)`，387-320≈67KB 与后台一致）
  - app 分区使用率: 95%（无变化）
- **回滚方式**：git revert 对应 commit
- **关联文档**：无（显示层调整，架构/上下文文档不涉及）

---

## 2026-08-17 — SRAM 优化：LVGL 堆整体迁往 PSRAM（内部 SRAM 3% → 17%）

- **修改内容**：
  1. **P2（核心）**：新增 `lvgl_mem_psram.cc`——实现 LVGL 9.4 `LV_USE_CUSTOM_MALLOC` 要求的 5 个符号（`lv_malloc_core`/`lv_realloc_core`/`lv_free_core` + 空 `lv_mem_init`/`lv_mem_deinit`），把 LVGL 全部动态内存（对象/样式/字符串/渲染临时块）重定向到 PSRAM（`MALLOC_CAP_SPIRAM`，无内部 SRAM 兜底）；`sdkconfig` + `sdkconfig.defaults` 从 `CONFIG_LV_USE_CLIB_MALLOC=y` 切换为 `CONFIG_LV_USE_CUSTOM_MALLOC=y`。
  2. **P0**：`weather_ui.cc` 状态条 3 个 label（wifi 符号 + 电池符号 + 百分比）合并为 1 个 `overview_status_label_`（不能叫 `status_label_`，那是 LcdDisplay 基类成员）；删除 3 个装饰对象（`assistant_divider`、`todo_rule`、`low_battery_popup_`+`low_battery_label_`）；低电量告警改为复用 `sensor_label_` 反色显示（白底黑字"电量低，请充电"），恢复时强制温湿度重写。`data_update_task.cc` 对应改为脏标记 + 末尾重组一次状态条字符串。`custom_lcd_display.h` 删除 3 个从未创建过的死字段（`wifi_icon_img_`/`battery_icon_img_`/`battery_pct_label_`）。
  3. **P1**：`weather_manager.cc`（16KB 响应缓冲）与 `quota_manager.cc`（HttpGet 16KB 体缓冲）分配失败时不再 fallback 到内部 SRAM，改为报错返回——宁可本次刷新失败也不能耗尽内部 SRAM。
- **修改原因**：设备信息页显示内部 SRAM 只剩 3%（约 11KB），最低水位一度 3411B，处于随时 OOM 的危险状态。根因：`CONFIG_LV_USE_CLIB_MALLOC` 让 LVGL 走标准 malloc（内部 SRAM 优先），6 个页面的 LVGL 对象长期占用内部 SRAM（曾出现 10 个 label 把余量压到 1219B）。
- **影响范围**：全设备内存布局。用户可见：界面无变化（截图验证 AI 页/综合页渲染一致）；低电量弹窗改为顶部传感器行反色提示；稳定性预期显著提升。LVGL 对象现驻 PSRAM，刷屏 buffer 本来就是显式 PSRAM 分配（`custom_lcd_display.cc:86`），flush 路径不经过 LVGL 堆，SPI DMA 不受影响。
- **测试结果**：
  - idf.py build: ✅（app 分区 5% free，无变化）
  - 契约测试: N/A（未改 UI 源码契约）
  - 真机验证: ✅（烧录后观察 75 秒无 abort/reboot/NO_MEM；`free sram` 稳定在 65–83KB，**minimal sram 58735B**（优化前 3411B），远超 ≥20KB 目标；AI 页与综合页截图渲染正常，合并状态条显示"📶 🔋 92%"正确，后台登录/截图/切页 API 正常）
  - app 分区使用率: 95%（与之前一致）
- **回滚方式**：`git checkout -- sdkconfig.defaults main/boards/waveshare-s3-rlcd-4.2/lvgl_mem_psram.cc`（未提交时）；已提交则 revert 对应 commit。注意 `sdkconfig` 本地未跟踪，回滚需同时把 `CONFIG_LV_USE_CUSTOM_MALLOC` 改回 `CONFIG_LV_USE_CLIB_MALLOC=y` 并重新编译。
- **关联文档**：`ARCHITECTURE.md` §7 不变量（LVGL 对象预算条款）；`AGENTS.md` §4.1 板级目录结构
- **注意**：`xcrun clang-format`（Apple clang-format 21）会按 `.clang-format` 把仓库现存风格（单行短 if 等）全部重排，产生大量格式化噪音——本仓库代码并未严格按 `.clang-format` 格式化过，**不要对存量文件运行 clang-format 21**，手改保持周边风格即可（本次已回退一轮噪音）。

---

## 2026-08-17 — 删除 116 个与本硬件无关的上游板级目录

- **修改内容**：删除 `main/boards/` 下除 `waveshare-s3-rlcd-4.2/`（本板）和 `common/`（共享层，本板依赖）之外的全部 116 个上游继承板目录（如 `esp32-s3-touch-amoled-1.8`、`kevin-box-2`、`m5stack-*` 等），共 543 个被跟踪文件。同步修正 `AGENTS.md` 中三处"119 个板目录"的过时表述。
- **修改原因**：用户确认这些目录与 Waveshare ESP32-S3-RLCD-4.2 无关，要求清除。删除前已验证：①构建只编译 `boards/${BOARD_TYPE}`（=waveshare-s3-rlcd-4.2）+ `boards/common`（`main/CMakeLists.txt:63-66,652-659`）；②本板源码无指向兄弟板目录的交叉 include；③`scripts/release.py` 与 CI 按目录动态发现板子，删除后仅矩阵缩小，不会断；④其他目录无任何硬编码引用这些路径。
- **影响范围**：仅仓库体积与可读性；不参与编译的代码，固件产物零变化。`main/Kconfig.projbuild` 中各板 `CONFIG_BOARD_TYPE_*` 选项保留（不引用目录，无害）。
- **测试结果**：
  - idf.py build: N/A（删除的目录从不参与编译，构建输入无变化；如需复验请告知）
  - 契约测试: N/A（未动本板源码）
  - 真机验证: N/A（固件无变化）
- **回滚方式**：`git checkout -- main/boards/`（删除尚未提交时）或从上游 `78/xiaozhi-esp32` 恢复
- **关联文档**：`AGENTS.md` §1、§2.5、§6.1

---

## 2026-08-14 — 修复“无法连接服务”错误提示永久残留小智区域

- **修改内容**：`custom_lcd_display.cc` `CustomLcdDisplay::SetChatMessage`——把原来的
  `if (!content || strlen(content) == 0) return;` 改为：空内容时**真正清空** `chat_status_label_`
  （停止滚动动画 + `SetShowingSystemInfo(false)` + 清空文本 + 恢复 WRAP/LEFT_MID 布局后 return）；
  非空内容路径不变。
- **修改原因**：用户反馈综合页小智区长期显示“无法连接服务，请稍后再试”，但实际已联网。
  串口日志证实 MQTT 连接/激活完全正常（`MQTT: Connected to endpoint` → `Activation done`）。
  根因：上游所有清除路径（`DismissAlert`、音频通道关闭）都靠 `SetChatMessage("system", "")`
  传空串清空，而本板重写版直接丢弃空内容 → 任何一次瞬时连接失败的提示都会永久残留到重启。
- **影响范围**：仅综合页小智区域文本的清除时机；对话结束/错误恢复后区域现在会正确清空；
  不影响其他页面、API、NVS。
- **测试结果**：
  - idf.py build: ✅（app 分区余量 5%，0x3cd40）
  - `git diff --check`: ✅
  - 契约测试: ⚠️ 44/46；2 个失败为 HEAD 上既有（`68377eb` 等近期 AI 卡片提交未同步断言），与本次无关
  - 真机烧录 + 串口 75s: ✅ 无 abort/reboot/NO_MEM/SPI 失败；MQTT 12s 连上
  - 屏幕截图（`/api/display/screenshot`）: ✅ 综合页小智区正常显示“待命”，无残留错误文字
  - 对话结束后区域自动清空: ⏳ 需用户实测（按 BOOT 对话 → 结束后区域应变空）
  - 注意：本次烧录后 minimal sram 低至 3859（额度 TLS 刷新高峰期），属既有基线而非本次回归
- **回滚方式**：`git revert` 本次提交
- **关联文档**：无架构变化；附带发现（未修，另立任务）：①后台 3 条 Wi-Fi 路由注册失败
  （`ESP_ERR_HTTPD_HANDLERS_FULL`，max_uri_handlers 需调大）②2 个契约测试断言过期

---

## 2026-08-13 — 重写项目 README + 新建功能文档（文档对齐代码现状）

- **修改内容**（纯文档，无代码改动）：
  - `README.md`：由上游英文版重写为本 fork 的中文项目 README（定位、功能速览、硬件、快速开始、按键、文档导航、上游关系）
  - `docs/功能文档.md`：**新建**，整理当前全部已实现功能：5 个显示页、按键矩阵、AI 额度（provider/刷新/代理/显示配置）、天气（高德+Open-Meteo）、日历/农历、待办+REST API+语音备忘、MCP 工具清单、后台认证与**完整路由表（约 40 条）**、持久化/分区、安全设计、已知限制
  - `main/boards/waveshare-s3-rlcd-4.2/README.md`：重写（原内容讲番茄钟/备忘录/QWeather，已严重过时），现为硬件+按键+构建烧录+故障排查，指向功能文档
  - `README_zh.md` / `README_ja.md`：`git rm` 删除（纯上游翻译版，经用户确认）
  - `PROJECT_CONTEXT.md`：修正 4 页→5 页（§1.4/§2/§5.2/§6.1）、§5.5 路由表更新并指向功能文档、§7 文档准确度表更新
- **修改原因**：根 README 仍是上游小智项目介绍，与本 fork 完全脱节；功能散落在多份部分过时的文档中，缺少一份以代码为准的功能清单
- **影响范围**：仅文档；不含代码、配置、构建变化。文档中 BOOT 上下文分发、Wi-Fi 管理路由、截图、AI 页显示配置等均按当前代码（含 working tree 未提交改动）核实
- **测试结果**：
  - idf.py build: N/A（无代码改动）
  - 契约测试: N/A
  - 真机验证: N/A
- **回滚方式**：`git revert` 本提交；被删的上游 README 可从 git 历史或上游仓库恢复
- **关联文档**：README.md、docs/功能文档.md、板级 README、PROJECT_CONTEXT.md §7

---

## 2026-08-12 — AI 卡片信息分层重构（5H/周额度拆开显示）

- **修改内容**：`quota_ui.cc` 匿名 namespace 新增 3 个 helper，重写 `RenderQuotaPageInternal` 主体：
  - 新增 `FindShortTier(card)`：找 `label == "5H"`，无则返 `SIZE_MAX`
  - 新增 `FindLongTier(card)`：优先 `label == "周" || "7D"`，否则首个非 5H，保证返回有效索引
  - 新增 `FormatResetAbsolute(reset_at, out, size)`：同月 `"15日15时"`，跨月 `"9月2日15时"`（`localtime_r` 取 `tm_mday/tm_hour`）
  - 重构 `FormatReset` → `FormatResetCountdown`（去掉 `"重置"` 后缀，输出 `"3天2小时后"`）
  - `PrimaryTier` 改为"5H 优先（FindShortTier），无则 FindLongTier"——之前是"剩余%最低"
  - render 主体改为三段：
    - 大数字 = `primary.remaining` %（5H 优先，无则周）
    - 进度条 = `weekly.remaining` %（固定周/7D）
    - 进度条下方 = `"周 N% · 15日15时 · 3天2小时后"`（周剩余% + 重置绝对时间 + 倒计时）
  - 新增 `test_quota_card_layout_separates_primary_and_weekly_tiers` 契约测试
- **修改原因**：用户明确要求三层信息拆分：(1) 主数显 5H 优先（5H 是用户能感知紧迫的指标），无 5H 才周；(2) 进度条固定显示周额度（长周期趋势）；(3) 进度条下方用周额度的剩余%、绝对重置时间、倒计时三段。原代码三个 UI 元素（大数字/进度条/下方文字）都跟着同一个 PrimaryTier（最低剩余%）走，信息冗余且不区分 5H 与周。
- **影响范围**：只动 `quota_ui.cc` 渲染层 + 测试；不影响 quota_manager 解析、API、NVS、其他页面。Codex（5H+7D）、Kimi（5H+周）、GLM（5H+周）都按预期分层；DeepSeek（只有余额）走 fallback：primary=weekly=同一 tier，bar 隐藏，下方显示 `"余额"` label。
- **测试结果**：
  - Python 契约测试: ✅ 46 个全过（新增 1 个）
  - `git diff --check`: ✅
  - idf.py build: ✅ 增量
  - 真机烧录: ✅ 设备在线（HTTP 200）
  - AI 页面实际显示: ⏳ 由用户实测（需触发一次刷新有数据）
- **回滚方式**：`git revert`；或恢复 `PrimaryTier` 的"最低剩余%"逻辑 + `FormatReset` 旧名
- **关联文档**：与 [[2026-08-12-统一4张AI卡片背景为白色]] 同属 AI 页面视觉/信息层重构

---

## 2026-08-12 — 统一 4 张 AI 卡片背景为白色（去掉上下排交替）

- **修改内容**：`quota_ui.cc` 的 `SetupQuotaUI()` for 循环里：
  - 去掉 `const bool light = row == 0;` 和三元运算（之前上排 light=true 白底、下排 light=false 黑底）
  - 直接硬编码 `foreground = lv_color_black()` / `background = lv_color_white()`
  - 同时把 `border_width(card, light ? 0 : 1, 0)` 改为 `border_width(card, 0, 0)`（4 张都无边框）
  - 加注释说明改动原因
  - 新增 `test_quota_cards_use_unified_white_background` 契约测试（断言不再有 `row == 0` / `const bool light`，必须硬编码黑白）
- **修改原因**：用户反馈"AI 卡片的背景没有统一，上下需要统一"。原代码上排（卡片 0/1）白底、下排（卡片 2/3）黑底，2x2 布局看起来黑白混搭。统一为 4 张全白底黑字后视觉一致。
- **影响范围**：只动 `SetupQuotaUI()` 内的循环开头几行；不影响 render 逻辑、API、NVS、其他页面。quota_page_ 本身仍是黑底，4 张白卡片在黑底页面上自然 contrast，可见性不丢。
- **测试结果**：
  - Python 契约测试: ✅ 45 个全过（新增 1 个）
  - `git diff --check`: ✅
  - idf.py build: ✅ 增量
  - 真机烧录 + 串口 30s: ✅ 0 异常
  - 设备屏幕实际 4 张卡片背景统一: ⏳ 由用户实测
- **回滚方式**：`git revert`；或恢复 `const bool light = row == 0;` + 三个三元运算
- **关联文档**：与 [[2026-08-12-统一4个AI-logo为白底]] 配套——logo 资源 + 卡片背景两层都改完，AI 页面整体视觉风格才真正统一

---

## 2026-08-12 — 统一 4 个 AI logo 为白底（GLM/DeepSeek 改回原版）

- **修改内容**：
  - `assets/icons/ui_img_quota_glm.c`：反相字节数组（每个字节 0x00↔0xff），去掉头部 `INVERTED_FOR_QUOTA_PAGE:` 注释前缀，改为 `Official provider mark`
  - `assets/icons/ui_img_quota_deepseek.c`：同上
  - `tests/host/rlcd_ui_source_contract_test.py`：原 `test_glm_and_deepseek_logos_are_inverted_for_the_quota_page` 改写为 `test_all_four_quota_logos_share_white_background`，断言 4 个 logo 都不是 inverted + 都是白底（首字节 0xff）+ 头部统一注释格式
- **修改原因**：用户反馈 AI 页面 4 个 logo 背景色不统一。原状态：Codex/Kimi 白底黑字、GLM/DeepSeek 黑底白字（inverted）。用户两个要求（"统一四个背景色为白色"+"glm和ds的logo改回原版"）其实是同一件事——把 GLM 和 DeepSeek 反相回白底黑字，与 Codex/Kimi 一致。ProviderLogo() 是纯查表，无代码层变换，所以直接改 .c 文件即可。
- **影响范围**：只动 2 个静态资源文件 + 1 个测试；不影响任何 C++ 逻辑、API、NVS。Codex/Kimi 不变。
- **测试结果**：
  - Python 契约测试: ✅ 44 个全过（含改写的 logo 白底断言）
  - `git diff --check`: ✅
  - idf.py build: ✅（增量，只重编 2 个 .c 资源）
  - 真机烧录 + 串口 45s: ✅（0 异常；minimal sram 20963 健康）
  - 设备屏幕实际显示 4 个 logo 统一白底: ⏳ 由用户在 AI 页面实测
- **回滚方式**：`git revert`；或对 GLM/DeepSeek 再次反相字节数组（XOR 0xff 是自反操作）
- **关联文档**：无文档影响（资源文件级修改）

---

## 2026-08-12 — 修复 Kimi/GLM 配额重置时间显示不准

- **修改内容**：
  - `quota_manager.cc` 匿名 namespace 新增 `ParseIso8601ToUnix(s)`（手算 Howard Hinnant days_from_civil，避免依赖 timegm）和 `ParseResetAt(obj, key)`（自动识别 3 种格式：JSON 数字→/1000；ISO 8601 字符串→直接解析；数字字符串→atof+/1000）
  - Kimi 解析：`JsonNumber(detail, "resetTime") / 1000` → `ParseResetAt(detail, "resetTime")`；usage 路径同样改
  - GLM 解析：`JsonNumber(item, "nextResetTime") / 1000` → `ParseResetAt(item, "nextResetTime")`（防御性，未来 API 变更也兼容）
  - `tests/host/rlcd_ui_source_contract_test.py` 新增 2 个契约测试（要求 ParseIso8601ToUnix 存在、Kimi/GLM 用 ParseResetAt；算法自检对 2026-08-11T15:53:05Z 求 Unix 秒）
- **修改原因**：用户反馈 AI 页面 Kimi 和 GLM 重置时间不准。研究官方实现（opencode-glm-quota、cc-switch、CodexBar、getpaseo/paseo #3024 真实响应）后定位根因：
  - **Kimi `resetTime` 实际是 ISO 8601 字符串**（如 `"2026-08-11T15:53:05.519605Z"`），但代码当 ms 数字处理。`JsonNumber` 对字符串调 `atof`，遇 `-` 停止返回 2026，再 `/1000 = 2`（Unix 秒 2 = 1970-01-01 00:00:02），`FormatReset` 算"已过期" → 永远显示"即将重置"
  - GLM `nextResetTime` 是 Unix 毫秒数，原代码正确；但统一改用 `ParseResetAt` 防御未来变更
- **影响范围**：只动 quota 解析逻辑；API 路由、manager 接口、UI 显示、NVS schema、其他 provider（codex/deepseek/generic-json/manual）均不变。
- **测试结果**：
  - Python 契约测试: ✅ 44 个全过（含新增 2 个）
  - 算法正确性自检: ✅ 6 个边界用例（真实 Kimi 响应、年初/末、闰日 2024-02-29、epoch）全部与 Python datetime 权威值一致
  - `git diff --check`: ✅
  - idf.py build: ✅（增量；xiaozhi.bin 4891920 → 4891520，-400 字节，因编译器优化）
  - 真机烧录 + 串口 60s: ✅（0 异常；完整启动；Open-Meteo 七日天气正常）
  - 真实 Kimi/GLM 刷新后显示正确重置时间: ⏳ 由用户在后台实测（需要用户已配置 Kimi/GLM 账号且触发一次刷新）
- **回滚方式**：`git revert` 本次 commit；或恢复 3 处 `ParseResetAt(...)` 调用为原 `JsonNumber(...)/1000`（Kimi 仍会回到 buggy 状态，GLM 仍正确）
- **关联文档**：
  - 上游参考：[opencode-glm-quota](https://github.com/guyinwonder168/opencode-glm-quota/blob/main/src/index.ts)、[cc-switch discussion #1038](https://github.com/farion1231/cc-switch/discussions/1038)、[getpaseo/paseo #3024 真实 Kimi 响应](https://github.com/getpaseo/paseo/issues/3024)、[CodexBar docs/zai.md](https://github.com/steipete/CodexBar/blob/main/docs/zai.md)
  - 遵守 AGENTS §5.2（不动 manager 接口；不破坏既有 provider 解析）

---

## 2026-08-12 — 后台 UI 重构：Tab 分组 + 按钮 loading 态 + 待办模态框

- **修改内容**：仅改 `admin_server.cc` 的 `kAdminHtml` 字符串（45 行 diff）
  - CSS：新增 `.tabs/.tab/.tab-pane`（黄下划线激活态，复用 `--signal`）、`button.loading/success` + `.spinner`、`.modal-bg/.modal/.modal-actions`
  - HTML：废弃原 `.grid > aside + article` 两栏布局；改为顶部 4 个 Tab + 4 个 `.tab-pane`
    - **概览**：设备控制（含切页按钮）+ 页面编排 + 运行状态
    - **AI 账号**：账号管理 + AI 自动刷新 + 代理诊断
    - **集成**：城市天气 + 日历节假日 + 待办
    - **系统**：待办 API Token
  - 新增待办模态框 HTML（替换 6 处 `prompt()` 调用）
  - JS：新增 `switchTab(name)` / `withFeedback(btn, fn, msg)`（按钮 loading→✓→恢复，乐观反馈）/ `openTodoModal(id)` / `closeTodoModal()` / `submitTodoModal()`；改写 `addTodo` 和 `editTodo` 调用模态框
  - 模态框支持 Esc 关闭 + 点击背景关闭
- **修改原因**：用户反馈"样式不好看 + 交互不流畅"。经过 8 轮 grill-me 明确真实需求：(1) 解决"点了不知道成功没"——根因是缺乐观更新和按钮态；(2) 替换待办 `prompt()` 弹窗；(3) 信息层级混乱——8 个区块一滚到底。决策路径：用户先选 Apple Settings-like → v1 mockup 出来后"不如原版" → 改回原版极客风（黑/纸白/荧光黄/monospace/硬阴影），只加 Tab + 交互改进，**不动整体审美**。
- **影响范围**：
  - 保留：CSS 变量、配色、字体、按钮硬阴影、header 黄色分隔条、所有组件级样式（page-row/quota/device-stats 等）
  - 新增：Tab 栏、模态框、按钮 loading/success 视觉态、乐观反馈机制
  - 不变：所有 API 路由、所有 manager 代码、所有 NVS schema、`api()` 封装、`status()` 轮询逻辑、cityCatalog/providerLogos 数据
- **测试结果**：
  - idf.py build: ✅（增量，9 步；xiaozhi.bin `0x4a9050` → `0x4aa1e0`，+4528 字节）
  - app 分区使用率: 6%（无变化）
  - kAdminHtml: 31345 → 35767 字节（+4422，在 +20KB 预算内）
  - `git diff --check`: ✅
  - 真机烧录 + 串口观察 65 秒: ✅（0 异常关键词；完整启动 NTP→天气→激活→MQTT→音频→唤醒词→Idle；Open-Meteo 七日天气正常加载；minimal sram 2855-19427 范围波动，与基线一致）
  - 后台 HTML 静态验证: ✅（HTTP 200；1 个 .tabs；4 个 .tab-pane；4 个 data-tab；1 个 #todoModal；withFeedback/switchTab/openTodoModal 函数都注册；**0 处残留 `prompt(`**）
  - 浏览器实际操作（tab 切换、按钮 loading、模态框增删改）: ⏳ 由用户在浏览器 `http://192.168.40.116:8080/admin` 实测
- **回滚方式**：`git revert` 本次 commit；或恢复 kAdminHtml 字符串（所有改动集中在 raw string 内，不影响 C++ 逻辑）
- **关联文档**：
  - `docs/mockups/admin-redesign-v1.html`（被否决的 Apple Settings 风格，保留作参考）
  - `docs/mockups/admin-redesign-v2.html`（采纳的"原版+Tab"草稿，本次实施基准）
  - 遵守 AGENTS §5.2（不破坏 ARCHITECTURE §7 不变量；不引入新依赖；不改 manager 代码）

### 设计决策小记（grill-me 留档）

1. **保留原版极客风**：用户在抽象选项里勾选 "Apple Settings-like"，但看到 v1 mockup 后明确表示"不如原版"。教训：抽象偏好问题需要用具体 mockup 验证，不能直接信抽象回答。
2. **不引入 gzip embed**：原本"bin 净增长 ≤ +20KB"是约束。最终只 +4.5KB，远低于预算，gzip embed 没必要（增加复杂度换不回多少收益）。
3. **保留多保存按钮工作流**：grill-me 中用户明确**未选** "多个独立保存按钮"为痛点，故不强行做"全局保存"或"自动保存"。
4. **保留组件细节不抛光**：用户明确**未选** "组件粗糙"，所以不动 button/input/border-radius 等细节样式。

---

## 2026-08-11 — 后台新增"切换显示页面"功能

- **修改内容**：
  - `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.h`：加 `DisplaySwitchHandler` 静态方法声明
  - `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc` 4 处改动：
    1. 匿名 namespace 新增 `ScheduleDisplaySwitch(mode)` 辅助函数（照搬 `ScheduleTodoDisplayRefresh` 模式，经 `Application::Schedule` 投递到主任务调 `CustomLcdDisplay::SwitchTo*Page` 或 `CycleDisplayMode`）
    2. `routes[]` 数组注册 `POST /api/display/switch`（路由总数 28→29，仍 ≤ `max_uri_handlers=34`）
    3. 新增 `DisplaySwitchHandler` 实现：`IsAuthorized(req, true)` Cookie+CSRF 鉴权 → `ReadBody` → cJSON 解析 `mode` → 白名单校验（toggle/overview/weather/calendar/forecast/quota）→ `ScheduleDisplaySwitch` → 返回 `{"ok":true,"mode":"..."}`
    4. `kAdminHtml` 设备控制区音量行下方新增"显示页面"按钮组（综合/日历/天气/AI/下一页）+ hint 文字；JS 新增 `switchPage(mode)` 函数
- **修改原因**：用户要求后台支持远程切换显示页面（类似 USER 按钮单击）。mode 白名单与现有 MCP 工具 `self.disp.switch`（`waveshare-s3-rlcd-4.2.cc:358-367`）完全一致，保证 Web/语音/MCP 三入口语义统一。
- **影响范围**：
  - 后台前端：设备控制区多一组按钮，登录后点击立即切页
  - API：新增一条路由，仅接受 Cookie+CSRF（与 `/api/pages`、`/api/quotas` 等纯 Web 路由一致；**未开放 Bearer Token 自动化**——是设计决策，切页命令型操作不需要自动化）
  - 不改 `custom_lcd_display.{h,cc}`、板级入口、QuotaManager、分区表、Kconfig、依赖
- **测试结果**：
  - idf.py build: ✅（增量，仅编译 admin_server.cc + 板级入口；xiaozhi.bin `0x4a8830` → `0x4a9050`，+528 字节，app 分区仍剩 6%）
  - `git diff --check`: ✅（无 whitespace 错误；本机未安装 clang-format，未跑）
  - 真机烧录 + 串口观察 70 秒: ✅（0 个 abort/reboot/NO_MEM/watchdog；启动流程完整：NTP→天气 prefetch→小智激活→MQTT→音频→唤醒词→Idle；minimal sram 稳定 3519，与基线一致）
  - API 鉴权: ✅
    - 未登录 POST `/api/display/switch` → HTTP 401 `{"error":"未登录"}`
    - 未登录 + 无效 mode → 仍 401（鉴权优先于参数校验，不泄露路由存在性）
    - GET `/admin` → 200（HTML 含 5 个 `switchPage('xxx')` 按钮、"显示页面"/"立即切换"文字）
  - 登录后实际切页: ⏳ 由用户在浏览器访问 `http://192.168.40.116:8080/admin` 自行验证（curl 无法登录因不掌握管理员密码）
  - app 分区使用率: 6%（无变化）
- **回滚方式**：`git revert` 本次 commit；或手动删除 admin_server.{h,cc} 的新增内容（4 处都是新增，无修改既有逻辑）
- **关联文档**：
  - 更新 `PROJECT_CONTEXT.md` §5.5 API 表（新增 `/api/display/switch` 行）
  - 遵循 `ARCHITECTURE.md` §7 不变量 #3（后台 IO 经 `Application::Schedule` 投递，不在 HTTP 线程操作 LVGL）
  - 遵循 `AGENTS.md` §5.2（写路由 `IsAuthorized(req, write=true)`、48KB body 上限、白名单校验、Cookie+CSRF）

### 设计决策（与原计划的小调整）

UI 位置从"PAGE ROUTING 区块下方"改为"设备控制区音量行下方"——实际看到 HTML 后，左侧 aside 已经很拥挤（页面列表 + 保存 + 运行状态 + 刷新间隔 + hint），再加按钮会过载；设备控制区已有"音量控制"这种"立即生效的远程操作"先例，把"显示页面切换"放在它下面语义更一致（都是"立刻对设备做某事"）。

---

## 2026-08-11 — 提交知识库 + 清理 dataless-backup（2 个 commit）

- **修改内容**：
  - Commit `b8b04c2` `docs: 建立项目知识库`：仅提交 5 个新文档（PROJECT_CONTEXT / ARCHITECTURE / PROJECT_CLEANUP_PLAN / AGENTS / DEVELOPMENT_LOG），共 1226 行
  - Commit `66b8b8c` `chore: 移除 dataless-backup 备份文件并补充 .gitignore`：`git rm --cached` 3 个误跟踪的 `*.dataless-backup`（共 324 行，本地文件保留）；`.gitignore` 增加 `*.dataless-backup` 规则
- **修改原因**：
  - 用户要求提交代码。working tree 只有 5 个新文档（源码零改动，与 PROJECT_HANDOFF.md 描述的"业务文件全部 untracked"完全不同——实际源码都已在"初始化"commit 内）
  - 扫描中发现"初始化"commit 误把 3 个 macOS dataless 备份文件纳入跟踪，借本次一并清理（对应 `PROJECT_CLEANUP_PLAN.md §3.1-3.3` P0 项）
- **影响范围**：无源码改动，不影响编译或运行行为。后续 git status 干净，新文档作为 AI 协作权威入口。
- **测试结果**：
  - idf.py build: N/A（未改代码）
  - 契约测试: N/A
  - 真机验证: N/A
  - `git diff --cached --check`: ✅（两个 commit 均通过）
  - `git check-ignore *.dataless-backup`: ✅（规则生效）
- **回滚方式**：`git revert 66b8b8c b8b04c2`（注意：revert 66b8b8c 会重新跟踪 dataless-backup，建议改用 `git rm` 手动处理）
- **关联文档**：
  - 完成 `PROJECT_CLEANUP_PLAN.md §3.1-3.3` 的 P0 项
  - `AGENTS.md §10.4` 基线提交规范已部分执行（但未建立独立基线分支，直接在 main 上提交——本次 working tree 仅文档所以可接受）

### 提交过程中的认知修正（重要）

扫描知识库时基于 `PROJECT_HANDOFF.md` 描述判断"业务文件全部 untracked"，但实际 `git status` 显示**只有 5 个新文档未跟踪**。这意味着 HANDOFF 描述的迁移过程已经在"初始化"commit 中完成，所有业务源码都已入库。后续工作应以当前 git 状态为准，不要被 HANDOFF 误导。

但"初始化"commit 同时误入了 3 个 dataless-backup（已在 66b8b8c 修复）——这说明基线 commit 本身不够干净，后续清理 music/pomodoro 死代码时仍建议建 feature 分支操作。

---

## 2026-08-11 — 建立项目知识库（5 个文档）

- **修改内容**：新增 5 个项目知识库文件
  - `PROJECT_CONTEXT.md`：项目介绍、目标、架构、目录结构、核心模块、当前状态
  - `ARCHITECTURE.md`：系统架构图、启动顺序、模块依赖、数据流、设计决策、不变量
  - `PROJECT_CLEANUP_PLAN.md`：基于实际引用分析的清理计划（含证据）
  - `AGENTS.md`：Codex / AI Agent 长期工作规范
  - `DEVELOPMENT_LOG.md`：本文件（含模板）
- **修改原因**：项目交接后需要建立长期维护所需的项目知识库。当前 git 只有"初始化"一个 commit，所有业务文件在 working tree；`PROJECT_HANDOFF.md` 描述的是迁移前的旧状态（版本 3.6.5、远程 ZhouhaoJiang、目录 codex/esp32/），与现状（版本 2.2.2、远程 shaqisheng/ESP32-S3-RLCD、目录 个人项目/）多处不符，需要以当前代码为准重新建立权威文档。
- **影响范围**：无代码改动，仅文档。所有未来 AI 协作将以这 5 个文件为入口。
- **测试结果**：
  - idf.py build: N/A（未改代码）
  - 契约测试: N/A
  - 真机验证: N/A
  - 知识库完整性自检: ✅
    - PROJECT_CONTEXT 涵盖 8 节（介绍/目标/架构/结构/模块/状态/文档/方向）
    - ARCHITECTURE 涵盖 9 节（总览/启动/依赖/数据流/持久化/决策/不变量/服务/债务）
    - CLEANUP_PLAN 每个 deletion 都有 grep 证据
    - AGENTS 涵盖 11 节（背景/原则/规范/文件/规则/禁止/测试/文档/模板/git/沟通）
- **回滚方式**：`git rm PROJECT_CONTEXT.md ARCHITECTURE.md PROJECT_CLEANUP_PLAN.md AGENTS.md DEVELOPMENT_LOG.md`（这 5 个文件都是新建，删除即可）
- **关联文档**：
  - `PROJECT_HANDOFF.md`：历史快照，建议加过时声明（见 CLEANUP_PLAN §3.x）
  - `docs/waveshare-s3-rlcd-4.2-project-handbook.md`：本板权威手册，知识库以此为基础
  - `docs/superpowers/plans/2026-08-11-rlcd-todo-weather-calendar-timezone-fixes.md`：当前最新迭代计划

### 关键发现（供后续工作参考）

1. **HANDOFF.md 已部分过时**：它提到的 WeatherManager race、TodoManager 回滚问题、严格日期校验**都已在当前代码修复**（通过 `manager_safety.h` 的 `ThreadSafeSnapshot` / `CommitValidatedUpdate` / `IsStrictIsoDate`）。
2. **遗留代码**（music_ui.cc / pomodoro_ui.cc / pomodoro_manager）仍在编译但完全不可达，回收它们是 P1 任务。
3. **最新计划**（2026-08-11）涉及待办刷新调度、Open-Meteo 七日天气、农历传统节日、OTA 时区修复——**实施前需确认其完成进度**。
4. **最大物理约束**：应用分区仅剩约 6%，后续扩展需严格预算。

---

## 模板（复制此节，修改后追加到上方）

```markdown
## YYYY-MM-DD — <一句话标题>

- **修改内容**：具体改了哪些文件、哪些函数
- **修改原因**：解决什么问题 / 实现什么需求
- **影响范围**：哪些模块、哪些用户可见行为
- **测试结果**：
  - idf.py build: ✅/❌
  - 契约测试: ✅/❌/N/A
  - 真机验证: ✅（观察 N 秒无 abort/reboot）/ ❌ / ⏳ 待验证
  - app 分区使用率: xx%（如有变化）
- **回滚方式**：git revert <commit> 或具体步骤
- **关联文档**：更新的 ARCHITECTURE/PROJECT_CONTEXT 章节、相关 plan 文件
```

### 填写规范

- **倒序排列**：新条目加在最上方（紧邻本节标题下方）。
- **一次逻辑改动一条**：不要把多个不相关改动合并。
- **测试结果必须真实**：跑过的写 ✅，没跑的写 N/A 或 ⏳，失败的写 ❌ 并附失败信息。
- **回滚方式具体**：能 `git revert` 就写 commit hash；不能就写步骤。
- **关联文档不能漏**：同步更新了哪些文档必须列出，避免文档与代码再次脱节。

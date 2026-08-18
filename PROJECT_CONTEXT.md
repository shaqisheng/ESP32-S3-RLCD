# PROJECT_CONTEXT

> 本文件由知识库扫描于 2026-08-11 建立，作为本项目（Waveshare ESP32-S3-RLCD-4.2 桌面信息屏）的权威上下文。
> 与代码不一致时以**当前代码**为准；本文件每次架构或重要功能变化后需同步更新。
> 历史/迁移背景见 `PROJECT_HANDOFF.md`（注意：HANDOFF 描述的是迁移前的状态，多处与当前代码已不同步）。

---

## 1. 项目介绍

### 1.1 解决什么问题

本项目是把开源的小智 AI 语音助手固件（`xiaozhi-esp32`，上游 `78/xiaozhi-esp32`）移植到 **Waveshare ESP32-S3-RLCD-4.2** 开发板上，并把它从"语音助手"扩展为一台**常驻桌面的低功耗单色信息屏**：

- 白天显示时间、农历、天气、七日预报、节假日、待办、温湿度和电池/Wi-Fi 状态；
- 同时保留小智语音对话能力（流式 ASR + LLM + TTS）；
- 增加一台 **AI 服务额度监控仪表盘**，自动轮询 Codex / Kimi / GLM / DeepSeek 等账号剩余额度；
- 通过局域网 HTTP 后台（`http://<设备IP>:8080/admin`）配置一切，不依赖公网。

### 1.2 目标硬件

| 项 | 值 |
|---|---|
| 板 | Waveshare ESP32-S3-RLCD-4.2 |
| 芯片 | ESP32-S3-WROOM-1-N16R8 |
| Flash / PSRAM | 16 MB / 8 MB |
| 屏幕 | 400×300 1-bit 反射式单色 LCD（UC8253 风格控制器），SPI 40 MHz |
| 板级固件变体 | `waveshare-s3-rlcd-4.2`（`config.json` 选择专用分区表 `16m_rlcd_quota.csv`） |

### 1.3 核心业务流程

1. **启动**：NVS init → Application → CustomBoard（I2C → 传感器 → SD → 按键 → MCP 工具 → QuotaManager::Init → 显示器 + 数据任务）→ 网络启动 → 小智激活 → 额度刷新与后台服务器启动。
2. **日常显示**：`DataUpdateTask` 每秒/每 5 秒（省电模式）更新时钟、传感器、电池、Wi-Fi、AI 状态、待办、天气、日历，并驱动 AI 子页 10 秒翻页。
3. **AI 额度刷新**：`QuotaManager` 串行轮询各账号（默认 5 分钟一次，超时 10s/账号，响应 ≤16 KB），失败时复用上次成功缓存并打 `stale=true`。每账号可选经独立 HTTP CONNECT 代理（`QuotaProxyTransport`）。
4. **后台管理**：局域网浏览器访问 8080 端口，首次设置管理员密码，Cookie+CSRF 管理；待办 API 还可用独立 Bearer Token 给自动化脚本。

### 1.4 主要功能模块

- **小智语音框架**（上游通用）：协议（WebSocket 或 MQTT+UDP）、音频编解码、MCP 工具、OTA。
- **板级显示**：综合页、日历页、七日天气页、AI 额度页、待办页（共 5 页）。
- **板级后台**：内嵌 HTTP 服务器 + 单页前端，账号/页面/天气/日历/待办/Token 全配置。
- **板级数据管理器**：Quota / Weather / Calendar / Todo / Sensor / Sdcard / QuotaProxy。

---

## 2. 项目目标

| 维度 | 目标 |
|---|---|
| 产品形态 | 桌面常驻单色信息屏 + 局域网管理 + 少量上游 HTTPS 请求 |
| 用户体验 | 综合/日历/七日天气/AI 额度/待办五页可切换；USER 单击切页、双击刷新、长按滚动系统信息；BOOT 单击按当前页上下文分发（综合页对话/AI 页刷额度/天气页刷天气） |
| 可用性 | 网络短暂失败时仍可读（天气/额度保留最后有效快照） |
| 维护性 | 局域网后台完全内嵌，无 CDN 依赖 |
| 安全性 | 后台仅在可信局域网使用；明文 HTTP 禁止暴露公网；密钥只写不回显 |
| 资源约束 | 内部 SRAM（PSRAM 之外的 heap）稀缺，UI 必须严格限制 LVGL 对象数量 |

---

## 3. 技术架构

### 3.1 技术栈

| 层 | 技术 |
|---|---|
| 语言 | C++17（GCC + ESP-IDF），少量 C（assets） |
| 框架 | ESP-IDF v5.5.2（`idf_component.yml` 要求 `>=5.5.2`） |
| UI | LVGL 9.4 + `esp_lvgl_port` |
| 显示驱动 | 自写 `RlcdDriver`（SPI + PSRAM 帧缓冲 + LUT） |
| 网络 | esp_http_server（后台）、esp_http_client（额度/天气/日历）、mbedTLS（TLS） |
| 协议 | WebSocket 或 MQTT + UDP（音频通道） |
| 持久化 | ESP-IDF NVS（无传统数据库） |
| 构建系统 | CMake + Ninja；组件由 IDF Component Manager 管理 |
| 后端服务 | 不内置；对接官方 `xiaozhi.me` 或自部署（见 README） |
| 前端 | 单页 HTML/CSS/JS 内嵌在 `admin_server.cc`（约 13.5 KB raw string） |
| 代码风格 | clang-format（Google 基础，4 空格，行宽 100），见 `docs/code_style.md` |
| 测试 | 宿主机 C++（裸 assert）+ Python 源码契约（unittest），**未接入 CI** |

### 3.2 关键依赖（`main/idf_component.yml` 摘要）

- 上游通用：`78/esp-wifi-connect`、`78/esp-ml307`、`78/xiaozhi-fonts`、`espressif/esp_audio_codec`、`espressif/esp-sr`、`espressif/button`、`espressif/knob`、`lvgl/lvgl`、`esp_lvgl_port`、`espressif/esp_lcd_*`（多家屏幕）、`espressif/esp_video`、`espressif/esp32-camera` 等。
- IDF：`>=5.5.2`。
- 完整版本锁定见 `dependencies.lock`（被 gitignore，仅本地）。

### 3.3 数据流

详见 `ARCHITECTURE.md`。简化版：

```
局域网浏览器/脚本 ──HTTP:8080──▶ AdminServer ──▶ QuotaManager/WeatherManager/...
                                                      │
ESP32-S3 firmware                                      ├─(HTTPS, 可选 CONNECT 代理)─▶ 上游服务
  ├─ Application ──Protocol──▶ 小智服务器              │
  ├─ DataUpdateTask ──周期──▶ 各 Manager + LVGL          │
  └─ CustomLcdDisplay ──LVGL──▶ RlcdDriver ──SPI──▶ 400×300 RLCD
                                                              │
持久化：NVS（默认 nvs + quota_nvs 两个分区）◀──各 Manager
```

---

## 4. 目录结构说明

```
xiaozhi-esp32/
├── CMakeLists.txt               # 顶层工程；PROJECT_VER=2.2.2；MINIMAL_BUILD=ON
├── sdkconfig.defaults           # 全平台基础（裁剪 LVGL、ML307 修复等）
├── sdkconfig.defaults.esp32s3   # S3 专用（16MB QIO、Octal PSRAM 80M、LV_USE_SNAPSHOT）
├── sdkconfig.defaults.*         # 各芯片变体
├── dependencies.lock            # 组件精确版本（gitignore）
├── PROJECT_HANDOFF.md           # 迁移前交接文档（部分过时，仅作历史参考）
├── PROJECT_CONTEXT.md           # 本文件
├── PROJECT_CLEANUP_PLAN.md      # 清理计划
├── AGENTS.md                    # Codex/Agent 长期工作规范
├── ARCHITECTURE.md              # 系统架构
├── DEVELOPMENT_LOG.md           # 开发日志
│
├── main/                        # 固件源码
│   ├── CMakeLists.txt           # 显式 SOURCES + 板级 GLOB 收集
│   ├── Kconfig.projbuild        # choice BOARD_TYPE，含 120 个板（含本板）
│   ├── main.cc                  # 入口：NVS init → Application::Run
│   ├── application.{h,cc}       # 上游核心 + fork 自加的音乐播放框架
│   ├── mcp_server.{h,cc}        # MCP 工具注册（含 self.music.play_url）
│   ├── ota.{h,cc}               # OTA 升级
│   ├── settings.{h,cc}          # NVS 薄封装
│   ├── system_info.{h,cc}       # 系统信息
│   ├── device_state_machine.*   # 设备状态机
│   ├── assets.{h,cc}            # 资源（字体/emoji/唤醒词）
│   ├── audio/  display/  led/   # 通用子系统
│   ├── protocols/               # websocket / mqtt+udp
│   ├── boards/                  # 120 个板级目录
│   │   └── waveshare-s3-rlcd-4.2/   # 本项目实际工作的板（详见 §5）
│   └── idf_component.yml        # 依赖清单
│
├── partitions/
│   ├── v1/                      # 旧分区表（已停用，仅历史）
│   └── v2/                      # v2 分区表（含本板专用 16m_rlcd_quota.csv）
│
├── docs/                        # 文档（部分过时，详见 §7）
│   ├── waveshare-s3-rlcd-4.2-project-handbook.md  # 本板权威手册
│   ├── quota-admin-design.md                      # 早期设计（部分过时）
│   ├── custom-board.md / mcp-*.md / mqtt-udp.md / websocket.md
│   ├── code_style.md
│   ├── superpowers/plans/                         # 迭代计划（最新 2026-08-11）
│   ├── 2026-02-10-计划文档/  添加天气文档/  v0/  v1/
│
├── scripts/
│   ├── release.py               # 板级构建打包；--list-boards 供 CI matrix
│   ├── versions.py              # 固件二进制解析/上传 OSS
│   ├── gen_lang.py              # 语言 JSON → lang_config.h
│   ├── build_default_assets.py  # 打包 assets.bin
│   ├── audio_debug_server.py / download_github_runs.py / generate_rlcd_weather_icons.py
│   ├── Image_Converter/  acoustic_check/  ogg_converter/  p3_tools/  spiffs_assets/  image-converter-statusicon/
│
├── tests/
│   └── host/
│       ├── rlcd_manager_safety_test.cc   # C++ 安全工具类测试（裸 assert，无构建脚本）
│       └── rlcd_ui_source_contract_test.py # UI 源码契约测试（unittest，纯文本断言）
│
├── .github/workflows/build.yml  # CI（仅编译，不跑测试）
├── .clang-format                # Google 基础
└── .gitignore                   # 含 sdkconfig / build / managed_components / secret_config.h；缺 *.dataless-backup
```

---

## 5. 核心模块说明（本板 `main/boards/waveshare-s3-rlcd-4.2/`）

### 5.1 入口

- **`waveshare-s3-rlcd-4.2.cc`**（797 行）：`CustomBoard` 类。构造时按序初始化 I2C→Sensors→Sdcard→Buttons→Tools→QuotaManager::Init→显示器+数据任务。`StartNetwork()` 先 `WifiBoard::StartNetwork()`，再 `QuotaManager::Start()` 和 `AdminServer::Start()`（避免 lwIP 未就绪）。注册板级 MCP 工具（`self.system.info`、`self.disp.network/switch`、`self.memo.*`），明确 disable 了 `self.music.play_url`、`self.screen.set_theme`、`self.weather.update`。番茄钟 MCP 工具整段 `#if 0` 屏蔽。

### 5.2 显示层

- **`custom_lcd_display.{h,cc}`**（564 行）：5 页枚举 `MODE_OVERVIEW/CALENDAR/FORECAST/QUOTA/TODO`。构造时只创建 `SetupWeatherUI/CalendarUI/ForecastUI/QuotaUI/TodoUI`，**不创建 music/pomodoro 页**。页面切换由 `QuotaManager::GetPageSettings()` 驱动，AI 子页翻页间隔可配（默认 10 秒）。`.h` 里 `music_page_`/`pomodoro_page_` 字段已无意义（遗留）。
- **UI 文件**：`weather_ui.cc`（综合页）、`calendar_ui.cc`（月历）、`forecast_ui.cc`（七日天气）、`quota_ui.cc`（AI 卡片）、`todo_ui.cc`（待办页）**全部在用**。`music_ui.cc`、`pomodoro_ui.cc` 已废弃，但**仍被 CMake glob 编译**。
- **`data_update_task.cc`**（633 行）：周期任务，更新时间/传感器/电池/Wi-Fi/AI 状态/天气/日历/待办；含番茄钟刷分支（实际不执行，因番茄钟状态恒 IDLE 且 `pomo_*_label_` 均为 nullptr）。
- **`rlcd_driver.{h,cc}`**：SPI 驱动 + PSRAM 帧缓冲 + LUT；按 1 KB 分片、`ESP_ERR_NO_MEM` 重试 3 次。

### 5.3 管理器（`managers/` 子目录）

| 模块 | 行数 | 职责 | 安全状态 |
|---|---:|---|---|
| `admin_server.{h,cc}` | 628 | 8080 端口 HTTP 服务、28 路由、密码+Cookie+CSRF、Bearer Token、内嵌前端 13.5 KB | 单会话；密码单轮 SHA-256（KDF 弱）；HTTP 明文（仅可信局域网） |
| `quota_manager.{h,cc}` | 795 | 最多 32 额度账号，5 分钟串行刷新，6 类供应商适配，缓存 stale，页面配置 | NVS 明文存 secret（设计如此） |
| `quota_proxy_transport.{h,cc}` | - | 无认证/带认证 HTTP CONNECT 自定义 transport，目标 TLS 强校验（`VERIFY_REQUIRED`） | 强校验 CA；诊断端点固定打 openai.com |
| `weather_manager.{h,cc}` | 350 | 和风天气新版接口（个人 API Host + API KEY 请求头）：实时 + 七日预报单源，失败保留旧快照 | **已用 `ThreadSafeSnapshot` + `update_mutex_` 加锁**（HANDOFF 提到的 race 已修） |
| `calendar_manager.{h,cc}` | 281 | `holiday-cn` 节假日同步 + 板载 1900-2049 农历算法 + 24 节气近似公式 | 农历公式仅 1900-2049 有效 |
| `todo_manager.{h,cc}` | 209 | CRUD + 旧 memo 迁移 | **`CommitValidatedUpdate` 原子提交 + `IsStrictIsoDate` 严格校验**（HANDOFF 提到的回滚/日期问题已修） |
| `sensor_manager.{h,cc}` | - | SHTC3 温湿度 + PCF85063 RTC + NTP 同步 | 在用 |
| `sdcard_manager.{h,cc}` | - | SDMMC `/sdcard` 挂载，仅 init/listFiles | 被 pomodoro/音频用，随番茄钟废弃将变 dead code |
| `pomodoro_manager.{h,cc}` | 462 | 番茄钟 + SD 白噪音 | **死代码**：状态恒 IDLE，仅被 data_update_task 不可达分支引用 |
| `manager_safety.h` | - | header-only 工具：`BackgroundNetworkMutex/Session/Generation`、`ThreadSafeSnapshot<T>`、`CommitValidatedUpdate`、`IsStrictIsoDate`、`StatusVisibility`、`FormatAdminAddress` | 安全基元，其他 manager 共用 |
| `proxy_auth.h` | - | header-only：`ParseProxyUrl` 严格校验 + `Base64Encode` + `ProxyAuthorizationValue` + `ProxyEndpoint` 脱敏 | 防止 CRLF 注入 |

### 5.4 分区表 `partitions/v2/16m_rlcd_quota.csv`

| 名称 | Offset | Size | 说明 |
|---|---:|---:|---|
| nvs | 0x9000 | 0x4000 | 默认 NVS（Wi-Fi、weather、calendar、todos、memo） |
| otadata | 0xd000 | 0x2000 | |
| phy_init | 0xf000 | 0x1000 | |
| ota_0 | 0x20000 | 0x4F0000 | ~5 MB |
| ota_1 | 0x510000 | 0x4F0000 | ~5 MB |
| assets | 0xA00000 | 0x5C0000 | ~5.75 MB（比默认 16m.csv 少 256K，让给 quota_nvs） |
| quota_nvs | 0xFC0000 | 0x40000 | 256 KB，admin password/api_token/quota 配置/额度缓存 |

### 5.5 API 端点（`AdminServer`，约 40 条）

完整路由表（含 2026-08 新增的 Wi-Fi 管理、屏幕截图、单账号刷新、AI 页显示配置等）以 `docs/功能文档.md` §8.3 为准。核心端点：

```
GET    /                      首页
GET    /admin                 同 /，后台入口别名
POST   /api/setup             首次设置密码
POST   /api/login             登录，返回 sid Cookie + csrf
POST   /api/logout            登出
POST   /api/refresh           手动触发额度刷新（/api/refresh-one 单账号）
POST   /api/calendar/sync     同步节假日（注意：同步阻塞 HTTP handler）
POST   /api/proxy-diagnostic  代理诊断
POST   /api/api-token         生成/重置 Todo Bearer Token
GET    /api/status            状态（未登录仅 setup_required；登录附额度明细）
GET    /api/weather-diagnostic
GET/PUT /api/pages            页面顺序与启停
GET/PUT /api/quotas           额度账号 CRUD（PUT 不回显 secret）
GET/PUT /api/refresh-interval 刷新周期（1-60 分钟）
GET/PUT /api/quota-display    AI 页显示配置（每屏卡数/翻页/固定页码）
GET/PUT /api/weather          和风城市与 API Host/Key 配置（qw_key 不回显）
GET/PUT /api/calendar         节假日源
GET/PUT /api/api-token        读取/写入 token
GET/PUT /api/device           设备信息 / 音量（含 battery_low / low_battery_alert 全局低电量提示状态）
POST   /api/display/switch    立即切换显示页面
GET    /api/display/screenshot 屏幕截图
GET    /api/logs?after&limit  系统日志（PSRAM 环形缓冲 256 条，增量拉取，敏感参数已打码）
GET/POST/PUT/DELETE /api/wifi*  Wi-Fi 管理（列表/新增/默认/切换/断开/AP/删除）
GET/POST      /api/todos              待办列表
GET/PUT/DELETE /api/todos/{id}        单条 CRUD
```

---

## 6. 当前开发状态

### 6.1 已完成且稳定

- 综合页 / 日历页 / 七日天气页 / AI 额度页 / 待办页五页 UI
- 后台管理（账号/页面/天气/日历/待办/Token）
- AI 额度自动刷新 + stale 缓存 + 每账号代理
- Todo CRUD + Bearer Token + 旧 memo 迁移
- 和风天气实时 + 七日预报（新版 v1 接口）
- 节假日同步 + 板载农历/节气
- **HANDOFF 提到的多个 P1 问题已在后续修复**：
  - WeatherManager 加锁（`ThreadSafeSnapshot` + `update_mutex_`）
  - TodoManager 原子提交 + 严格日期校验（`CommitValidatedUpdate` + `IsStrictIsoDate`）
  - `manager_safety.h` 提供统一安全基元
  - `proxy_auth.h` 严格代理 URL 校验（防 CRLF 注入）

### 6.2 已知遗留（详见 `PROJECT_CLEANUP_PLAN.md`）

- `music_ui.cc`、`pomodoro_ui.cc`、`pomodoro_manager.cc/.h` 仍被编译但完全不可达
- `custom_lcd_display.h` 残留 `music_page_`/`pomodoro_page_` 字段及 Setup 声明
- `data_update_task.cc` 含 music/pomodoro 死代码分支
- `waveshare-s3-rlcd-4.2.cc` 的 `#if 0` 番茄钟 MCP 工具块
- `mcp_server.cc` 仍注册 `self.music.play_url`（板级 DisableTool 已禁用，但通用层未移除）
- `application.{h,cc}` 仍包含 fork 自加的音乐播放框架
- 3 个 `*.dataless-backup` 备份文件未被 gitignore，可能被误提交
- `docs/quota-admin-design.md` 仍按音乐/番茄钟页面描述，与现状冲突
- 测试（`tests/host/*`）未接入 CI，无构建脚本

### 6.3 风险

- **应用分区约剩 6%**：后续加入字体/资源/后台代码容易超出 OTA 槽（每次构建需检查 size）
- **内部 SRAM 紧张**：PSRAM 充足不代表 SPI/DMA 用的内部 heap 充足，UI 必须严格限制 LVGL 对象数
- **后台明文 HTTP**：禁公网暴露，密码 KDF 弱（单轮 SHA-256 + salt）
- **第三方 API 易变**：Codex 使用 ChatGPT 非公开稳定后端；解析器无回归 fixture

### 6.4 正在开发

`docs/superpowers/plans/2026-08-11-rlcd-todo-weather-calendar-timezone-fixes.md` 是最新计划，目标：
- 后台待办编辑后立即刷新硬件 UI（`ScheduleTodoDisplayRefresh`）
- 七日天气（Open-Meteo 替代高德三日）
- 日历传统节日/节气本地算法
- OTA 时间戳时区修正

**请实施前先读该计划确认当前完成进度**（检查 checkbox）。

---

## 7. 文档准确度

| 文档 | 状态 |
|---|---|
| `README.md` | **已重写为本 fork 的中文项目 README**（2026-08-13），准确 |
| `README_zh.md` / `README_ja.md` | **已删除**（纯上游翻译版，2026-08-13 移除） |
| `docs/功能文档.md` | **完整功能清单**（2026-08-13 建立，含 5 页、后台路由表、数据源、安全设计），准确 |
| `PROJECT_HANDOFF.md` | 迁移前快照，**部分过时**：版本 `3.6.5`（现 `2.2.2`）、目录 `codex/esp32/`、远程 `ZhouhaoJiang/`、构建大小数字均与当前不符；但其架构图、风险清单、技术决策仍可参考 |
| `docs/waveshare-s3-rlcd-4.2-project-handbook.md` | 本板手册（2026-08-10），**§6.1 天气部分过时**（现为高德实时 + Open-Meteo 七日，非 QWeather），其余烧录/排障经验仍可参考 |
| `docs/quota-admin-design.md` | **部分过时**：仍按音乐/番茄钟页面描述 |
| `docs/custom-board.md` / `docs/mcp-*.md` / `docs/mqtt-udp.md` / `docs/websocket.md` | 上游通用，准确 |
| `docs/code_style.md` | 准确，clang-format（Google 基础，4 空格，行宽 100） |
| `docs/superpowers/plans/` | 5 份 RLCD 迭代计划，最新 `2026-08-11` |
| `main/boards/waveshare-s3-rlcd-4.2/README.md` | **已重写**（2026-08-13）：硬件、按键、构建烧录、故障排查，准确 |

---

## 8. 后续开发方向

按优先级（详细原因见 `PROJECT_CLEANUP_PLAN.md` 与 `PROJECT_HANDOFF.md §9`）：

1. **P0 提交当前工作树**：仅有"初始化"一个 commit，所有改动都是 working tree。先建分支保存。
2. **P1 完成遗留清理**：移除 music_ui/pomodoro_ui/pomodoro_manager、custom_lcd_display.h 遗留字段、data_update_task 死分支、`#if 0` 块、3 个 dataless-backup、`.gitignore` 补 `*.dataless-backup`。
3. **P2 长期稳定性测试**：30 分钟+ 真机运行，跨多个 5 分钟刷新周期。
4. **P2 接入测试到 CI**：让 `tests/host/*` 真正能跑。
5. **P2 安全加固**：密码 KDF（PBKDF2-HMAC-SHA256）、未登录 `/api/status` 收窄。
6. **P3 文档同步**：归档/重写 `quota-admin-design.md`、`PROJECT_HANDOFF.md`。
7. **P3 后台可维护性**：拆 `admin_server.cc` 内嵌前端为独立文件，构建期嵌入。
8. **P3 配置导入导出**：默认不导出 secret。

---

## 9. 构建与烧录（快速参考）

```bash
# 环境（路径不要硬编码到脚本）
. /path/to/esp-idf-v5.5.2/export.sh
cd /Users/shaqisheng/个人项目/xiaozhi-esp32

# 配置
idf.py set-target esp32s3
idf.py menuconfig
# 确认：
#   CONFIG_IDF_TARGET="esp32s3"
#   CONFIG_BOARD_TYPE_WAVESHARE_S3_RLCD_4_2=y
#   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_rlcd_quota.csv"
#   CONFIG_USE_DEVICE_AEC=y

# 编译
idf.py build

# 烧录 + 串口监视
ls /dev/cu.usbmodem*   # 当前发现 usbmodem2101
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

串口至少观察 60 秒，搜索：`abort` / `reboot` / `ESP_ERR_NO_MEM` / `rlcd spi tx failed` / `watchdog` / `minimal sram` / `局域网后台已启动` / `额度刷新完成`。

完整烧录布局、设备备份、依赖恢复见 `PROJECT_HANDOFF.md §12-13`（命令本身仍可参考）。

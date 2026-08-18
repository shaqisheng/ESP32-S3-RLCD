# ARCHITECTURE

> 本文件描述系统的整体架构、模块关系、数据流和核心设计决策。
> 与代码不一致时以代码为准；架构变化时必须更新本文件。

---

## 1. 系统架构总览

```
                     ┌─────────────────────────────────────┐
                     │  局域网浏览器 / 自动化脚本           │
                     │  (Home Assistant / curl / iOS 快捷指令)│
                     └─────────────────┬───────────────────┘
                                       │ HTTP :8080
                              Cookie+CSRF / Bearer
                                       │
┌──────────────────────────────────────▼───────────────────────────────────┐
│                          ESP32-S3 固件                                     │
│                                                                            │
│ ┌─────────────────────── Application (上游通用，单例) ─────────────────┐  │
│ │  Protocol (WS/MQTT+UDP)  AudioService  OTA  DeviceStateMachine        │  │
│ │  EventLoop  McpServer (self.* 工具)  Alert/Reboot                     │  │
│ └──────────────────────────────────┬────────────────────────────────────┘  │
│                                    │ owns                                  │
│ ┌──────────────────────────────────▼────────────────────────────────────┐ │
│ │                   CustomBoard (waveshare-s3-rlcd-4.2)                  │ │
│ │                                                                        │ │
│ │  Board 基类职责        │  本板扩展                                     │ │
│ │  ──────────────────────┼───────────────────────────────────────────── │ │
│ │  Wi-Fi 配网            │ CustomLcdDisplay (4 页 UI)                    │ │
│ │  音频 (I2C codec)      │ DataUpdateTask (周期数据/时钟/天气/待办)       │ │
│ │  按键                  │ AdminServer:8080 (后台+REST API)              │ │
│ │  LED                   │ QuotaManager (AI 额度轮询)                     │ │
│ │  MCP 工具入口          │ QuotaProxyTransport (可选 HTTP CONNECT)       │ │
│ │  OTA 回调              │ WeatherManager (和风天气)                     │ │
│ │                        │ CalendarManager (节假日+农历)                 │ │
│ │                        │ TodoManager (CRUD)                            │ │
│ │                        │ SensorManager (SHTC3+RTC)  SdcardManager     │ │
│ └────────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  持久化：NVS（默认 nvs 分区）+ quota_nvs（独立 256KB 分区）                 │
│  显示：LVGL 9.4 + RlcdDriver (SPI 40MHz, PSRAM 帧缓冲)                     │
└──────────────────────────────┬─────────────────────────────────────────────┘
                               │
            ┌──────────────────┼────────────────────┬───────────────┐
                ▼                    ▼                    ▼               ▼
       小智服务器            上游 AI/天气/节假日     局域网 HTTP 代理    RLCD 400×300
   (xiaozhi.me / 自部署)     (HTTPS, TLS 强校验)     (HTTP CONNECT)     (1-bit 单色)
```

---

## 2. 启动顺序（关键，乱序会崩）

`CustomBoard` 构造函数（`waveshare-s3-rlcd-4.2.cc:722-730`）按序执行，**任何依赖网络的步骤都不能放在构造函数**：

```
1. InitializeI2c()              # 共享总线：codec + SHTC3 + RTC
2. InitializeSensors()          # SensorManager: 恢复 RTC 时间
3. InitializeSdcard()           # SDMMC 挂载
4. InitializeButtons()          # BOOT / USER 按键 + 回调
5. InitializeTools()            # 板级 MCP 工具；明确 disable music/theme/weather
6. QuotaManager::Init()         # 打开独立 quota_nvs
7. InitializeLcdDisplay()       # 创建 CustomLcdDisplay
8. StartDataUpdateTask()        # 8KB 栈，优先级 2
```

`StartNetwork()`（`waveshare-s3-rlcd-4.2.cc:732-749`）由 Application 在网络阶段触发：

```
1. WifiBoard::StartNetwork()    # 上游 Wi-Fi 配网
2. 注册 state listener          # 音频会话时 CancelBackgroundNetwork()
3. QuotaManager::Start()        # 启动 5 分钟刷新循环
4. AdminServer::Start()         # 启动 :8080
```

**关键约束**：第 3、4 步必须在 lwIP ready 之后，否则崩溃（HANDOFF §7.7 已踩过）。

`PrepareForActivation()`（`:751`）：在小智激活前 prefetch NTP + 天气，让首屏不空。

---

## 3. 模块依赖关系

### 3.1 上游通用层（`main/` 顶层，非 boards/）

| 模块 | 职责 | 备注 |
|---|---|---|
| `main.cc` | 入口：NVS init → Application::Run | 极简 |
| `application.{h,cc}` | 单例。状态机调度、协议生命周期、MCP 消息分发、Alert | **含 fork 自加的音乐播放框架（约 79 行 .cc + 17 处 .h 字段），现状为 dead path（板级 disable 了 `self.music.play_url`）** |
| `device_state_machine.{h,cc}` | 12 状态严格转移：Unknown→Starting→WifiConfiguring→Idle→Connecting→Listening→Speaking→Upgrading→Activating→AudioTesting→FatalError | atomic + mutex + 观察者 |
| `mcp_server.{h,cc}` | MCP 工具注册中心 | `AddCommonTools` 注册 `self.music.play_url`（上游通用）；`AddUserOnlyTools` 注册 reboot/upgrade/snapshot 等 |
| `protocols/{websocket,mqtt}_protocol.*` | 通信实现 | 默认 MQTT，可用 WS |
| `ota.{h,cc}` | OTA 升级 | 顺序写 + rollback |
| `settings.{h,cc}` | NVS 薄封装 | 命名空间由调用方指定 |
| `system_info.{h,cc}` | 版本/heap/MAC 等 | |

### 3.2 本板（`main/boards/waveshare-s3-rlcd-4.2/`）

```
waveshare-s3-rlcd-4.2.cc            CustomBoard（组装点，单例）
  ├─ rlcd_driver.{h,cc}             SPI 驱动 + PSRAM 帧缓冲
  ├─ custom_lcd_display.{h,cc}      LcdDisplay 子类，持有 4 页 + 切页/AI 子页
  │   ├─ weather_ui.cc              Setup/Update 综合页
  │   ├─ calendar_ui.cc             月历 + 农历
  │   ├─ forecast_ui.cc             七日天气
  │   ├─ quota_ui.cc                AI 卡片
  │   ├─ music_ui.cc                ⚠ 废弃（编译但不可达）
  │   └─ pomodoro_ui.cc             ⚠ 废弃
  ├─ data_update_task.cc            周期任务，调用所有 Manager 的 Get/Update
  ├─ config.h / config.json         引脚/构建变体配置
  └─ managers/
      ├─ admin_server.{h,cc}        :8080 HTTP 服务（单例，被 Board::StartNetwork 启动）
      ├─ quota_manager.{h,cc}       AI 额度（单例）
      ├─ quota_proxy_transport.{h,cc} 自定义 transport
      ├─ weather_manager.{h,cc}     和风天气（实时+七日）
      ├─ calendar_manager.{h,cc}    节假日+农历
      ├─ todo_manager.{h,cc}        待办
      ├─ sensor_manager.{h,cc}      SHTC3+RTC
      ├─ sdcard_manager.{h,cc}      SD（依赖者已半废弃）
      ├─ pomodoro_manager.{h,cc}    ⚠ 死代码
      ├─ manager_safety.h           header-only 安全基元（被多 manager 共享）
      └─ proxy_auth.h               header-only 代理 URL 解析（被 admin + proxy 共享）
```

### 3.3 Manager 间的依赖

- **`manager_safety.h`** 是基元层：`BackgroundNetworkMutex/Session/Generation`（联网串行化 + 取消）、`ThreadSafeSnapshot<T>`、`CommitValidatedUpdate`、`IsStrictIsoDate`。被 WeatherManager、QuotaManager、TodoManager 使用。
- **`proxy_auth.h`** 被 `admin_server.cc`（解析配置输入）和 `quota_proxy_transport.cc`（构造 `Proxy-Authorization` 头）共用。
- **`QuotaManager`** 是其他模块的"配置中枢"：页面顺序/启停也存这里（虽类名偏 quota）。AdminServer 的 `/api/pages` 直接走 QuotaManager。
- **`AdminServer`** 持有/调度所有 Manager：转发 PUT 到对应 Apply，转发 GET 到对应 Get。但**它不持有显示层**，刷新 UI 通过 `Application::Schedule(...)` 投递到主任务。

---

## 4. 数据流

### 4.1 显示更新（主路径，每秒）

```
DataUpdateTask (1Hz, 8KB 栈)
  ├─ tick++ → 每分钟：刷新时钟/日历标签 (calendar_ui)
  ├─ 每秒：检查 PomodoroManager 状态（恒 IDLE，dead path）
  ├─ 10s：电池采样 + WiFi 图标更新
  ├─ 按 interval：WeatherManager.update() → parseAmapNow / parseOpenMeteoForecast
  │                                │
  │                                └─ 写 ThreadSafeSnapshot<WeatherData>
  │                                       （UI 读 getLatestData() 自带快照，无 race）
  ├─ 按需：CalendarManager.SyncYear() → holiday-cn HTTPS → Settings("calendar")
  ├─ 按需：TodoManager.List() → Settings("todos")
  ├─ 每秒：TickQuotaPage() → 10s 自动切 AI 子页
  │            └─ 读 QuotaManager::GetCards() (revision 检查)
  └─ SensorManager.getTempHumidity / getRtcTime → 写标签
```

### 4.2 AI 额度刷新（独立任务）

```
QuotaManager::Start()
  └─ FreeRTOS task (周期 5 min, 可配 1-60)
       │
       ├─ 守卫：device state == Idle 且空闲 ≥30s（不抢语音链路）
       ├─ BackgroundNetworkSession(generation)  ← 可被 CancelBackgroundNetwork 取消
       │
       └─ for each QuotaEntry (最多 32):
            ├─ QuotaProxyTransport.connect()  ← 若 proxy_enabled
            │     └─ CONNECT host:port → 可选代理 TLS → 目标 mbedTLS (VERIFY_REQUIRED)
            ├─ esp_http_client GET (timeout 10s, body ≤16KB)
            ├─ parse by provider: codex/kimi/glm-cn/glm-global/deepseek/generic-json/manual
            ├─ 成功：SaveCardCache → revision++
            └─ 失败：复用上次缓存 + stale=true + error
       sleep 150ms  ← 防止上游限流
```

UI（`RenderQuotaPageInternal`）通过 `GetRevision()` 与本地缓存对比，避免无变化时重绘。后台浏览器通过 `GET /api/status`（已认证）拿到带 tiers 的 JSON。

### 4.3 后台写操作（带认证）

```
浏览器 PUT /api/quotas  (Cookie sid + X-CSRF-Token + body ≤48KB)
  └─ AdminServer.Handler
       ├─ IsAuthorized(req, write=true)
       │     ├─ 校验 sid cookie (内存单会话, 30min idle)
       │     └─ 校验 X-CSRF-Token
       ├─ QuotaManager.ApplyConfigJson(body)  ← 持 mutex，同步写 NVS
       └─ 返回 GetConfigJson()  (has_secret 布尔，不回显)
```

后台写 Todo 成功后**应当**通过 `ScheduleTodoDisplayRefresh()` 投递到主任务刷新 LVGL（见最新计划 `2026-08-11-rlcd-todo-weather-calendar-timezone-fixes.md` Task 1）。

### 4.4 后台读操作（公私分明）

```
GET /api/status:
  ├─ 未登录 → 仅 { setup_required, authenticated, network }  (P2: 仍可能间接泄露)
  └─ 已登录 → 附 QuotaManager::GetStatusJson()（含 tiers / errors / checked_at，但无 secret/account name）
```

### 4.5 待办自动化（无 Cookie）

```
外部脚本 GET/POST/PUT/DELETE /api/todos[/{id}]
  Authorization: Bearer <24B hex api_token>
  └─ AdminServer 跳过 sid/csrf，仅校验 Bearer
```

---

## 5. 持久化层

无数据库。所有数据通过 `Settings`（NVS 包装）。

| 命名空间 | 分区 | 内容 | 持有者 |
|---|---|---|---|
| `wifi` / 系统键 | `nvs` (默认) | Wi-Fi SSID/密码、激活信息、OTA 状态 | 上游 |
| `weather` | `nvs` | 省/市/lat/lng、qw_host、qw_key | WeatherManager |
| `calendar` | `nvs` | holiday year/days/synced/source | CalendarManager |
| `todos` | `nvs` | Todo JSON 数组（≤32 条） | TodoManager |
| `memo` | `nvs` | 旧备忘（首次启动自动迁移到 todos） | TodoManager |
| `admin` | `quota_nvs` | passhash + salt（16B 随机盐 + 单轮 SHA-256） | AdminServer |
| `admin` | `quota_nvs` | api_token（24B hex 明文） | AdminServer |
| `quota` | `quota_nvs` | e00..e31（≤4KB/条）、count、pages、all_ok_at、refresh_min、每卡缓存 r%08x | QuotaManager |

**专用 `quota_nvs` 设计动机**：原 16KB `nvs` 容纳不下多个长 Token，划出独立 256KB 分区，避免挤占 Wi-Fi 等系统配置，也让 admin/quota 配置 schema 独立迁移。代价是分区表成为兼容性关键，**切换分区表必须完整烧录**。

---

## 6. 核心设计决策

### 6.1 单一信息屏产品形态，不追求通用 Web 服务

- 后台完全内嵌（13.5 KB raw string），无 CDN。**离线仍可配置**，但维护性差。
- 所有网络请求串行 + 严格超时，避免与语音链路抢内存/连接。

### 6.2 内部 SRAM 比 PSRAM 更稀缺

PSRAM 充足不代表 SPI/DMA 可用的内部 heap 充足。最初的日历/天气页创建大量 LVGL 对象导致 `ESP_ERR_NO_MEM`（HANDOFF §7.6）。**对策**：
- **LVGL 堆整体驻留 PSRAM**（2026-08-17 起）：`CONFIG_LV_USE_CUSTOM_MALLOC=y`，`lvgl_mem_psram.cc` 把 `lv_malloc_core/lv_realloc_core/lv_free_core` 重定向到 `MALLOC_CAP_SPIRAM`（无内部 SRAM 兜底）。安全前提：刷屏帧缓冲是显式 PSRAM 分配（`custom_lcd_display.cc:86`），flush 路径不经过 LVGL 堆，LVGL 分配的内存不参与 SPI DMA。**不要把 LVGL malloc 改回 CLIB/BUILTIN**——那会把全部 LVGL 对象/样式/字符串拉回内部 SRAM（此前 free SRAM 仅 ~8KB、最低水位 3.4KB；迁移后 free ~65KB、最低水位 ~58KB）。
- 用少量多行 `lv_label` 替代网格对象树；
- 显示帧缓冲与 LUT 全部放 PSRAM；
- SPI 按 1 KB 分片，`ESP_ERR_NO_MEM` 重试 3 次。
- 联网响应缓冲（天气/额度各 16KB）只分配 PSRAM，失败时报错而不是 fallback 到内部 SRAM。
- **系统日志环形缓冲驻留 PSRAM**（2026-08-17 起）：`system_log_buffer.h` 用 `esp_log_set_vprintf` 全局钩子把全系统日志写入 256 条 × ~172B 的 PSRAM 环形缓冲，供后台「日志」tab 与 `GET /api/logs` 读取。钩子在所有日志上下文运行：只做事先分配的本地格式化 + 自旋锁内单条拷贝（**临界区内禁止 malloc/snprintf**）；连续重复行折叠为计数防刷屏（AFE 每 30ms 告警）；`key=/token=/password=/secret=` 打码防 secret 泄露。

### 6.3 安全基元统一到 header-only

`manager_safety.h` / `proxy_auth.h` 是 inline/template 头文件，让所有 manager 共享同一套线程安全/原子提交/URL 校验代码，避免每个 manager 各写一套并各自踩坑。**新写联网/持久化代码必须复用这两个头**，不要再造轮子。

### 6.4 后台联网串行化 + 可取消

`BackgroundNetworkSession` 用一把 mutex + 一个 atomic generation，让后台额度/天气请求串行执行；进入音频会话时调用 `CancelBackgroundNetwork()` 立刻打断（避免阻塞语音）。所有后台 IO 必须 `BackgroundNetworkCancelled(generation)` 检查。

### 6.5 密钥只写、缓存最后成功值

- 后台 GET 配置永不回显 secret，只返回 `has_secret` 布尔；
- 编辑时空 secret = 保留，只有显式 clear 才删除；
- 额度请求失败时保留上次成功值并标记 `stale=true`，保证网络抖动时桌面屏仍可读。

### 6.6 每账号独立代理

不同供应商对代理需求不同（如 Codex 需翻墙、Kimi 不需要）。`QuotaProxyTransport` 支持 per-account `proxy_enabled` + `proxy_url`（含可选 Basic 认证），不影响天气/语音/系统流量。代理走 HTTP CONNECT，**目标 HTTPS 证书仍强校验**。

### 6.7 应用分区预算仅约 6%

`ota_0/ota_1` 各 ~5 MB，应用当前已用 ~94%。**新增字体/图标/后台代码前必须检查 size 输出**，否则 OTA 失败。后续考虑：压缩字体、拆分后台到独立源文件 + gzip 嵌入、或评估去掉 LVGL 未用控件。

---

## 7. 关键不变量（修改代码时不能破坏）

1. **网络启动顺序**：`QuotaManager::Start` / `AdminServer::Start` 必须在 `WifiBoard::StartNetwork()` 之后。
2. **音频会话抢占**：进入 Listening/Speaking 时必须 `CancelBackgroundNetwork()`。
3. **后台 IO 必须可取消**：所有 HTTPS 请求循环中检查 `BackgroundNetworkCancelled(generation)`。
4. **配置写入必须原子**：用 `CommitValidatedUpdate`（拷贝→校验→持久化→提交），失败必须回滚。
5. **共享数据用 `ThreadSafeSnapshot<T>`**：UI 读、Manager 写、MCP 外部写都走快照。
6. **secret 不进 GET 响应**：所有 GET 接口只回 `has_secret/has_proxy_auth`。
7. **后台写操作必须 CSRF**：所有非 GET 路由 `IsAuthorized(req, write=true)`。
8. **CMake glob 规则**：本板 `*.cc`、`managers/*.cc`、`assets/*.c` 全部自动收集。**新增 `.cc` 自动入编译**，废弃文件必须物理删除或重命名否则继续占空间。
9. **LVGL 对象预算**：UI 改动后必须真机验证 minimal SRAM ≥ 20KB（曾跌到 387 bytes 引起崩溃）。
10. **分区表切换必须完整烧录**：换分区表不能 OTA。

---

## 8. 服务关系（外部）

| 服务 | 用途 | 配置位置 |
|---|---|---|
| `xiaozhi.me`（默认）或自部署 | 小智服务器（语音链路） | OTA 激活时写入 NVS |
| 高德 `/v3/weather/weatherInfo` | 实时天气 + 三日降级 | WeatherManager，key 在 NVS `weather.amap_key` |
| Open-Meteo | 七日预报 | WeatherManager，按城市经纬度，**无需 key** |
| `raw.githubusercontent.com/NateScarlet/holiday-cn/master/{year}.json` | 中国法定节假日 | CalendarManager，源 URL 可在后台改 |
| Codex `https://chatgpt.com/backend-api/wham/usage` | ChatGPT 额度 | QuotaManager entries |
| Kimi `https://api.kimi.com/coding/v1/usages` | Kimi 额度 | 同上 |
| GLM CN `https://open.bigmodel.cn/api/monitor/usage/quota/limit` | GLM 国内额度 | 同上 |
| GLM Global `https://api.z.ai/api/monitor/usage/quota/limit` | GLM 国际额度 | 同上 |
| DeepSeek `https://api.deepseek.com/user/balance` | DeepSeek 余额 | 同上 |
| 局域网 HTTP 代理（per-account 可选） | 翻墙访问上游 | QuotaManager entries.proxy_url |

所有外部 HTTPS（除代理本身）通过 `esp_crt_bundle_attach` 校验 CA，`MBEDTLS_SSL_VERIFY_REQUIRED`。

---

## 9. 当前架构债务

详见 `PROJECT_CLEANUP_PLAN.md`。一句话总结：

- **遗留代码**（music/pomodoro）仍在编译但不可达，浪费 Flash 与维护成本；
- **上游通用层** 仍注册 `self.music.play_url`，板级靠 DisableTool 屏蔽；
- **后台前端**内嵌在 C++ 文件中，无构建期检查；
- **测试**未接入 CI；
- **应用分区**仅剩约 6%，是后续扩展的最大物理约束。

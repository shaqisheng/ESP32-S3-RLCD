# PROJECT HANDOFF — Waveshare ESP32-S3-RLCD-4.2 小智信息屏

> 交接日期：2026-08-10
>
> 当前项目目录：`/Users/shaqisheng/codex/esp32/xiaozhi-esp32`
>
> Git 远程：`https://github.com/ZhouhaoJiang/xiaozhi-esp32.git`
>
> 当前分支：`main`
>
> 当前基线提交：`88115fa fix: 修复白噪音播放卡顿及显示框架 UI 初始化问题`
>
> 目标板：Waveshare ESP32-S3-RLCD-4.2
>
> 固件变体：`waveshare-s3-rlcd-4.2`

## 0. 迁移前必须先知道的事情

### 0.1 当前可用功能尚未提交到 Git

这是本项目迁移的最高风险。

当前 Git `main` 仅有基线提交 `88115fa`。本轮开发的大量文件仍处于 modified 或 untracked 状态，尤其是后台管理、AI 额度、代理、日历、天气预报、待办管理和新分区表。**如果在另一台机器上只执行 `git clone origin main`，这些功能不会出现。**

迁移前应先在当前机器建立交接分支并提交完整快照。不要只复制 `git diff`，因为 `git diff` 默认不包含 untracked 文件。

建议操作：

```bash
cd /Users/shaqisheng/codex/esp32/xiaozhi-esp32
git switch -c handoff/waveshare-rlcd-dashboard

# 先检查并移除或排除 *.dataless-backup，随后显式添加业务文件。
git status --short
git add main sdkconfig.defaults partitions docs PROJECT_HANDOFF.md -- \
  ':(exclude,glob)**/*.dataless-backup'
git status --short
git commit -m "feat: add RLCD dashboard, quota admin, calendar, forecast and todos"
```

不要提交以下生成物或机器文件：

- `build/`
- `managed_components/`
- `sdkconfig`（通常由配置流程生成；如需完全复现可安全地单独加密归档）
- `.DS_Store`
- `*.dataless-backup`
- 任何真实管理员密码、API Key、Bearer Token、Wi-Fi 凭据或设备 Flash 备份

### 0.2 当前工作树是已经编译和烧录验证的真实版本

2026-08-10 重新检查结果：

- Ninja 增量编译成功；
- `git diff --check` 通过；
- 内嵌后台 JavaScript 通过 `node --check`；
- `xiaozhi.bin` 大小 `0x4a57f0`；
- 最小应用分区 `0x4f0000`，仅剩 `0x4a810`，约 6%；
- 最近一次完整烧录后串口连续运行约 77 秒，无重启；
- free SRAM 稳定约 38～39 KB，minimal SRAM 为 20,747 bytes；
- 之前的 `ESP_ERR_NO_MEM` 刷屏崩溃已经解决。

最终应用固件 SHA-256：

```text
955861f76c3b505987875cab5df95966a757a62f9ba6cc7ca859e1d4bc8e494a  build/xiaozhi.bin
```

其他烧录文件校验值：

```text
6d7d5e99d8e249d3864622e71185e8de70330117efc2da73df035350a01e250e  build/bootloader/bootloader.bin
af289b984c950f2a6a975dec843182089d2b6784056591b8395ab9e91d30d0ca  build/partition_table/partition-table.bin
2fe10b5b1c2619f58a3ab56889e4807e05f342e260d7900572b1af6399cff73a  build/generated_assets.bin
```

## 1. 项目背景与目标

项目基于 `ZhouhaoJiang/xiaozhi-esp32`，最初目标是在 Waveshare ESP32-S3-RLCD-4.2 上运行小智开源固件。随后根据实际使用需求，把设备扩展为一台常驻桌面的单色信息屏：既保留小智语音能力，又展示时间、天气、日历、待办和多个 AI 服务账号的额度。

完整需求演进如下：

1. 选择并烧录 `waveshare-s3-rlcd-4.2` 固件到对应开发板。
2. 增加 AI 额度页面，参考 CC Switch 的额度查询思路，支持 Codex、GLM、Kimi、DeepSeek，且允许同供应商多个账号。
3. AI 额度每 5 分钟自动刷新，右上角显示刷新时间。
4. 增加局域网后台，用于配置页面和账号。
5. 修复首次设置密码后无法登录、无有效 Cookie 请求 `/api/quotas` 报错的问题。
6. 后台从“只能添加账号”扩展为完整账号管理：编辑、启停、排序、删除。
7. AI 页面一屏最多 4 个账号，增加供应商 Logo 和页面预览。
8. 每个额度账号可独立启用 HTTP CONNECT 流量代理。
9. AI 页面使用弹性布局：1 个铺满、2 个左右、3 个自适应、4 个 2×2。
10. 去除番茄钟页和音乐页。
11. 增加日历页、农历和中国节假日/调休标记。
12. 将原天气页改为综合页：时间、年月日、农历、天气、待办和小智状态。
13. 新增独立七日天气页，位置和 QWeather 配置由后台管理。
14. 增加待办 REST API，可由局域网客户端管理待办。
15. 完成编译、烧录，并修复新页面引起的内部 SRAM 耗尽问题。

项目产品目标不是通用 Web 服务，而是“离线 UI + 局域网管理 + 少量上游 HTTPS 请求”的嵌入式设备。设计和实现都应优先考虑内部 SRAM、Flash 分区、网络超时及掉线后的可用性。

## 2. 当前整体架构

```text
                     ┌─────────────────────────┐
                     │  局域网浏览器 / API 客户端 │
                     └────────────┬────────────┘
                                  │ HTTP :8080
                         Cookie+CSRF / Bearer
                                  │
┌─────────────────────────────────▼─────────────────────────────────┐
│ ESP32-S3 firmware                                                  │
│                                                                    │
│  CustomBoard                                                       │
│  ├─ 按键、I2C、音频、传感器、SD、ADC 电池                           │
│  ├─ CustomLcdDisplay                                               │
│  │  ├─ 综合页 weather_ui.cc                                        │
│  │  ├─ 日历页 calendar_ui.cc                                       │
│  │  ├─ 七日天气 forecast_ui.cc                                     │
│  │  └─ AI 额度 quota_ui.cc                                         │
│  ├─ DataUpdateTask：时间/天气/传感器/待办/状态更新                   │
│  ├─ AdminServer：后台页面、认证、REST API                           │
│  ├─ QuotaManager ── QuotaProxyTransport ── AI 供应商 HTTPS          │
│  ├─ WeatherManager ─────────────────────────── QWeather HTTPS       │
│  ├─ CalendarManager ────────────────────────── holiday JSON HTTPS   │
│  └─ TodoManager                                                     │
│                                                                    │
│  默认 NVS：Wi-Fi/weather/calendar/todos/memo                         │
│  quota_nvs：admin密码/API Token/额度配置/页面配置/额度缓存             │
└──────────────────────────────┬─────────────────────────────────────┘
                               │ LVGL RGB565 buffer in PSRAM
                               ▼
                      RlcdDriver → SPI → 400×300 RLCD
```

### 2.1 板级入口

`main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc`

负责硬件初始化、按键、MCP 工具、显示器创建，以及在 TCP/IP 栈启动后启动 `QuotaManager` 和 `AdminServer`。

重要初始化顺序：

1. 初始化 I2C；
2. 初始化传感器和 SD 卡；
3. 初始化按键与板级 MCP 工具；
4. `QuotaManager::Init()` 初始化独立 NVS；
5. 创建显示和后台数据任务；
6. `StartNetwork()` 先调用 `WifiBoard::StartNetwork()`；
7. TCP/IP 就绪后再启动额度刷新任务和 HTTP 管理后台。

后台和额度请求不能在板构造函数中提前启动，否则 lwIP 尚未初始化。

### 2.2 显示层

`CustomLcdDisplay` 的稳定页面 ID：

| ID | 中文名称 | 文件 |
|---|---|---|
| `overview` | 综合页 | `weather_ui.cc` |
| `calendar` | 日历页 | `calendar_ui.cc` |
| `forecast` | 天气页 | `forecast_ui.cc` |
| `quota` | AI 页 | `quota_ui.cc` |

`data_update_task.cc` 周期性更新时钟、RTC/NTP、传感器、电池、Wi-Fi、待办和天气快照。页面顺序与启停配置来自 `QuotaManager`，至少保留一个页面。

### 2.3 管理器

| 类 | 主要职责 |
|---|---|
| `AdminServer` | 端口 8080、内嵌后台、管理员认证、CSRF、Todo Bearer Token、API 路由 |
| `QuotaManager` | 最多 32 个额度账号、5 分钟刷新、供应商适配、缓存、页面设置 |
| `QuotaProxyTransport` | 无认证 HTTP CONNECT 代理；HTTPS 仍校验目标证书 |
| `WeatherManager` | QWeather 实时天气、七日预报、位置和 Host/API Key 配置 |
| `CalendarManager` | 年度节假日数据源、缓存、节假日查询和本地农历转换 |
| `TodoManager` | 最多 32 条待办、旧 memo 迁移、排序、完成状态和 JSON 持久化 |

### 2.4 构建和资源

`main/CMakeLists.txt` 使用 glob 将当前板目录、`managers/*.cc` 和 `assets/*.c` 纳入构建，并新增：

- `esp_http_server`
- `tcp_transport`
- `mbedtls`

`sdkconfig.defaults` 启用 `CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y`，这是额度代理自定义 transport 的必要条件。

## 3. 已完成功能

### 3.1 固件与硬件

- 目标芯片 ESP32-S3；
- 16 MB Flash 和 8 MB PSRAM；
- Waveshare 400×300 单色 RLCD；
- I2C 共享音频、SHTC3 和 RTC；
- ADC 电池状态读取；
- Wi-Fi 配网和小智语音框架；
- 已完成整包编译、完整烧录和启动日志验证。

### 3.2 综合页

- 大号时间；
- 年月日、星期和农历；
- 当前天气、温度、体感、湿度和风力数据；
- 温湿度传感器、电池和 Wi-Fi 状态；
- 小智对话/状态；
- 最多展示 3 条未完成待办，更多时显示剩余数量。

### 3.3 日历页

- 当前年月和完整农历标题；
- 六周月历网格；
- 每日农历文本；
- “休”表示法定休息，“班”表示调休上班；
- 默认从 `holiday-cn` 年度 JSON 同步并缓存。

### 3.4 天气页

- 后台配置 QWeather Location、城市显示名、API Host 和 API Key；
- 当前天气和七日高低温；
- 网络失败保留最后有效快照；
- 综合页与天气页共享数据源。

### 3.5 AI 额度页

- 支持多个同类型账号；
- 支持 `codex`、`kimi`、`glm-cn`、`glm-global`、`deepseek`、`generic-json`、`manual`；
- 内置 Codex、Kimi、GLM 和 DeepSeek 单色 Logo 资源；
- 1/2/3/4 个账号使用弹性布局；
- 一屏最多 4 个，超过后每 10 秒自动切换 AI 子页；
- 页面标题已改为“AI”，不显示 `1/1`；
- 右上显示更新时间；
- 卡片显示额度层级、进度、剩余量或重置时间；
- 每 5 分钟自动刷新，也支持后台手动刷新；
- 请求串行执行，单请求超时 10 秒，响应最大 16 KB；
- 请求失败时保留最后成功缓存并标记 stale/error；
- 每个账号可独立使用无认证 HTTP CONNECT 代理。

### 3.6 局域网后台

地址：

```text
http://<设备 IP>:8080/admin
```

已实现：

- 首次设置 8～72 位管理员密码；
- 用户名固定为 `admin`；
- 登录、退出、30 分钟闲置超时；
- 真实 `sid` HttpOnly Cookie；
- 写请求 CSRF 校验；
- AI 页面浏览器预览；
- 页面启停和排序；
- 账号添加、管理、启停、排序、删除和整体保存；
- 密钥只写，不从设备回显；
- 天气、日历、待办和 Todo API Token 配置；
- 最大请求体 48 KB。

### 3.7 待办 API

支持后台 Cookie 会话或独立 Bearer Token：

| 方法 | 路径 | 功能 |
|---|---|---|
| GET | `/api/todos` | 列表 |
| POST | `/api/todos` | 新增 |
| GET | `/api/todos/{id}` | 查询单条 |
| PUT | `/api/todos/{id}` | 修改或完成 |
| DELETE | `/api/todos/{id}` | 删除 |

### 3.8 按键

| 操作 | 当前功能 |
|---|---|
| BOOT 单击 | 启动阶段进入配网；正常运行时切换小智对话状态 |
| USER 单击 | 按后台配置循环页面；AI 多子页时先切换子页 |
| USER 双击 | 刷新天气、传感器和时间 |
| USER 长按 | 在综合页滚动显示系统信息 |

## 4. 未完成或仅部分完成的功能

以下内容不能当作已完成验收：

1. **音乐功能未彻底删除。** 板级音乐页面不再创建，但通用 `main/mcp_server.cc` 仍注册 `self.music.play_url`。旧音乐 UI 字段和更新分支仍存在。
2. **番茄钟未彻底清理。** 板级 MCP 工具块被 `#if 0` 禁用，页面不创建，但 `pomodoro_ui.cc`、manager 和 `DataUpdateTask` 中的 legacy 引用仍在构建或源码中。
3. **新日历/天气轻量布局只完成稳定性验证，未完成完整视觉回归。** 需要在真机检查长城市名、长节日名、跨月、不同农历文字和七日数据对齐。
4. **天气“图标”目前主要是 ASCII/文字映射**（如晴、雨、雪的简单符号），不是完整的 QWeather 图标资源集。
5. **缺少自动化测试。** 当前主要依赖编译、JavaScript 语法检查、真机烧录和串口观察。
6. **第三方额度解析缺少固定回归样本。** Codex/GLM/Kimi/DeepSeek 接口一旦变更，只能运行时发现。
7. **配置导入/导出未实现。** 后台可以管理配置，但没有一键备份和恢复 JSON；迁移到另一块设备时需手工重配或备份 NVS。
8. **管理员密码重置入口未实现。** 忘记密码时没有后台自助恢复流程，需要擦除 `quota_nvs` 或全片重置。
9. **长期稳定性测试不足。** 已验证 77 秒无崩溃，但仍需 30 分钟、数小时和跨 5 分钟刷新周期测试。
10. **旧设计文档尚未同步。** `docs/quota-admin-design.md` 仍包含早期音乐/番茄钟/QUOTA 页面描述，应以本文件和当前代码为准。

## 5. 最近修改过的文件及原因

### 5.1 已跟踪但尚未提交的修改

| 文件 | 修改原因 |
|---|---|
| `main/CMakeLists.txt` | 为后台 HTTP 服务和代理 TLS transport 增加 `esp_http_server`、`tcp_transport`、`mbedtls` 依赖；纳入板级 managers/assets glob |
| `main/boards/waveshare-s3-rlcd-4.2/config.json` | 为该固件变体选择专用 `16m_rlcd_quota.csv` 分区表 |
| `custom_lcd_display.cc` | 页面状态机改成综合/日历/天气/AI；创建新页面；切页和 AI 子页逻辑；待办显示；音乐页切换退化到综合页 |
| `custom_lcd_display.h` | 新页面枚举、控件、额度卡片数组、页面切换和更新接口 |
| `data_update_task.cc` | 综合页日期/农历、天气和 Todo 更新；AI 页 tick；遗留番茄状态保持惰性 |
| `weather_manager.cc/.h` | 增加位置持久化、QWeather Host/API Key、当前天气详情和七日预报 |
| `waveshare-s3-rlcd-4.2.cc` | 新按键语义；初始化 Quota/Admin；网络启动顺序；禁用板级番茄工具；整合 Todo |
| `weather_ui.cc` | 原天气主页面重构为综合页，并预留天气详情/农历/待办区域 |
| `sdkconfig.defaults` | 启用 ESP HTTP Client 自定义 transport，供额度代理使用 |

### 5.2 新增但尚未跟踪的业务文件

| 文件 | 作用 |
|---|---|
| `calendar_ui.cc` | 轻量月历 UI，使用单一多行标签降低内部 SRAM |
| `forecast_ui.cc` | 轻量七日天气 UI |
| `quota_ui.cc` | AI 卡片和 1～4 项弹性布局 |
| `managers/admin_server.cc/.h` | 内嵌管理后台、认证、全部管理 API |
| `managers/quota_manager.cc/.h` | 额度配置、刷新、解析、缓存和页面配置 |
| `managers/quota_proxy_transport.cc/.h` | HTTP CONNECT + TLS 自定义 transport |
| `managers/calendar_manager.cc/.h` | 节假日同步、缓存和农历算法 |
| `managers/todo_manager.cc/.h` | Todo CRUD、排序和旧 memo 迁移 |
| `assets/icons/ui_img_quota_*.c` | Codex、Kimi、GLM、DeepSeek 单色 Logo |
| `partitions/v2/16m_rlcd_quota.csv` | 增加 256 KB `quota_nvs`，保持两个 OTA 槽 |
| `docs/quota-admin-design.md` | 早期额度后台设计，部分内容已过时 |
| `docs/waveshare-s3-rlcd-4.2-project-handbook.md` | 已验证功能、操作、API、烧录和故障经验手册 |
| `PROJECT_HANDOFF.md` | 本迁移交接文档 |

### 5.3 不应提交的临时文件

以下文件是 macOS dataless/编辑过程产生的备份，与当前源文件内容不一致：

```text
main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc.dataless-backup
main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.h.dataless-backup
main/boards/waveshare-s3-rlcd-4.2/managers/quota_manager.h.dataless-backup
```

迁移提交前应人工确认无恢复需求，然后从交接提交中排除。不要让 CMake glob 误把非 `.cc` 后缀当源码；目前它们不会编译，但会污染版本库。

## 6. 重要技术决策及原因

### 6.1 页面配置和额度配置统一由 QuotaManager 管理

虽然类名偏额度，但页面顺序与额度项需要共同驱动 AI 子页及后台预览。把页面配置放在同一独立 NVS 分区，能避免原 16 KB NVS 被长 Token 挤满，并允许配置 schema 迁移。

代价是职责名称不够准确。未来若扩展更多页面，可重构成 `DashboardConfigManager`，但迁移时不要随意拆分，以免破坏现有 NVS 数据。

### 6.2 使用专用 `quota_nvs`

多个 Codex Token 可能很长，原 NVS 还承载 Wi-Fi 和其他设置。项目从 assets 尾部划出 256 KB `quota_nvs`，专门保存 admin 和 quota 数据。

这样能降低普通 NVS 空间不足风险，但也使分区表成为固件兼容性关键。首次切换新分区表要完整烧录。

### 6.3 密钥只写、缓存最后成功额度

后台 GET 配置不返回 secret，只返回 `has_secret`。编辑时空 secret 表示保留，只有显式 clear 才删除。额度请求失败时保留上次成功值并标记 stale。

这是为了减少浏览器泄露凭据，并让桌面信息屏在网络短暂失败时仍可用。

### 6.4 每个账号单独配置代理

仅某些供应商可能需要代理。使用每账号 `proxy_enabled` 和 `proxy_url`，避免天气、语音和所有系统流量被强制走同一个代理。

当前代理只支持无认证 HTTP CONNECT，目标 HTTPS 证书仍校验。没有实现 SOCKS、代理认证或系统级代理。

### 6.5 串行刷新和资源限制

AI 额度每次只请求一个上游，超时 10 秒，响应体限制 16 KB，启动后延迟约 5 秒，周期 5 分钟。

ESP32 内部 SRAM 和并发连接数有限，串行请求比并行请求更稳定，也减少与语音链路争抢内存和网络。

### 6.6 少量 LVGL 对象代替网格对象树

最初日历创建 42 个格子、每格多个标签，七日天气也为每列创建多个控件，导致内部 SRAM 几乎耗尽。当前日历和七日天气用少量多行标签完成排版。

这牺牲部分精细对齐和单格样式能力，换取可接受的内存安全。400×300 单色屏上应继续遵守这个取舍。

### 6.7 后台完全内嵌、无 CDN

后台 HTML/CSS/JS 是 `admin_server.cc` 内的原始字符串，不依赖互联网。设备离线或上游服务异常时仍能打开配置页。

代价是文件大、维护困难、缺少前端构建期检查。修改后必须额外运行 JavaScript 语法检查和浏览器回归。

### 6.8 Cookie/CSRF 与独立 Todo Token 混合认证

浏览器后台使用 HttpOnly Cookie 和 CSRF，适合交互管理；Todo 自动化使用 Bearer Token，适合 Home Assistant、快捷指令或局域网脚本。

管理后台仍是明文 HTTP，不应暴露到公网。

## 7. 已解决的问题、坑与解决方案

### 7.1 后台设置密码后无法访问 `/api/quotas`

**现象**：curl 带 `-b 'V'` 请求仍失败。

**原因**：`V` 不是设备通过 `/api/login` 签发的真实 `sid` Cookie；写请求还缺少 CSRF Token。

**解决**：浏览器使用 `credentials: same-origin`；命令行先登录并用 `-c` 保存 Cookie，再用 `-b` 回放。写请求同时带登录 JSON 返回的 `csrf`。

```bash
DEVICE=http://192.168.2.71:8080
curl -sS -c /tmp/rlcd-cookie.txt \
  -H 'Content-Type: application/json' \
  -d '{"password":"YOUR_PASSWORD"}' \
  "$DEVICE/api/login"
curl -sS -b /tmp/rlcd-cookie.txt "$DEVICE/api/quotas"
```

设备 IP `192.168.2.71` 是当时局域网地址，不是固定配置。

### 7.2 后台只能添加账号，不能管理

增加账号折叠管理区，支持：编辑、启停、上移、下移、删除、放弃修改和保存全部更改；增加账号状态和错误展示。

### 7.3 AI 页面样式和 Logo 不符合预期

将标题改为“AI”，去除 `1/1`，修复“几分钟前更新”和重置时间文本，加入真实供应商 Logo 资源，名称前展示 Logo，后台同步加入页面预览。

### 7.4 多账号布局不合理

实现 1～4 项弹性布局和最多 4 项/页。超过 4 项生成 AI 子页并自动翻页。

### 7.5 上游额度请求需要代理

新增 `QuotaProxyTransport`，允许单账号通过局域网 HTTP CONNECT 代理访问目标 HTTPS 服务。

### 7.6 新页面启动后 `ESP_ERR_NO_MEM`

**现象**：刷屏 SPI transaction 分配失败，abort 并重启；minimal SRAM 曾只有约 387 bytes。

**原因**：日历和预报页创建过多 LVGL 对象。PSRAM 仍充足不代表 SPI/DMA 需要的内部 SRAM 充足。

**解决**：日历和七日天气重构为轻量标签布局。复测后 minimal SRAM 提升至 20,747 bytes，约 77 秒无重启。

### 7.7 网络服务启动过早

额度 HTTP 和管理后台必须在 `WifiBoard::StartNetwork()` 后启动，不能放到构造函数中。当前代码已经按此顺序实现。

### 7.8 页面设计文档落后于实现

早期设计仍写音乐/番茄钟/QUOTA。当前代码页面是综合/日历/天气/AI。交接时以本文件、项目经验手册和当前代码为准。

## 8. 当前已知 Bug 与风险

按优先级排序：

### P0：未提交工作树可能在迁移时丢失

大部分新文件是 untracked。必须先提交到交接分支或完整复制工作树。

### P1：WeatherManager 存在线程安全问题

`weather_manager.h` 注释称 `getLatestData()` 线程安全，但实现直接复制 `latest_data_`，更新、外部 MCP 写入、后台配置和 UI 读取均没有 mutex。`WeatherData` 内含多个 `std::string`，并发读写属于数据竞争，极端情况下可能崩溃或产生破损文本。

建议新增 mutex，并对以下操作统一加锁：

- `getLatestData()`；
- `updateFromExternal()`；
- `parseWeatherJson()` / `parseForecastJson()`；
- 位置和 API 配置读写；
- `update()` 开始时复制一份配置快照，网络请求期间不要长期持锁。

### P1：TodoManager 更新失败会留下内存脏数据

`Update()` 先直接修改目标 item，再执行字段验证。若验证失败，函数返回 false，但已修改的无效值仍留在 RAM，虽然未写入 NVS。应先复制 item、验证副本、保存成功后再替换，或在失败时回滚。

### P1：通用音乐 MCP 工具仍然暴露

用户要求移除音乐功能和页面。板级页面已移除，但 `main/mcp_server.cc:66` 仍注册 `self.music.play_url`。启动日志也曾显示该工具。若需求是彻底移除音乐能力，需要增加板型条件或关闭通用注册。

### P2：公开 `/api/status` 泄露额度状态

`/api/status` 为了判断是否需要首次设置而允许未登录访问，但它先调用 `QuotaManager::GetStatusJson()`，其中包含额度 ID、tiers、错误和检查时间。虽然没有 secret 和账号名称，仍会暴露使用情况。应在未认证状态仅返回 `setup_required`、`authenticated` 和必要网络信息，认证后再附加额度状态。

### P2：管理员密码哈希强度有限

密码使用随机盐 + 单次 SHA-256，不是 PBKDF2/scrypt/Argon2。局域网低风险场景可用，但对 Flash 离线提取的抵抗力有限。ESP-IDF mbedTLS 可考虑 PBKDF2-HMAC-SHA256，并做 schema 迁移。

### P2：后台只支持一个内存会话

新登录会覆盖旧会话；设备重启后所有会话失效。该行为目前没有在 UI 明确提示，多人同时管理时会互相登出。

### P2：最大请求体与最大账号/Token 长度不完全匹配

后台最大 body 48 KB，额度允许 32 项，每个 secret 最长 3500 字节。理论最大配置远超 48 KB，因此“最多 32 项”只对正常长度 Token 成立。应降低单 secret 限制、分项保存，或提高请求限制并评估内存。

### P2：Todo 日期校验不严格

`ValidDate()` 只校验长度和第 5/8 个字符是否为 `-`，不校验数字、月份范围和真实日期。例如某些非日期字符串也可能通过。应做严格 `YYYY-MM-DD` 解析和往返验证。

### P2：日历同步在 HTTP handler 中同步执行

`POST /api/calendar/sync` 直接执行年度 HTTPS 下载，会占用 HTTP handler 较长时间。网络慢时浏览器体验差，也会占用服务器任务栈。建议改为排队后台同步并通过状态接口轮询。

### P2：第三方 API 不稳定

Codex 使用 ChatGPT 非公开稳定后端；Kimi、GLM 和 DeepSeek 响应也可能升级。解析器没有自动化样本测试，字段变化会显示“JSON 解析失败”或“未找到额度数据”。

### P3：代码与二进制仍包含遗留页面结构

`music_ui.cc`、`pomodoro_ui.cc`、相应 manager、控件字段和 `DataUpdateTask` 空分支仍存在。它们增加维护成本和应用体积。

### P3：后台资源难维护

HTML/CSS/JS 全部内嵌在一个约 48 KB 的 C++ 文件中。修改时容易引入引号、模板和脚本语法问题。

### P3：应用分区只剩约 6%

后续加入字体、图片、TLS 组件或后台代码时容易超出 OTA 槽。每次构建都必须检查 size 输出。

## 9. 下一步开发计划

### 第一阶段：先保证可迁移和可维护

1. 建立交接分支并提交当前全部业务文件；排除 `*.dataless-backup`。
2. 在新机器从零安装 ESP-IDF 5.5.2，验证 clean build。
3. 修复 `WeatherManager` 数据竞争。
4. 修复 `TodoManager::Update()` 失败不回滚及日期校验。
5. 限制未登录 `/api/status` 返回内容。
6. 真机运行至少 30 分钟，覆盖多个 5 分钟额度刷新周期。

### 第二阶段：完成用户要求的彻底清理和 UI 验收

1. 按板型关闭 `self.music.play_url`。
2. 从本板构建中移除 music/pomodoro UI、manager 和空分支。
3. 删除 `CustomLcdDisplay` 中未使用的旧控件字段和数组。
4. 为天气增加完整的单色图标映射。
5. 真机验收日历跨月、春节、闰月、节假日长文本和 1～5 个额度布局。
6. 同步或归档过时的 `quota-admin-design.md`。

### 第三阶段：测试和配置迁移能力

1. 为农历、Todo 校验、额度解析增加宿主机单元测试。
2. 保存各供应商脱敏 JSON fixture，做解析回归。
3. 增加后台配置导出/导入，但绝不能默认导出 secret。
4. 增加忘记管理员密码的物理按键安全恢复流程。
5. 将后台前端拆成独立源文件，并在构建时压缩/嵌入。
6. 评估密码 KDF、多个会话和 API rate limit。

## 10. 开发环境要求

### 10.1 已验证环境

| 项目 | 版本/值 |
|---|---|
| 操作系统 | macOS（当前机器） |
| ESP-IDF | 5.5.2 |
| Python | 3.9.6（ESP-IDF 环境） |
| Ninja | 1.12.1 |
| esptool | 4.12.0 |
| Node.js | 仅用于后台 JS 语法检查，固件运行不需要 |
| 芯片 Target | `esp32s3` |
| Flash | 16 MB |
| PSRAM | 8 MB |
| 项目版本 | 3.6.5 |

当前机器工具路径仅供排查，不应硬编码到脚本：

```text
ESP-IDF: /Users/shaqisheng/codex/esp32/esp-idf-v5.5.2
Python:  /Users/shaqisheng/codex/esp32/.espressif-idf/python_env/idf5.5_py3.9_env/bin/python
Ninja:   /Users/shaqisheng/codex/esp32/.espressif-idf/tools/ninja/1.12.1/ninja
```

### 10.2 新机器安装建议

1. 安装 Git、CMake、Ninja 和系统编译工具；macOS 先安装 Xcode Command Line Tools。
2. 使用 Espressif 官方安装方式安装 ESP-IDF **5.5.2**。
3. 克隆包含交接提交的仓库或解开 Git bundle。
4. 运行 ESP-IDF `export.sh`。
5. 让 IDF Component Manager 根据 `main/idf_component.yml` 恢复 `managed_components/`。
6. 使用板级 `config.json`/发布脚本重建 `sdkconfig`，或安全复制当前 `sdkconfig` 作为精确配置参考。

`build/`、`managed_components/`、`sdkconfig` 和 `dependencies.lock` 当前都被 `.gitignore` 忽略。特别注意：

- `managed_components/` 是生成依赖，不应当作源代码；
- `sdkconfig` 记录当前完整菜单配置，但不是 Git 交付内容；
- `dependencies.lock` 也未被 Git 跟踪，若要求完全可复现，应作为受控迁移附件保存，或调整项目策略后纳入版本管理；
- 仅依赖版本范围重新解析组件，未来可能得到不同补丁版本。

## 11. 新机器迁移步骤

### 11.1 推荐方式：提交交接分支

当前机器：

```bash
cd /Users/shaqisheng/codex/esp32/xiaozhi-esp32
git switch -c handoff/waveshare-rlcd-dashboard

# 先确认不要加入 dataless-backup 和秘密文件。
git add main sdkconfig.defaults partitions docs PROJECT_HANDOFF.md -- \
  ':(exclude,glob)**/*.dataless-backup'
git diff --cached --check
git status --short
git commit -m "feat: hand off Waveshare RLCD dashboard firmware"

# 若有可写的个人 fork，推送到 fork；不要擅自推送上游 origin。
git push YOUR_FORK handoff/waveshare-rlcd-dashboard
```

也可生成离线 bundle：

```bash
git bundle create waveshare-rlcd-handoff.bundle handoff/waveshare-rlcd-dashboard
shasum -a 256 waveshare-rlcd-handoff.bundle
```

新机器：

```bash
git clone waveshare-rlcd-handoff.bundle xiaozhi-esp32
cd xiaozhi-esp32
git switch handoff/waveshare-rlcd-dashboard
```

### 11.2 临时方式：复制工作树

如果当前状态不能提交，必须复制 tracked 与 untracked 文件，不能只保存 patch：

```bash
rsync -a \
  --exclude '.git/' \
  --exclude 'build/' \
  --exclude 'managed_components/' \
  --exclude '.DS_Store' \
  --exclude '*.dataless-backup' \
  /Users/shaqisheng/codex/esp32/xiaozhi-esp32/ \
  /path/to/transfer/xiaozhi-esp32/
```

在新机器先 clone 相同基线，再将这份工作树覆盖进去并检查 `git status --short`。这种方式可追溯性较差，只建议作为应急方案。

### 11.3 可复现配置附件

建议通过加密介质单独迁移：

- `sdkconfig`：精确菜单配置参考；
- `dependencies.lock`：精确组件版本；
- 本文件中记录的固件 SHA-256；
- 如确有需要，设备 Flash/NVS 备份。

不要把包含凭据的设备备份放进 Git 或普通网盘。

## 12. 构建、启动与烧录

### 12.1 可移植构建方式

```bash
cd /path/to/xiaozhi-esp32
. /path/to/esp-idf-v5.5.2/export.sh

idf.py set-target esp32s3
idf.py menuconfig
```

在 menuconfig 中确认：

```text
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_BOARD_TYPE_WAVESHARE_S3_RLCD_4_2=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_rlcd_quota.csv"
CONFIG_USE_DEVICE_AEC=y
```

然后：

```bash
idf.py build
```

也可以让板级发布脚本读取 `config.json`：

```bash
python scripts/release.py waveshare-s3-rlcd-4.2 \
  --name waveshare-s3-rlcd-4.2
```

发布脚本若发现同版本 ZIP 已存在会跳过构建，开发时直接 `idf.py build` 更直观。

### 12.2 查找串口

当前验证端口曾为 `/dev/cu.usbmodem21101`，但端口名会变化：

```bash
ls /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* 2>/dev/null
```

### 12.3 常规烧录

```bash
idf.py -p /dev/cu.usbmodem21101 flash monitor
```

### 12.4 完整烧录布局

从 `build/` 目录执行：

```bash
python -m esptool \
  --chip esp32s3 \
  --port /dev/cu.usbmodem21101 \
  --baud 460800 \
  --before default_reset \
  --after hard_reset \
  write_flash \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  0x0 bootloader/bootloader.bin \
  0x20000 xiaozhi.bin \
  0x8000 partition_table/partition-table.bin \
  0xd000 ota_data_initial.bin \
  0xa00000 generated_assets.bin
```

首次使用专用分区表时必须烧录 partition table。不要漏烧 `generated_assets.bin`，否则字体、图标和表情可能缺失。

### 12.5 串口验证

```bash
python -m serial.tools.miniterm /dev/cu.usbmodem21101 115200
```

至少观察 60 秒，并搜索：

```text
abort
reboot
ESP_ERR_NO_MEM
rlcd spi tx failed
watchdog
minimal sram
局域网后台已启动
额度刷新完成
```

## 13. 配置、数据和依赖

### 13.1 没有传统数据库

项目没有 MySQL、SQLite 或云数据库。所有设备配置使用 ESP-IDF NVS：

| 分区/命名空间 | 内容 |
|---|---|
| 默认 `nvs` | Wi-Fi 和基础系统配置 |
| 默认 NVS `weather` | Location、城市名、QWeather Host/API Key |
| 默认 NVS `calendar` | 节假日源、年份缓存和同步信息 |
| 默认 NVS `todos` | Todo JSON 数组 |
| 默认 NVS `memo` | 旧备忘兼容数据 |
| `quota_nvs/admin` | 密码盐/摘要、Todo API Token |
| `quota_nvs/quota` | AI 账号、secret、页面配置、额度缓存 |

### 13.2 分区表

`partitions/v2/16m_rlcd_quota.csv`：

| 名称 | Offset | Size |
|---|---:|---:|
| `nvs` | `0x9000` | `0x4000` |
| `otadata` | `0xd000` | `0x2000` |
| `phy_init` | `0xf000` | `0x1000` |
| `ota_0` | `0x20000` | `0x4F0000` |
| `ota_1` | `0x510000` | `0x4F0000` |
| `assets` | `0xA00000` | `0x5C0000` |
| `quota_nvs` | `0xFC0000` | `0x40000` |

### 13.3 第三方服务配置

以下数据不在 Git 中，迁移开发机后仍保留在原开发板上；换板时需重新配置：

- Wi-Fi SSID/密码；
- 小智服务端激活信息；
- 管理员密码；
- Todo Bearer Token；
- QWeather API Host、Key 和位置；
- Codex/Kimi/GLM/DeepSeek Token；
- Codex Account ID；
- 额度代理 URL；
- 自定义节假日源。

AI 默认接口：

```text
Codex:     https://chatgpt.com/backend-api/wham/usage
Kimi:      https://api.kimi.com/coding/v1/usages
GLM CN:    https://open.bigmodel.cn/api/monitor/usage/quota/limit
GLM Global:https://api.z.ai/api/monitor/usage/quota/limit
DeepSeek:  https://api.deepseek.com/user/balance
Holiday:   https://raw.githubusercontent.com/NateScarlet/holiday-cn/master/{year}.json
```

### 13.4 设备数据备份

仅迁移开发机器时，不需要读取设备 Flash；同一块开发板会保留 NVS，前提是不执行全片擦除。

如果还要迁移到另一块硬件，可对原设备做完整 16 MB 备份：

```bash
python -m esptool \
  --chip esp32s3 \
  --port /dev/cu.usbmodem21101 \
  read_flash 0x0 0x1000000 device-full-backup.bin
```

该文件包含 Wi-Fi 密码、管理员摘要、API Token 和供应商密钥，必须加密保存。完整 Flash 克隆到另一块硬件可能同时复制设备身份、校准或激活信息，执行恢复前应单独评估；通常更安全的做法是在新设备后台手工重建业务配置。

### 13.5 依赖恢复

主要依赖由 ESP-IDF 和 IDF Component Manager 管理。`managed_components/` 是本地生成目录。新机器首次构建可能需要互联网下载组件。

若下载失败，检查：

- ESP-IDF 是否确为 5.5.2；
- Python 虚拟环境是否由该 IDF 安装器创建；
- 代理和 DNS；
- `main/idf_component.yml`；
- 是否拥有迁移附件中的 `dependencies.lock`。

## 14. 后台与 API 使用要点

后台监听 8080，首次访问设置密码。用户名固定为 `admin`。

浏览器后台写操作依赖：

1. `/api/login` 返回的 `Set-Cookie: sid=...`；
2. 登录 JSON 中的 `csrf`；
3. 写请求头 `X-CSRF-Token`。

Todo 自动化可使用后台生成的 Bearer Token：

```bash
curl -H 'Authorization: Bearer TOKEN' \
  http://DEVICE_IP:8080/api/todos
```

主要路由：

```text
POST       /api/setup
POST       /api/login
POST       /api/logout
GET        /api/status
GET/PUT    /api/pages
GET/PUT    /api/quotas
POST       /api/refresh
GET/POST   /api/todos
GET/PUT/DELETE /api/todos/{id}
GET/PUT    /api/weather
GET/PUT    /api/calendar
POST       /api/calendar/sync
GET/POST   /api/api-token
```

后台是明文 HTTP，只允许可信局域网使用，禁止路由器端口映射到公网。

## 15. 新开发者验收清单

### 15.1 源码完整性

- [ ] `git status` 不再出现关键业务文件 untracked；
- [ ] 没有提交 `*.dataless-backup`、`sdkconfig`、build 或凭据；
- [ ] `calendar_ui.cc`、`forecast_ui.cc`、`quota_ui.cc` 存在；
- [ ] admin/quota/proxy/calendar/todo manager 存在；
- [ ] 四个供应商 Logo 文件存在；
- [ ] 专用分区表存在并被板级 `config.json` 选择。

### 15.2 构建

- [ ] ESP-IDF 5.5.2 环境已导出；
- [ ] `idf.py build` clean build 成功；
- [ ] app 分区仍有余量；
- [ ] 后台 JavaScript 语法检查通过；
- [ ] `git diff --check` 通过。

### 15.3 真机

- [ ] 综合、日历、天气、AI 四页可切换；
- [ ] USER 单击/双击/长按和 BOOT 单击符合交互表；
- [ ] 后台首次设置、登录、退出可用；
- [ ] 账号可编辑、停用、排序和删除；
- [ ] AI 1～5 个账号布局正确；
- [ ] 五分钟刷新和十秒子页切换正常；
- [ ] 代理开关只影响对应账号；
- [ ] QWeather 和节假日同步正常；
- [ ] Todo Token 的 GET/POST/PUT/DELETE 正常；
- [ ] 运行 30 分钟无 `NO_MEM`、watchdog 或重启。

## 16. 电池与硬件注意事项

固件只读取电池电量和充放电状态，不提供软件“反向充电”能力。电池能否由 USB 充电、能否 OTG 供电，取决于板载电源/充电芯片和接线。更换电池或供电方式前必须核对 Waveshare 原理图、极性和电压。

RLCD 刷屏需要 SPI/DMA 可用的内部 SRAM。即使 PSRAM 还有数 MB，内部 SRAM 过低仍会导致 `ESP_ERR_NO_MEM`。UI 设计时要优先控制 LVGL 对象数量。

## 17. 参考文档

- `docs/waveshare-s3-rlcd-4.2-project-handbook.md`：操作、API、烧录和故障经验的详细版本；
- `docs/quota-admin-design.md`：早期设计，仅供历史参考，部分页面定义已过时；
- `main/boards/waveshare-s3-rlcd-4.2/README.md`：原板级硬件和天气功能说明；
- `docs/custom-board.md`：上游项目的板型配置与构建方式；
- Waveshare 硬件文档：`https://docs.waveshare.net/ESP32-S3-RLCD-4.2`；
- 上游源码：`https://github.com/ZhouhaoJiang/xiaozhi-esp32`。

## 18. 交接结论

当前工作树已经形成可编译、可烧录、能稳定启动的桌面信息屏固件，四个核心页面、局域网后台、AI 额度、代理、天气、节假日和 Todo API 均已有实际实现。最大即时风险不是功能代码，而是这些改动尚未提交；迁移工作的第一步必须是保存完整工作树并建立可追溯提交。

新开发者接手后应先复现 clean build，再修复 WeatherManager 数据竞争和 Todo 更新回滚问题，随后完成音乐/番茄遗留代码清理及长期真机测试。不要在这些基础问题解决前继续大量增加 LVGL 对象或内嵌资源，因为当前应用分区和内部 SRAM 都已经需要严格预算。

# Waveshare ESP32-S3-RLCD-4.2 小智固件项目经验手册

> 更新日期：2026-08-10
> 项目目录：`/Users/shaqisheng/codex/esp32/xiaozhi-esp32`
> 上游项目：`ZhouhaoJiang/xiaozhi-esp32`
> 固件变体：`waveshare-s3-rlcd-4.2`
> 芯片：ESP32-S3，Flash 16 MB，PSRAM 8 MB，屏幕 400×300 单色 RLCD

## 1. 项目目标与当前成果

本项目基于小智 ESP32 开源固件，为 Waveshare ESP32-S3-RLCD-4.2 增加一套适合常驻桌面的信息屏和局域网管理能力，同时保留小智语音交互。

当前顶层页面为：

1. **综合页**：时间、年月日、农历、当前天气、温湿度、小智状态和未完成待办。
2. **日历页**：月历、农历日、法定节假日及调休上班标记。
3. **天气页**：当前地点和七天天气预报。
4. **AI 页**：Codex、Kimi、GLM、DeepSeek、通用 JSON 或手动额度；支持同供应商多个账号。

音乐页和番茄钟页不再创建，也不参与页面循环。源码中仍保留部分兼容字段、旧 UI 文件和惰性状态读取，后续可继续清理；它们不代表当前产品功能。

局域网后台地址：

```text
http://<设备 IP>:8080/admin
```

后台支持：

- 首次设置密码及后续登录；
- 页面启停和排序；
- AI 额度账号的新增、编辑、启停、排序和删除；
- AI 页面实时预览；
- 单账号代理查询设置；
- 天气位置、QWeather Host 和 API Key 配置；
- 节假日数据源配置与立即同步；
- 待办事项管理及独立 REST API Token；
- 手动触发额度刷新。

## 2. 系统结构

### 2.1 板级入口

`main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc`

负责：

- I2C、音频、传感器、SD 卡、屏幕和按键初始化；
- 创建 `CustomLcdDisplay`；
- 启动额度管理器和局域网后台；
- 注册板级 MCP 工具；
- 读取电池、电量及系统信息。

### 2.2 显示层

| 文件 | 职责 |
|---|---|
| `custom_lcd_display.cc/.h` | 页面状态机、页面切换、LVGL/RLCD 连接、语音状态适配 |
| `weather_ui.cc` | 综合页布局 |
| `calendar_ui.cc` | 月历、农历和节假日显示 |
| `forecast_ui.cc` | 当前天气及七日预报 |
| `quota_ui.cc` | AI 额度卡片、自适应布局和供应商 Logo |
| `data_update_task.cc` | 时间、天气、传感器、电池、Wi-Fi、待办及页面周期更新 |
| `rlcd_driver.cc/.h` | SPI 刷屏、像素映射和 RLCD 硬件命令 |

页面 ID 是稳定配置值，不要因为中文标题变化而修改：

```text
overview  -> 综合页
calendar  -> 日历页
forecast  -> 天气页
quota     -> AI 页
```

### 2.3 数据与服务层

| 管理器 | 职责 |
|---|---|
| `QuotaManager` | 额度配置、供应商请求、解析、缓存、5 分钟刷新和页面编排 |
| `QuotaProxyTransport` | 可选的无认证 HTTP CONNECT 代理隧道，隧道内仍校验目标 HTTPS 证书 |
| `AdminServer` | 8080 端口后台、登录会话、CSRF 和 REST 路由 |
| `WeatherManager` | QWeather 当前天气、七日预报及位置配置 |
| `CalendarManager` | 年度节假日同步、缓存和公历转农历 |
| `TodoManager` | 待办增删改查、排序、完成状态和旧 memo 数据迁移 |

设计原则是网络任务、持久化和 LVGL 分离。管理器更新线程安全快照，显示层只读取快照；不要在 LVGL 锁内进行 HTTP 请求。

## 3. 页面与按键操作

### 3.1 页面布局规则

AI 页一屏最多显示 4 个已启用账号：

- 1 个账号：占满整个内容区域；
- 2 个账号：左右各占一半；
- 3 个账号：上方两个、下方一个自适应；
- 4 个账号：2×2；
- 超过 4 个：生成子页，在 AI 页停留时每 10 秒自动翻页。

页面右上角显示额度刷新时间；每个额度层级显示进度、剩余或重置说明。供应商 Logo 位于账号名称前，未知供应商使用文字回退标识。

额度后台任务开机等待约 5 秒后首次执行，之后每 5 分钟自动刷新。请求串行进行，单个请求超时 10 秒，响应体最多 16 KB。失败时保留最后成功值并标记 stale/error，避免短暂断网让屏幕完全失去数据。

### 3.2 三类按键操作

| 操作 | 功能 |
|---|---|
| BOOT 单击（GPIO0） | 启动阶段进入配网；正常运行时切换小智对话状态 |
| USER 单击（GPIO18） | 按后台配置顺序切换已启用页面；在 AI 多子页场景下先切下一个 AI 子页 |
| USER 双击 | 刷新天气、传感器和时间等数据 |
| USER 长按 | 在综合页的小智区域滚动显示 CPU、运行时间、SRAM、PSRAM、电池和 Wi-Fi 信息 |

任何按键操作都会退出省电状态。连续 5 分钟无操作后，UI 周期刷新由 1 秒降低为 5 秒。

## 4. AI 额度管理

### 4.1 支持的数据源

| provider | 默认接口 | 凭据方式 |
|---|---|---|
| `codex` | `https://chatgpt.com/backend-api/wham/usage` | Bearer access token；可附加 ChatGPT Account ID |
| `kimi` | `https://api.kimi.com/coding/v1/usages` | Bearer API key |
| `glm-cn` | `https://open.bigmodel.cn/api/monitor/usage/quota/limit` | Authorization API key |
| `glm-global` | `https://api.z.ai/api/monitor/usage/quota/limit` | Authorization API key |
| `deepseek` | `https://api.deepseek.com/user/balance` | Bearer API key |
| `generic-json` | 后台填写 URL | 可选 Bearer 凭据及总量/剩余字段名 |
| `manual` | 不联网 | 后台直接填写总量和剩余量 |

这些供应商接口不是统一稳定标准，特别是 ChatGPT 后端接口可能变更。适配异常时先区分：HTTP 状态错误、认证失效、JSON 字段变化、TLS/代理错误，而不是直接修改 UI。

### 4.2 凭据安全

- 已保存的 secret/API Key 不回显给浏览器，只返回 `has_secret`；
- 编辑时密钥留空代表保留旧值；
- 管理员密码保存为随机盐加 SHA-256 摘要，不保存明文；
- 登录 Cookie 为 `HttpOnly; SameSite=Strict`，会话闲置 30 分钟失效；
- 所有后台写请求需要会话 Cookie 和 `X-CSRF-Token`；
- HTTP 后台本身没有 TLS，只应在可信局域网使用，不应做公网端口映射。

### 4.3 余额查询代理

每个非手动账号都可独立启用代理：

```text
http://192.168.2.2:7890
```

当前只支持**无认证 HTTP CONNECT 代理**。代理仅用于该账号的余额查询，不改变天气、节假日、语音或后台网络。代理建立隧道后仍校验目标 HTTPS 证书。

遇到代理问题依次检查：

1. ESP32 与代理机是否在同一局域网；
2. 代理是否监听局域网地址，而非仅监听 `127.0.0.1`；
3. 防火墙是否允许代理端口；
4. 代理是否支持 CONNECT 到 443；
5. 后台账号卡片中是否真正保存了 `proxy_enabled` 和 `proxy_url`。

## 5. 后台认证与 API

### 5.1 首次设置与正常登录

首次打开 `/admin` 时设置 8～72 位密码，用户名固定为 `admin`。后续访问使用该密码登录。

直接请求 `/api/quotas` 而没有有效 `sid` Cookie 会得到 401。下面这种写法是无效的：

```bash
curl -b 'V' http://DEVICE_IP:8080/api/quotas
```

`V` 不是设备签发的会话 Cookie。正确的脚本调用顺序如下：

```bash
DEVICE=http://192.168.2.71:8080

# 登录并保存 Set-Cookie；不要把真实密码提交到仓库或 shell 历史。
curl -sS -c /tmp/rlcd-cookie.txt \
  -H 'Content-Type: application/json' \
  -d '{"password":"YOUR_ADMIN_PASSWORD"}' \
  "$DEVICE/api/login"

# GET 请求只需要真实会话 Cookie。
curl -sS -b /tmp/rlcd-cookie.txt "$DEVICE/api/quotas"
```

登录响应中的 `csrf` 是写请求所需令牌：

```bash
curl -sS -b /tmp/rlcd-cookie.txt \
  -H 'Content-Type: application/json' \
  -H 'X-CSRF-Token: CSRF_FROM_LOGIN_RESPONSE' \
  -X POST "$DEVICE/api/refresh"
```

若浏览器登录后仍循环回登录页，检查：设备 IP 是否变化、Cookie 是否被禁用、是否同时用 IP 和主机名访问、设备是否重启导致内存会话失效，以及密码是否确实为 8 位以上。

### 5.2 后台路由

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/admin`、`/` | 内嵌管理页面 |
| POST | `/api/setup` | 首次设置管理员密码 |
| POST | `/api/login`、`/api/logout` | 登录和退出 |
| GET | `/api/status` | 登录状态、设备 IP、刷新状态 |
| GET/PUT | `/api/pages` | 页面顺序与启停 |
| GET/PUT | `/api/quotas` | 额度账号读取与整体保存 |
| POST | `/api/refresh` | 请求立即刷新额度 |
| GET/POST | `/api/todos` | 待办列表和新增 |
| GET/PUT/DELETE | `/api/todos/{id}` | 单条待办管理 |
| GET/PUT | `/api/weather` | 天气位置和凭据配置 |
| GET/PUT | `/api/calendar` | 节假日数据源配置 |
| POST | `/api/calendar/sync` | 立即同步本年度节假日 |
| GET/POST | `/api/api-token` | 查看或重新生成待办 API Token |

后台页面使用 Cookie 会话。待办接口还支持独立 Bearer Token，适合 Home Assistant、快捷指令或局域网脚本，不需要 CSRF。

### 5.3 待办 API 示例

在后台“待办 API”区域复制 Token：

```bash
DEVICE=http://192.168.2.71:8080
TOKEN='TOKEN_FROM_ADMIN'

# 列表
curl -sS -H "Authorization: Bearer $TOKEN" "$DEVICE/api/todos"

# 新增
curl -sS -X POST \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"content":"提交周报","due_date":"2026-08-10","due_time":"18:00"}' \
  "$DEVICE/api/todos"

# 完成
curl -sS -X PUT \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"completed":true}' \
  "$DEVICE/api/todos/TODO_ID"

# 删除
curl -sS -X DELETE \
  -H "Authorization: Bearer $TOKEN" \
  "$DEVICE/api/todos/TODO_ID"
```

待办字段：`id`、`content`、`due_date`、`due_time`、`completed`、`order`、`created_at`、`updated_at`。设备升级时会尝试从旧 `memo` 数据迁移。

## 6. 天气、日历与农历

### 6.1 天气

天气由 QWeather 获取。后台需配置：

- Location ID、经纬度或 `auto_ip`；
- 屏幕显示城市名称；
- QWeather API Host；
- API Key。

天气管理器同时请求实时天气和七日预报，并保存最后有效快照。综合页显示当前天气，天气页显示七日数据。API Host 应填写控制台分配的 Host，不要默认假设公共 Host 永久可用。

### 6.2 节假日与农历

默认年度数据源：

```text
https://raw.githubusercontent.com/NateScarlet/holiday-cn/master/{year}.json
```

`{year}` 在同步时替换为当前年份。日历页用“休”表示放假，用“班”表示调休上班；其他日期显示农历日。节假日同步失败会保留旧缓存，避免日历退化为空白。

农历转换在设备本地完成，不依赖节假日接口。修改算法时要覆盖春节前后、闰月、跨年和支持年份边界测试。

## 7. 持久化与分区

本板使用自定义 16 MB 分区表：`partitions/v2/16m_rlcd_quota.csv`。

| 分区 | 偏移 | 大小 | 用途 |
|---|---:|---:|---|
| `nvs` | `0x9000` | 16 KB | Wi-Fi 及旧设置 |
| `otadata` | `0xd000` | 8 KB | OTA 状态 |
| `phy_init` | `0xf000` | 4 KB | RF 参数 |
| `ota_0` | `0x20000` | `0x4F0000` | 应用槽 0 |
| `ota_1` | `0x510000` | `0x4F0000` | 应用槽 1 |
| `assets` | `0xA00000` | `0x5C0000` | 字体、表情等资源 |
| `quota_nvs` | `0xFC0000` | 256 KB | 管理密码、Token、额度配置和缓存 |

关键 NVS 命名空间：

- `quota_nvs/admin`：管理员密码摘要、盐和待办 API Token；
- `quota_nvs/quota`：额度项、页面配置和额度缓存；
- 默认 NVS 的 `weather`、`calendar`、`todos`：对应业务配置；
- `memo`：旧备忘数据及兼容逻辑。

只写应用分区不会清除配置；全片擦除或更换分区表会丢失 Wi-Fi、管理员密码、API Token 和业务数据。升级前如需保留数据，应先通过后台或接口导出可恢复配置。

## 8. 构建环境与编译

本次验证环境：

- ESP-IDF 5.5.2；
- Python 3.9.6；
- Ninja 1.12.1；
- 项目版本 3.6.5；
- 目标 `esp32s3`；
- `CONFIG_BOARD_TYPE_WAVESHARE_S3_RLCD_4_2=y`；
- 16 MB Flash；
- 自定义分区表 `partitions/v2/16m_rlcd_quota.csv`。

### 8.1 推荐的可移植构建方式

```bash
cd /Users/shaqisheng/codex/esp32/xiaozhi-esp32
. /Users/shaqisheng/codex/esp32/esp-idf-v5.5.2/export.sh

# 首次或切换芯片/板型时执行。
idf.py set-target esp32s3
idf.py menuconfig
# Board Type 选择 Waveshare ESP32-S3-RLCD-4.2，并确认 16 MB Flash。

idf.py build
```

项目也提供发布脚本，它会读取板级 `config.json`、选择板型、应用分区配置并打包：

```bash
python scripts/release.py waveshare-s3-rlcd-4.2 \
  --name waveshare-s3-rlcd-4.2
```

注意：发布脚本若发现同版本 ZIP 已存在会跳过；开发迭代通常直接使用 `idf.py build` 更直观。

### 8.2 本机快速增量编译

已有 CMake 配置时：

```bash
/Users/shaqisheng/codex/esp32/.espressif-idf/tools/ninja/1.12.1/ninja \
  -C /Users/shaqisheng/codex/esp32/xiaozhi-esp32/build
```

最终验证固件：

```text
xiaozhi.bin 大小：0x4a57f0（约 4.6 MB）
最小应用分区：0x4f0000
剩余：0x4a810（约 6%）
SHA-256：955861f76c3b505987875cab5df95966a757a62f9ba6cc7ca859e1d4bc8e494a
```

应用分区余量已经不大。加入新字体、TLS 组件或大段内嵌 HTML 前必须查看 `check_sizes.py` 输出，不能只看编译是否成功。

## 9. 烧录与启动验证

### 9.1 常规烧录

```bash
idf.py -p /dev/cu.usbmodem21101 flash monitor
```

串口名会因电脑和 USB 口变化，可先查找：

```bash
ls /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* 2>/dev/null
```

### 9.2 已验证的完整烧录布局

在 `build` 目录执行：

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

不要漏烧 `generated_assets.bin`，否则字体、表情或图标可能缺失。首次使用新分区表时应完整烧录，避免旧 assets/NVS 布局与新固件不一致。

### 9.3 验证标准

烧录成功不等于交付完成。至少监控 60 秒串口：

```bash
python -m serial.tools.miniterm /dev/cu.usbmodem21101 115200
```

本次最终验证结果：

- 烧录后连续运行约 77 秒，无重启；
- free SRAM 稳定在约 38～39 KB；
- minimal SRAM 为 20,747 bytes；
- 待办刷新、时间更新和后台任务正常；
- 未再出现 `ESP_ERR_NO_MEM`。

建议验收时继续检查：

1. 四个页面可循环；
2. AI 页 1、2、3、4、5 个账号的布局和分页；
3. 后台首次设置、退出、重新登录和会话超时；
4. 天气和节假日断网后的缓存显示；
5. 待办 Token 的 GET/POST/PUT/DELETE；
6. 代理启用和关闭后的额度请求；
7. 设备运行 30 分钟以上无异常重启。

## 10. 关键故障与解决经验

### 10.1 后台登录成功但 API 401

**现象**：浏览器或 curl 请求 `/api/quotas` 报错。

**根因**：接口不是仅靠密码或任意 Cookie 字符串授权；必须使用 `/api/login` 返回的真实 `sid` Cookie。写请求还必须携带同一会话的 CSRF Token。

**经验**：调试认证时先看 HTTP 状态和 `Set-Cookie`，使用 curl 的 `-c` 保存 Cookie、`-b` 回放 Cookie，不要手写猜测 Cookie。

### 10.2 页面对象过多导致刷屏崩溃

**现象**：启动后约几十秒出现 RLCD SPI 发送失败、`ESP_ERR_NO_MEM`、abort 和重启；minimal SRAM 曾降至约 387 bytes。

**根因**：最初日历使用 42 个日期格，每格创建多个 LVGL 标签；天气预报也为 7 天创建大量独立控件。LVGL 对象、样式和刷新缓冲同时占用内部 SRAM，PSRAM 充足并不能替代 SPI/DMA 所需内部内存。

**解决**：

- 日历改为标题加一个多行网格标签；
- 七日天气改为少量标题、当前天气标签和一个多行预报标签；
- 保持视觉信息密度，同时将对象数量从上百个降至个位数级；
- 重新编译、完整烧录并监控 minimal SRAM。

**通用经验**：在 ESP32 的 LVGL 页面上，优先减少对象数量，再考虑缩小文本或搬到 PSRAM。能由一个标签排版的表格，不要机械地为每个单元格创建三个对象。

### 10.3 设计文档与实现发生偏移

早期 `docs/quota-admin-design.md` 仍描述天气、音乐、番茄钟、额度四页，也曾描述标题 `QUOTA` 和页码。当前实现已经变更为综合、日历、天气、AI 四页，并去掉 AI 页的 `1/1`。以后以本手册和当前代码为准；架构变化时应同步更新旧设计文档，避免后来者按过期界面验收。

### 10.4 功能“从页面移除”不等于代码完全删除

音乐和番茄钟页面当前不实例化，板级番茄钟工具块已禁用，但通用框架或旧源文件仍可能包含音乐/番茄钟符号。若目标是彻底减小固件，需要进一步：

- 从构建源列表排除旧 UI 和 manager；
- 删除 `CustomLcdDisplay` 中兼容字段与空实现；
- 删除 `DataUpdateTask` 中对应空指针分支；
- 检查通用 MCP 注册是否仍暴露音乐工具；
- 完整回归语音状态、页面切换和链接结果。

不要只根据屏幕上“看不到页面”就宣称二进制中已彻底移除功能。

### 10.5 修改分区表后的风险

自定义分区表保持两个 OTA 应用槽，但压缩了 assets 并增加 `quota_nvs`。任何偏移调整都必须同步：

- `config.json` 的分区表选择；
- 编译生成的 partition table；
- esptool 烧录偏移；
- OTA 包大小；
- assets 实际大小。

错误偏移可能让固件看似写入成功，却在启动或读取资源时失败。

## 11. 代码维护建议

### 11.1 提交前检查

```bash
git diff --check
git status --short
idf.py build
```

后台 HTML/JS 内嵌在 C++ 原始字符串中。修改 JavaScript 后，建议抽取或复制脚本内容用 `node --check` 做语法检查，并在真实浏览器中验证登录、增删账号、预览和移动端布局。

### 11.2 当前技术债

1. `music_ui.cc`、`pomodoro_ui.cc` 及相关字段仍由 glob 纳入构建或保留兼容代码，可继续清理以释放应用空间。
2. `data_update_task.cc` 仍有旧音乐控件空指针分支和惰性番茄状态读取。
3. `custom_lcd_display.h` 中保留未使用的日历/天气控件数组，可移除以减少误导。
4. 后台全部 HTML/CSS/JS 位于一个 C++ 字符串，迭代效率低；可增加构建时资源嵌入流程，同时保持离线运行。
5. QWeather、Codex 和其他额度接口均依赖第三方契约，需要解析回归样本。
6. 当前应用分区仅约 6% 余量，新增功能前应先做二进制体积预算。
7. 工作树中存在 `*.dataless-backup` 临时备份文件，确认无恢复需求后不应提交到版本库。

### 11.3 推荐测试层次

- **纯逻辑测试**：农历转换、节假日解析、额度百分比、重置时间、Todo JSON 校验。
- **HTTP 测试**：登录、Cookie、CSRF、Bearer Token、非法 body、超长字段、账号上限 32。
- **UI 测试**：1～5 个额度项、长账号名、未知 Logo、中文字体、跨月日历。
- **硬件测试**：SPI 刷屏、低内存、Wi-Fi 断连、USB/电池供电、按键去抖。
- **耐久测试**：至少 30 分钟，覆盖一次 5 分钟额度刷新及多次 10 秒 AI 子页切换。

## 12. 电池与供电说明

电池充电能力由开发板电源/充电芯片和硬件连接决定，不是该应用固件提供“反向充电”。固件只读取电量及充放电状态。使用 USB 给板载电池接口充电前应以 Waveshare 原理图和硬件手册确认电池类型、极性、电压及充电路径；不要把电池端口当作通用输出或 OTG 反向供电口。

## 13. 后续改动的推荐流程

1. 先把需求转换成页面状态、数据源、存储字段和 API 变更清单。
2. 先设计 400×300 单色稿，确认一屏信息量、长文本和空状态。
3. 评估内部 SRAM 对象数、应用分区余量和 NVS 单值大小。
4. 数据层先提供线程安全快照，再连接 LVGL。
5. 后台修改同时补接口示例和认证说明。
6. 编译后检查分区余量，完整烧录后监控串口至少 60 秒。
7. 用 0、1、2、3、4、5 和最大数量数据验收弹性布局。
8. 更新本手册、设计文档及变更记录，再提交代码。

## 14. 快速恢复清单

当后续维护者拿到开发板时，按以下顺序最快恢复现场：

```text
1. 确认仓库：/Users/shaqisheng/codex/esp32/xiaozhi-esp32
2. 确认板型：waveshare-s3-rlcd-4.2
3. 导出 ESP-IDF 5.5.2 环境
4. 确认 sdkconfig 中 ESP32-S3、16 MB Flash 和正确板型
5. 编译并关注 app partition 剩余空间
6. 查找实际串口，不要硬编码旧端口
7. 完整烧录 bootloader、partition、ota_data、app、assets
8. 监控 115200 串口至少 60 秒，关注 reboot、abort、NO_MEM、minimal sram
9. 打开 http://设备IP:8080/admin 验证登录
10. 验证四页、AI 额度刷新、天气、节假日和 Todo API
```

这份手册记录的是当前工作树和已经烧录验证的状态。后续若合并上游、调整分区或清理遗留功能，应重新生成固件校验值并更新“当前成果”“技术债”和“启动验证”三节。

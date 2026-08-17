# DEVELOPMENT_LOG

> 本文件记录本仓库所有有意义的代码修改。
> **每次代码修改后必须追加一条**（模板见末尾）。
> 倒序排列（最新在最上）。

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

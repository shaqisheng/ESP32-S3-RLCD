# DEVELOPMENT_LOG

> 本文件记录本仓库所有有意义的代码修改。
> **每次代码修改后必须追加一条**（模板见末尾）。
> 倒序排列（最新在最上）。

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

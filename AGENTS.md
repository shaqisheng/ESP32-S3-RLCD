# AGENTS.md — Codex / AI Agent 长期工作规范

> 本文件是本仓库所有 AI 协作（Codex、Cursor、Claude、ZCode 等）的强制规范。
> 每次开始任务前必须完整阅读本文件，以及 `PROJECT_CONTEXT.md` 和 `PROJECT_CLEANUP_PLAN.md`。
> 修改完成后必须按 §6 更新 `DEVELOPMENT_LOG.md`，架构变化时同步 `ARCHITECTURE.md`。

---

## 1. 项目背景（30 秒读懂）

- **本项目**：Waveshare ESP32-S3-RLCD-4.2 桌面信息屏固件。基于上游 `78/xiaozhi-esp32`（小智 AI 语音助手），fork 出来扩展为综合页/日历/七日天气/AI 额度四页 + 局域网后台。
- **当前唯一生产板**：`main/boards/waveshare-s3-rlcd-4.2/`。上游继承的其余 116 个板目录已于 2026-08-17 应用户要求全部删除（与本硬件无关、不参与编译）。`main/boards/common/` 是所有板共享的通用层，本板基类来自这里，**不要删**。
- **资源硬约束**：内部 SRAM 紧张、应用分区仅剩约 6%、单会话后台、明文 HTTP（仅可信局域网）。
- **代码状态**：当前 git 只有"初始化"一个提交，所有业务文件在 working tree。`PROJECT_HANDOFF.md` 描述的是迁移前的旧版本（部分过时），以**当前代码**为准。
- 完整介绍见 `PROJECT_CONTEXT.md`。

---

## 2. 开发原则

0. **设计先行 + 烧录确认**（最高优先级，覆盖以下所有原则）：
   - **涉及前端页面（设备屏 LVGL UI、后台管理界面）的修改，必须先出可视化设计稿（HTML mockup 或 ASCII 布局图）让用户确认后再编码**。不允许"边写边看效果"。参考 `docs/mockups/` 的现有 mockup。
   - **编译和烧录设备前必须先告诉用户"我准备编译/烧录了"，得到明确同意（如"继续"/"烧"/"可以"）才执行**。不允许擅自 `idf.py build` 或 `esptool write_flash`。

1. **小步、可验证、可回滚**。每次改动尽量能在 5 分钟内编译 + 真机验证。
2. **优先复用现有安全基元**（`manager_safety.h`、`proxy_auth.h`），不要造轮子。
3. **资源优先**：内部 SRAM 比 PSRAM 稀缺；Flash 比 CPU 稀缺。新增任何 LVGL 对象、字体、资源前先评估。
4. **离线优先**：网络抖动时桌面屏仍要可读，所有外部请求失败保留最后有效快照。
5. **不改上游通用层**（`main/` 顶层、`boards/common`），除非任务明确要求且用户已确认。
6. **不引入新的运行时依赖**（尤其是 C++ 第三方库）；优先用 ESP-IDF 自带组件。
7. **当文档与代码冲突时，以代码为准**，并立刻更新文档。

---

## 3. 代码规范

### 3.1 格式

- 使用项目根目录 `.clang-format`（Google 基础，4 空格缩进，行宽 100，指针靠左，Attach 大括号，自动排序 include）。
- 提交前运行：
  ```bash
  find main/boards/waveshare-s3-rlcd-4.2 -iname "*.cc" -o -iname "*.h" | xargs clang-format -i
  git diff --check   # 不应有 whitespace 错误
  ```
- 详见 `docs/code_style.md`。

### 3.2 命名

- 类：`CamelCase`（如 `QuotaManager`、`CustomLcdDisplay`）。
- 函数/变量：`snake_case_`（成员变量带尾下划线，如 `latest_data_`、`display_mode_`）。
- 常量/枚举：`kCamelCase`（如 `kMaxRequest`、`MODE_OVERVIEW`）或 `UPPER_SNAKE`（宏）。
- 文件：lower_case_with_underscores（`quota_manager.cc`）。
- 与现有命名保持一致；不要混入新风格。

### 3.3 注释

- 代码已大量使用中文注释，**保持中文**，不要强行翻译。
- 关键决策（为何这样做）写注释；what 注释只在意图不明显时写。
- 引用其他文件用 `path:line` 格式（如 `// 见 custom_lcd_display.cc:345`）。

### 3.4 头文件

- 所有 manager 类用单例（`GetInstance()`），与现有保持一致。
- 内联工具类放 header-only（参考 `manager_safety.h`、`proxy_auth.h`）。
- include 用 `""` 引本板文件，`<>` 引 ESP-IDF 和第三方。

---

## 4. 文件组织规则

### 4.1 本板目录结构（必须遵守）

```
main/boards/waveshare-s3-rlcd-4.2/
├── waveshare-s3-rlcd-4.2.cc   # 板级入口（CustomBoard）
├── config.h                   # 引脚/常量
├── config.json                # 构建变体（选择分区表等）
├── README.md                  # 板级说明
├── *_ui.cc                    # 各 LVGL 页 UI（Setup + Update 函数）
├── custom_lcd_display.{h,cc}  # LcdDisplay 子类
├── data_update_task.cc        # 周期数据任务
├── rlcd_driver.{h,cc}         # 显示驱动
├── secret_config.h.example    # 密钥模板（真实 secret_config.h 被 gitignore）
├── assets/                    # 图标/字体（C 数组）
└── managers/                  # 业务 manager（每个一对 .h/.cc）
    └── manager_safety.h, proxy_auth.h  # 共享基元（header-only）
```

### 4.2 新增文件规则

- 新 manager：放 `managers/`，命名 `xxx_manager.{h,cc}`，单例模式。
- 新 UI 页：放板级根目录，命名 `xxx_ui.cc`，导出 `SetupXxxUI()` 和（如需）`UpdateXxxInternal()`。
- 新增 `.cc`/`.c` 文件**会被 CMake glob 自动纳入编译**（见 `main/CMakeLists.txt:651-659`），无需手改 CMake。
- 新增头文件 include 路径已配置，直接 `#include "managers/xxx_manager.h"`。

### 4.3 禁止新增的文件类型

- ❌ 不要创建 `*.dataless-backup`、`*.bak`、`*.orig`（macOS 编辑器残留）
- ❌ 不要在仓库内创建临时调试文件（用 `/tmp` 或 git stash）
- ❌ 不要创建未跟踪的 build 产物

---

## 5. 修改代码规则

### 5.1 修改前的强制步骤

1. **读 `AGENTS.md`**（本文件）、`PROJECT_CONTEXT.md`、`PROJECT_CLEANUP_PLAN.md`。
2. **确认任务范围**：是本板？上游通用？跨板？
3. **查 `docs/superpowers/plans/`** 是否有相关迭代计划，已实现到哪一步（看 checkbox）。
4. **检查当前 git 状态**：`git status --short`，确认没有未提交的无关改动混入。

### 5.2 修改中的规则

- **每次只解决一个问题**，不要顺手"清理"无关代码（那是另一个 PR）。
- **不破坏 ARCHITECTURE.md §7 列出的不变量**（网络启动顺序、可取消 IO、原子提交、ThreadSafeSnapshot、secret 不回显、CSRF、CMake glob、LVGL 对象预算、分区表）。
- **新增联网代码必须**：
  - 在循环里检查 `BackgroundNetworkCancelled(generation)`；
  - 用 `BackgroundNetworkSession` 串行化；
  - 设超时（≤10 秒）和响应体上限（≤16 KB）；
  - 失败时保留旧快照（stale 模式）。
- **新增持久化代码必须**：用 `CommitValidatedUpdate` 模式（拷贝→校验→持久化→提交），失败回滚。
- **新增 UI 代码必须**：
  - 用少量多行 `lv_label` 替代大量小对象；
  - 不要在 SPI 刷屏关键路径分配大块内部 SRAM；
  - 改完后真机验证 minimal SRAM ≥ 20KB。
- **新增后台路由必须**：
  - 写操作用 `IsAuthorized(req, write=true)`（Cookie + CSRF）；
  - Todo 自动化可用 Bearer；
  - GET 接口不回显 secret；
  - body 大小有上限（≤48 KB）。
- **修改密钥相关代码必须**：只写不读，空值=保留，显式 clear=删除。

### 5.3 修改后的强制步骤

1. `clang-format -i` 改过的文件。
2. `git diff --check`。
3. **告诉用户准备编译/烧录，得到明确同意后再执行**（§2.0 硬性要求）：
   - 编译：`idf.py build`（确认 app 分区没超）。
   - 真机烧录 + 观察 60 秒（搜索 `abort`/`reboot`/`ESP_ERR_NO_MEM`/`rlcd spi tx failed`）。
4. 后台 JS 若改动：`node --check` 或在 `tests/host/rlcd_ui_source_contract_test.py` 里加新契约并跑通。
5. 更新 `DEVELOPMENT_LOG.md`。
6. 架构或核心决策变化时同步 `ARCHITECTURE.md`。
7. 新增功能/路由时同步 `PROJECT_CONTEXT.md` §5.5。

---

## 6. 禁止事项

### 6.1 绝对禁止（任何情况下）

- ❌ 在未提交 working tree 前做大规模重构（先 commit 再改）
- ❌ 删除或弱化安全检查（CSRF、Cookie 校验、TLS 证书校验、CRLF 过滤）
- ❌ 在 GET 接口回显 secret、api_token、密码、proxy 凭据
- ❌ 把后台暴露到公网（路由器端口映射）
- ❌ 修改分区表后只 OTA 不完整烧录
- ❌ 在 `ApplyConfigJson` 等持锁路径加 NVS/网络 IO
- ❌ 在构造函数中启动网络服务（必须等 `WifiBoard::StartNetwork()`）
- ❌ 在 LVGL 关键路径分配大块内部 SRAM
- ❌ 改动 `main/boards/common/` 共享层（本板依赖它；除非任务明确要求且用户确认）
- ❌ 删 `idf_component.yml` 任何依赖（多板共享）
- ❌ 提交 `sdkconfig`、`build/`、`managed_components/`、`dependencies.lock`、`*.bin`、`secret_config.h`、`*.dataless-backup`（前 6 个已被 .gitignore）

### 6.2 强烈不推荐

- ⚠️ 引入新的 C++ 第三方库（增加 Flash 占用、可能不被 ESP-IDF 支持）
- ⚠️ 在 data_update_task 单文件里堆叠更多职责（应拆函数）
- ⚠️ 用 `std::string` 拼接构造大 JSON（用 cJSON 或预分配 buffer）
- ⚠️ 在 HTTP handler 里执行长 IO（应排队后台任务）
- ⚠️ 把上游通用层和本板代码混在一个 commit

---

## 7. 测试要求

### 7.1 现状

- **没有单元测试框架**（无 Unity / Catch2 / doctest）。
- `tests/host/rlcd_manager_safety_test.cc`：裸 `<cassert>` + `main()`，需手动 `g++` 配头文件路径。
- `tests/host/rlcd_ui_source_contract_test.py`：`unittest`，对源码文本做契约断言（防回归）。可独立跑：
  ```bash
  python3 -m unittest tests.host.rlcd_ui_source_contract_test
  ```
- CI（`.github/workflows/build.yml`）**只编译，不跑测试**。

### 7.2 何时必须新增测试

- 修改 `manager_safety.h` / `proxy_auth.h` → 给 `rlcd_manager_safety_test.cc` 加 case
- 修改 UI 源码契约（页面 ID、控件命名、字段命名） → 同步更新 `rlcd_ui_source_contract_test.py`
- 修改额度解析（codex/kimi/glm/deepseek）→ 建议加 fixture JSON + 解析回归
- 修改农历/节气算法 → 加日期边界 case

### 7.3 最低验证标准

每次改动至少完成：

1. `idf.py build` 通过
2. `git diff --check` 通过
3. 改了后台 JS → `node --check` 通过 / 契约测试通过
4. 真机烧录后串口观察 60 秒无 `abort`/`reboot`/`NO_MEM`

---

## 8. 文档维护要求

| 文档 | 何时更新 |
|---|---|
| `DEVELOPMENT_LOG.md` | **每次代码修改后必写一条**（见模板 §9） |
| `ARCHITECTURE.md` | 模块关系、数据流、不变量、设计决策变化时 |
| `PROJECT_CONTEXT.md` | 新增/删除功能模块、路由、Manager 时；§5.5 API 表、§6 状态 |
| `PROJECT_CLEANUP_PLAN.md` | 发现新债务或清理掉旧债务时 |
| `docs/waveshare-s3-rlcd-4.2-project-handbook.md` | 用户可见行为或操作流程变化时 |
| `docs/superpowers/plans/` | 开始一个有计划的新任务时新建计划文件 |
| `AGENTS.md` | 工作规范本身变化时 |

发现文档与代码冲突时：**先修代码、再立刻修文档**，不要留 TODO。

---

## 9. DEVELOPMENT_LOG 条目模板

每次修改后追加一条到 `DEVELOPMENT_LOG.md`：

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

---

## 10. Git 工作流

### 10.1 分支

- 主开发在 `main`，但**重大改动用 feature 分支**：`feat/<topic>`、`fix/<topic>`、`chore/<topic>`。
- 远程是 `shaqisheng/ESP32-S3-RLCD`，**不要**未经允许推送（这是个人 fork）。

### 10.2 提交信息

- 中文，简洁。前缀建议：`feat:`/`fix:`/`refactor:`/`docs:`/`chore:`/`test:`。
- 示例：`feat: 后台待办编辑后立即刷新硬件 UI`。

### 10.3 提交粒度

- 一个逻辑改动一个 commit。
- **不要把"清理 whitespace"和"功能修改"混在一起**。
- **不要在功能 commit 里夹带未声明的"顺手清理"**。

### 10.4 当前最重要的事

由于 git 只有一个"初始化"commit，**第一次正式工作前应先建议用户建立基线提交**：

```bash
git switch -c handoff/baseline-2026-08-11
# 排除 *.dataless-backup、secret_config.h
git add main sdkconfig.defaults partitions docs scripts tests CMakeLists.txt .clang-format .github
git status --short   # 复查
git commit -m "chore: 导入 waveshare-s3-rlcd-4.2 信息屏固件基线"
```

然后所有后续改动基于这个基线。

---

## 11. 沟通规则

- **不确定时问用户**，不要擅自决定方向（特别是删功能、改架构、动上游代码、改分区表）。
- 用 `AskUserQuestion` 提供选项时，把推荐项放第一个并标注 `(Recommended)`。
- 报告结果要忠实：测试失败就说失败，跳过了就说跳过，不要美化。
- 删除/覆盖前先看目标（"look before you leap"），与描述矛盾就先停下汇报。
- 不要在没验证的情况下说"完成"。

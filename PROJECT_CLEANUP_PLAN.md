# PROJECT_CLEANUP_PLAN

> 本文件基于 2026-08-11 对**当前代码**的实际引用分析生成。
> 每个删除/修改建议都附带证据（grep 结果），不基于猜测。
> 执行任何条目前必须先读 `AGENTS.md`，并在 `DEVELOPMENT_LOG.md` 记录。

---

## 引用分析证据汇总

```
# 证据 1：SetupMusicUI / SetupPomodoroUI 从不被调用
$ grep -rn "SetupMusicUI\|SetupPomodoroUI" main/boards/waveshare-s3-rlcd-4.2/
music_ui.cc:41:void CustomLcdDisplay::SetupMusicUI()          # 仅定义
pomodoro_ui.cc:39:void CustomLcdDisplay::SetupPomodoroUI()    # 仅定义
custom_lcd_display.h:28-29,170-171                            # 仅注释和声明
（没有任何 .cc 文件调用它们 → 完全死代码）

# 证据 2：music_*/pomo_*/pomodoro_page_ 字段在 display + data_update_task 出现 49 次
$ grep -rn "music_page_\|pomodoro_page_\|music_time_label_\|...pomo_*_label_" main/boards/waveshare-s3-rlcd-4.2/
# 共 49 次，全部位于 custom_lcd_display.{h,cc} 和 data_update_task.cc
# 但因 SetupMusicUI/SetupPomodoroUI 从不执行，这些指针恒为 nullptr，所有 lv_label_set_text(...) 调用被 nullptr 守卫跳过

# 证据 3：mcp_server 仍注册 self.music.play_url，板级 DisableTool 屏蔽
$ grep -n "music.play_url" main/mcp_server.cc main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc
waveshare-s3-rlcd-4.2.cc:269: mcp_server.DisableTool("self.music.play_url");
mcp_server.cc:72: AddTool("self.music.play_url", ...)

# 证据 4：application.cc 含 fork 自加的音乐框架（约 80 行 + .h 6 个字段）
$ grep -nE "PlayMusicFromUrl|MusicPlaybackTask|music_playing_|current_music_title_|last_played_url_" main/application.{h,cc}
# 20 处命中

# 证据 5：3 个 dataless-backup 文件未被 gitignore
$ find main -name "*.dataless-backup"
main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc.dataless-backup
main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.h.dataless-backup
main/boards/waveshare-s3-rlcd-4.2/managers/quota_manager.h.dataless-backup
$ grep "dataless" .gitignore   # 无匹配 → 当前会被 git 追踪

# 证据 6：data_update_task.cc 的 pomodoro include 注释直说 "legacy state remains inert"
$ grep -n "pomodoro_manager.h" main/boards/waveshare-s3-rlcd-4.2/data_update_task.cc
31:#include "managers/pomodoro_manager.h"  // legacy state remains inert; no page/tool starts it
```

---

## 1. 当前项目结构问题

| # | 问题 | 影响 |
|---|---|---|
| 1.1 | Git 仅有"初始化"一个 commit，所有当前业务文件**都在 working tree 未提交** | 任何机器故障或 `git checkout` 都会丢失全部工作（最高风险） |
| 1.2 | `PROJECT_VER=2.2.2`，但 `PROJECT_HANDOFF.md` 写 `3.6.5`；远程是 `shaqisheng/ESP32-S3-RLCD`，HANDOFF 写 `ZhouhaoJiang/xiaozhi-esp32`；目录也从 `codex/esp32/` 改到 `个人项目/` | 文档与现状脱节，新人误导 |
| 1.3 | `docs/quota-admin-design.md` 仍按音乐/番茄钟页面描述 | 与现状冲突 |
| 1.4 | 后台前端（13.5 KB HTML/CSS/JS）内嵌在 `admin_server.cc` 单个 raw string | 修改风险高、无构建期检查 |
| 1.5 | `tests/host/` 无构建脚本，CI 完全不跑 | 测试等于不存在 |
| 1.6 | `build/`、`build-codex/` 两个构建目录同时存在 | 占盘，来源不明（推测 build-codex 是其他 agent 用过的） |

---

## 2. 代码质量问题

### 2.1 安全（按严重度）

| # | 位置 | 问题 | 严重度 |
|---|---|---|---|
| 2.1.1 | `admin_server.cc:266-299` | 管理员密码 = 16B 随机盐 + **单轮 SHA-256**，无 KDF 迭代 | P1（NVS 物理读取后易暴力） |
| 2.1.2 | `quota_manager.cc:607` | 各供应商 `secret` 和 `proxy_url`（含 user:pass）**明文** 存 `quota_nvs` | P1（NVS 物理可读即泄露） |
| 2.1.3 | `admin_server.cc:336` | Todo Bearer Token（24B hex）**明文** 存 NVS | P2 |
| 2.1.4 | `admin_server.cc` Cookie | `sid` Cookie 无 `Secure` 属性（HTTP 明文） | P2（设计如此，靠局域网信任） |
| 2.1.5 | `admin_server.cc` 登录失败 | 仅 sleep 400ms，**无失败计数 / lockout** | P2 |
| 2.1.6 | `admin_server.cc StatusHandler` | 未登录态本应只返公开字段；当前实现已收窄但**已登录态返回 tiers 含百分比**，可能间接泄露用量 | P2 |
| 2.1.7 | `admin_server.cc` diagnostic | proxy/weather 诊断把上游 `last_result` **直接拼 JSON**（无转义），上游响应含 `"` 会破坏 JSON（自身接口崩，无 XSS） | P3 |

### 2.2 并发与正确性

| # | 位置 | 问题 | 严重度 |
|---|---|---|---|
| 2.2.1 | `quota_manager.cc:702-753` | `ApplyConfigJson` 在**持 mutex 时同步执行 NVS 写 + transport 校验**，长路径会阻塞 `GetCards` 轮询和 UI 渲染 | P2 |
| 2.2.2 | `admin_server.cc` `/api/calendar/sync` | 同步 HTTPS 下载节假日**直接在 HTTP handler 中执行**，网络慢时占用 handler 栈、浏览器卡死 | P2 |
| 2.2.3 | `calendar_manager.cc` 农历 | 24 节气为**近似公式**，临界年可能偏 1 天；1900-2049 之外直接返 `--` | P3 |
| 2.2.4 | `quota_manager.cc` | 第三方额度 API（Codex 用 ChatGPT 非公开稳定后端）**无回归 fixture**，上游变更只能运行时发现 | P2 |

### 2.3 可维护性

| # | 位置 | 问题 | 严重度 |
|---|---|---|---|
| 2.3.1 | `admin_server.cc` | 28 个路由 + 13.5 KB HTML 全在一个 628 行文件里 | P3 |
| 2.3.2 | `mcp_server.cc` 注释中文 / `application.cc` 音乐代码 | 上游 + fork 自加混在一起，未标注边界 | P3 |
| 2.3.3 | 多处 `data_update_task.cc` | 单文件 633 行承担过多职责（时钟/传感器/电池/WiFi/AI状态/天气/日历/待办/番茄） | P3 |

---

## 3. 可以删除的文件

> 删除前必须：① 再次 grep 全仓确认无引用；② 在分支上做；③ 编译通过；④ 真机验证 4 页正常；⑤ 记 DEVELOPMENT_LOG。

| # | 文件 | 行数 | 证据 | 风险 | 优先级 |
|---|---|---:|---|---|---|
| 3.1 | `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.cc.dataless-backup` | - | macOS dataless 备份，与当前源不同步；不在编译范围；不应入库 | 极低 | **P0** |
| 3.2 | `main/boards/waveshare-s3-rlcd-4.2/managers/admin_server.h.dataless-backup` | - | 同上 | 极低 | **P0** |
| 3.3 | `main/boards/waveshare-s3-rlcd-4.2/managers/quota_manager.h.dataless-backup` | - | 同上 | 极低 | **P0** |
| 3.4 | `main/boards/waveshare-s3-rlcd-4.2/music_ui.cc` | 370 | `SetupMusicUI` 从不被调用（证据 1）；music_* 字段恒 nullptr（证据 2） | 低（先删字段后删文件） | P1 |
| 3.5 | `main/boards/waveshare-s3-rlcd-4.2/pomodoro_ui.cc` | 234 | `SetupPomodoroUI` 从不被调用（证据 1） | 低 | P1 |
| 3.6 | `main/boards/waveshare-s3-rlcd-4.2/managers/pomodoro_manager.cc` | 462 | 状态恒 IDLE；唯一引用方 `data_update_task.cc:31` 注释明说 "legacy state remains inert"；MCP 工具已被 `#if 0` 关 | 中（需同步删 include 与不可达分支） | P1 |
| 3.7 | `main/boards/waveshare-s3-rlcd-4.2/managers/pomodoro_manager.h` | - | 同 3.6 | 中 | P1 |

**注意**：3.4-3.7 互相依赖，**必须作为一次原子提交**：删 `.cc` 同时删 `custom_lcd_display.h` 中相关声明与字段、`data_update_task.cc` 中相关分支、`pomodoro_manager.h` include、`#if 0` MCP 工具块。分步删会导致编译失败。

### 3.x 暂不删除但应归档

| 文件 | 原因 |
|---|---|
| `PROJECT_HANDOFF.md` | 历史快照，对理解迁移背景有价值。**在文件顶部加 ⚠️ 过时声明**，不要删 |
| `docs/quota-admin-design.md` | 同上，加过时声明 |
| `partitions/v1/` | 上游历史，不动 |

---

## 4. 可以删除的代码

| # | 位置 | 内容 | 证据 | 优先级 |
|---|---|---|---|---|
| 4.1 | `custom_lcd_display.h:170-171` | `void SetupMusicUI(); void SetupPomodoroUI();` 声明 | 证据 1 | P1（配合 3.4-3.7） |
| 4.2 | `custom_lcd_display.h` 中 `music_page_`/`pomodoro_page_`/`music_*_label_`/`pomo_*_label_` 字段 | 所有 music/pomo 相关 `lv_obj_t*` 字段 | 证据 2：恒 nullptr | P1 |
| 4.3 | `custom_lcd_display.cc` 中 `SwitchToMusicPage`/`SwitchToPomodoroPage`/`UpdatePomodoroDisplay` 实现 | 退化函数（已 fallback 到 weather） | HANDOFF §3 已确认 | P1 |
| 4.4 | `data_update_task.cc:31` | `#include "managers/pomodoro_manager.h"` | 证据 6 | P1 |
| 4.5 | `data_update_task.cc:526-608` | pomodoro 状态查询与 pomo_* label 更新分支（恒不执行） | 番茄钟恒 IDLE | P1 |
| 4.6 | `data_update_task.cc` 中所有 `music_*_label_` 引用（约 10 处） | nullptr 守卫跳过，dead path | 证据 2 | P1 |
| 4.7 | `waveshare-s3-rlcd-4.2.cc:380-476` | `#if 0 ... #endif` 番茄钟 MCP 工具块 | 注释明说屏蔽 | P2 |
| 4.8 | `waveshare-s3-rlcd-4.2.cc:269` | `DisableTool("self.music.play_url")`（如果 4.9 完成） | 上游通用层移除后此调用无意义 | P2（依赖 4.9） |

### 4.x 上游通用层（影响所有板，谨慎）

| # | 位置 | 内容 | 风险 | 优先级 |
|---|---|---|---|---|
| 4.9 | `mcp_server.cc:72-96` | `self.music.play_url` 工具注册 | **中**：上游 78/xiaozhi-esp32 可能有其他板在用；本仓库只剩 waveshare-s3-rlcd-4.2 一个生产板的话可删 | **需用户确认** |
| 4.10 | `application.{h,cc}` 音乐播放整套 | `PlayMusicFromUrl` / `MusicPlaybackTask` / `music_playing_` / `current_music_title_` / `last_played_url_` / `MusicPlaybackTaskArgs`（约 80 行 .cc + 6 字段 .h） | **中**：同 4.9，是 fork 自加而非上游原生；若未来想恢复音乐功能需保留 | **需用户确认** |

**4.9 和 4.10 必须一起决定**：要么全留（当前状态），要么全删。删之前请用户确认"是否永远不再需要音乐播放"。

---

## 5. 可以优化的模块

| # | 模块 | 优化方向 | 优先级 |
|---|---|---|---|
| 5.1 | `admin_server.cc` | 把内嵌 HTML 拆到独立 `.html`/`.js` 文件，构建时 `xxd`/CMake `embed_txt` 嵌入；同时引入前端构建期语法检查 | P3 |
| 5.2 | `quota_manager.cc:702-753` | `ApplyConfigJson` 持锁内的 NVS 写改为先在副本上修改、最后一次性 commit；或拆成 stage + commit 两阶段 | P2 |
| 5.3 | `admin_server.cc` `/api/calendar/sync` | 改为后台任务排队 + 状态轮询（`POST` 立即返 `accepted`，`GET /api/calendar/sync-status` 查进度） | P2 |
| 5.4 | `weather_manager.cc` | 24 节气近似公式换成精确天文算法，或缓存到 NVS（首次联网取，之后离线可用） | P3 |
| 5.5 | `data_update_task.cc` | 按职责拆分（时钟/传感器/电池/天气/日历/待办各一个小函数或文件） | P3 |
| 5.6 | `tests/host/` | 增加 `tests/host/CMakeLists.txt`（host target）或 `Makefile`，并接入 `.github/workflows/build.yml` | P2 |

---

## 6. 可以移除的依赖

当前 `main/idf_component.yml` 的依赖都是**通用多板支持**所需，没有针对本板的冗余。不建议删任何依赖——会破坏其他 119 个板的构建（虽然本仓库实际只用 1 个，但 Kconfig 仍列了全部）。

> **不要**为了"瘦身"删 idf_component.yml 里的项，除非确认整个仓库不再构建其他板。

---

## 7. 重构建议

### 7.1 优先：完成"彻底移除音乐/番茄"一次性重构

**目标**：让代码与现实产品形态一致，回收约 1500+ 行死代码 + 应用分区空间。

执行顺序（一次提交内完成）：

1. 物理删除 7 个文件（见 §3.1-3.7）
2. 删 `custom_lcd_display.{h,cc}` 中所有 music/pomo 字段、声明、退化函数（§4.1-4.3）
3. 删 `data_update_task.cc` 中 pomodoro include + 不可达分支 + music_* 引用（§4.4-4.6）
4. 删 `waveshare-s3-rlcd-4.2.cc` 的 `#if 0` 块（§4.7）
5. （需用户确认）删 `mcp_server.cc:72-96` + `application.{h,cc}` 音乐代码（§4.9, 4.10）；如不删则保留 `DisableTool("self.music.play_url")` 防御
6. 把 `*.dataless-backup` 加入 `.gitignore`
7. 编译 → 真机验证 4 页 → 检查 app 分区释放了多少 → 记 DEVELOPMENT_LOG

### 7.2 次优先：把测试接入 CI

- 给 `tests/host/` 加构建脚本（CMake host target 或 Makefile）
- 在 `build.yml` 加一个 `test` job
- 让 `rlcd_ui_source_contract_test.py` 每次都跑（这是防 UI 回归的最便宜手段）

### 7.3 中期：后台前端解耦

- 拆 `admin_server.cc` 的 HTML 到 `web/admin.html` 等
- 用 CMake `target_add_data` 或代码生成嵌入
- 引入 eslint/jsx 检查

### 7.4 长期：安全加固

- 密码换 PBKDF2-HMAC-SHA256（ESP-IDF mbedTLS 支持），做 schema 迁移
- 未登录 `/api/status` 完全不返 tiers
- 登录失败计数 + 指数 backoff

---

## 8. 优先级排序

| 优先级 | 任务 | 工作量 | 收益 |
|---|---|---|---|
| **P0** | 提交当前 working tree 到分支（先保存） | 10 分钟 | 防止全丢 |
| **P0** | 删 3 个 `*.dataless-backup` + 补 `.gitignore` | 5 分钟 | 防误入库 |
| **P1** | 一次性清理 music/pomodoro 死代码（§7.1 步骤 1-4） | 半天 | 回收空间 + 降低维护成本 |
| **P1** | 同步/归档过时文档（PROJECT_HANDOFF / quota-admin-design） | 30 分钟 | 防误导 |
| **P2** | 接入 tests/host 到 CI | 半天 | 防回归 |
| **P2** | 修 `ApplyConfigJson` 持锁 IO（§5.2） | 2 小时 | UI 不再被配置写阻塞 |
| **P2** | `/api/calendar/sync` 改异步（§5.3） | 半天 | 后台 UX |
| **P2** | （需用户确认）删上游 music.play_url + application 音乐代码 | 1 小时 | 进一步回收 |
| **P3** | 后台前端解耦（§7.3） | 1-2 天 | 长期可维护 |
| **P3** | 安全加固 KDF / status 收窄（§7.4） | 半天 | 安全 |
| **P3** | 长期稳定性测试（30 分钟+ 真机） | 半天 | 信心 |

---

## 9. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 清理 music/pomo 时漏删某个引用导致编译失败 | 中 | 低（编译期发现） | 增量编译 + grep 复查 |
| 清理后 LVGL 对象数变化引起 SRAM 行为变化 | 低 | 中（运行期） | 真机验证 minimal SRAM ≥ 20KB |
| 删上游 music 后某天想恢复音乐功能 | 低 | 中（需重写） | 先用 git tag 标记清理前提交；用户明确确认 |
| 应用分区清理后反而**增大**（编译器优化差异） | 低 | 低 | 比对 build size |
| 删除 dataless-backup 后丢失某次编辑历史 | 极低 | 极低 | 它们与当前源不同步，本来就不是真历史；删前可gzip备份一份到工作树外 |
| 改 `.gitignore` 后已跟踪文件仍被跟踪 | 低 | 低 | `git rm --cached` 显式取消跟踪 |

---

## 10. 不应做的事

- **不要**为了瘦身删 `idf_component.yml` 依赖（多板共享）
- **不要**在未提交 working tree 前做任何清理（先 P0）
- **不要**在没有真机验证的情况下删 LVGL 相关代码
- **不要**删 `partitions/v1/`、`README*.md`、`docs/v0/`、`docs/v1/`（上游历史）
- **不要**改分区表（除非有强需求；改了必须完整烧录）
- **不要**在 `ApplyConfigJson` 等持锁路径里加新 IO

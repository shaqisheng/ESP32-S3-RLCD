// CustomLcdDisplay 核心类
//
// 负责：
// - 构造/析构（初始化 RLCD 驱动 + LVGL + 创建 UI）
// - LVGL flush 回调（RGB565 → 1-bit 转换）
// - AI 消息适配（重写小智的 SetChatMessage / SetEmotion / ClearChatMessages）
// - 备忘录功能（加载/刷新备忘录列表）
// - 基类方法重写（UpdateStatusBar / SetTheme）
//
// 其他功能拆分到独立文件：
//   rlcd_driver.cc        - RLCD 硬件驱动
//   weather_ui.cc          - 天气站 UI 布局
//   music_ui.cc            - 音乐页 UI 布局
//   data_update_task.cc    - 后台数据更新任务

#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "application.h"
#include "lvgl_theme.h"
#include "managers/quota_manager.h"
#include "managers/todo_manager.h"

static const char *TAG = "CustomDisplay";

// ===== LVGL flush 回调 =====

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *self = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    RlcdDriver *rlcd = self->rlcd_;
    uint16_t *buffer = (uint16_t *)color_p;
    for(int y = area->y1; y <= area->y2; y++)
    {
        for(int x = area->x1; x <= area->x2; x++) 
        {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            rlcd->RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    rlcd->RLCD_Display();
    lv_disp_flush_ready(disp);
}

// ===== 构造 / 析构 =====

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    spi_display_config_t spiconfig,
    spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height)
{
    // 1. 初始化 RLCD 硬件驱动
    rlcd_ = new RlcdDriver(spiconfig, width, height, spi_host);

    // 2. 初始化 LVGL
    ESP_LOGI(TAG, "初始化 LVGL");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    int transfer = width * height;
    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
    uint8_t *lvgl_buffer1 = (uint8_t *)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
    lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 3. 初始化 RLCD 屏幕
    ESP_LOGI(TAG, "初始化 RLCD 屏幕");
    rlcd_->RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }

    // 4. 创建综合、日历、七日天气、额度和待办页 UI
    ESP_LOGI(TAG, "创建综合页 + 日历页 + 七日天气页 + AI 页 + 待办页 UI");
    SetupWeatherUI();
    SetupCalendarUI();
    SetupForecastUI();
    SetupQuotaUI();
    SetupTodoUI();
    // 告诉显示框架：当前自定义 UI 已经初始化完成
    // 否则基类的 SetStatus/ShowNotification 会一直误判为“UI 未准备好”
    setup_ui_called_ = true;
    ApplyDisplayMode();

    // 5. 启动时迁移并加载待办
    TodoManager::GetInstance().Init();
    LoadMemoFromNvs();
}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (update_task_handle_) {
        vTaskDelete(update_task_handle_);
    }
    delete rlcd_;
}

// ===== 备忘录功能 =====

void CustomLcdDisplay::LoadMemoFromNvs() {
    // 直接调用 RefreshMemoDisplay 从 NVS 读取并更新 UI
    RefreshMemoDisplay();
}

// 内部版本：不获取锁（调用者必须已持有 DisplayLock）
void CustomLcdDisplay::RefreshMemoDisplayInternal() {
    if (!memo_list_label_) return;
    auto items = TodoManager::GetInstance().List();
    std::string display_text;
    int shown = 0, pending = 0;
    for (const auto& item : items) {
        if (item.completed) continue;
        pending++;
        if (shown >= 3) continue;
        if (shown++) display_text += "\n";
        display_text += "○ ";
        if (!item.due_time.empty()) display_text += item.due_time + " ";
        display_text += item.content;
    }
    if (shown == 0) display_text = "暂无待办";
    else if (pending > shown) display_text += "\n+" + std::to_string(pending - shown) + " 项待办";
    lv_label_set_text(memo_list_label_, display_text.c_str());
    ESP_LOGI(TAG, "待办列表已刷新，共 %d 条未完成", pending);
}

// 外部版本：自动获取锁（供 MCP 工具等外部调用）
void CustomLcdDisplay::RefreshMemoDisplay() {
    DisplayLockGuard lock(this);
    RefreshMemoDisplayInternal();
}

// ===== AI 消息适配（重写小智的方法，只更新左下角卡片）=====

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_status_label_ == nullptr && music_chat_status_label_ == nullptr) return;
    if (!content || strlen(content) == 0) return;

    // 停止可能正在运行的滚动动画（系统信息或之前的 AI 滚动）
    lv_anim_delete(chat_status_label_, nullptr);
    
    // 停止系统信息滚动，恢复 DataUpdateTask 更新
    SetShowingSystemInfo(false);
    
    // 设置文本内容
    lv_label_set_text(chat_status_label_, content);
    lv_label_set_long_mode(chat_status_label_, LV_LABEL_LONG_WRAP);
    
    // 先恢复居中对齐（正常模式），计算内容高度
    lv_obj_align(chat_status_label_, LV_ALIGN_LEFT_MID, 64 + 20, 0);
    
    // 检查内容是否超出父容器（chat_inner）的可见高度
    lv_obj_update_layout(chat_status_label_);
    int label_h = lv_obj_get_height(chat_status_label_);
    // 从父容器动态获取高度，不硬编码（父容器是 chat_inner）
    lv_obj_t *parent = lv_obj_get_parent(chat_status_label_);
    int visible_h = parent ? lv_obj_get_content_height(parent) : 108;
    
    if (label_h > visible_h) {
        // 超长内容：切换到 TOP_LEFT 绝对定位后启用滚动
        // （和鱼咬尾同理，LEFT_MID 对齐会干扰动画的 set_y）
        const int text_x = 64 + 20;
        lv_obj_align(chat_status_label_, LV_ALIGN_TOP_LEFT, text_x, 0);
        
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, chat_status_label_);
        lv_anim_set_values(&a, 0, -(label_h - visible_h));  // 从顶部滚到底部
        lv_anim_set_delay(&a, 1500);  // 开始前停顿 1.5 秒
        lv_anim_set_duration(&a, (label_h - visible_h) * 50);  // 速度：每像素 50ms
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 2000);  // 滚完后暂停 2 秒再重新开始
        lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
            lv_obj_set_y((lv_obj_t *)obj, v);
        });
        lv_anim_start(&a);
        
        ESP_LOGI("CustomLcdDisplay", "AI 回答过长（%dpx > %dpx），启用慢速滚动", label_h, visible_h);
    }

    // 音乐页同步显示 AI 文案
    if (music_chat_status_label_) {
        lv_label_set_long_mode(music_chat_status_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(music_chat_status_label_, content);
    }
    // 番茄钟页同步显示 AI 文案
    if (pomo_chat_status_label_) {
        lv_label_set_long_mode(pomo_chat_status_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(pomo_chat_status_label_, content);
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    
    // 1. 更新左侧文字（完整映射小智所有 21 种表情 + 额外状态）
    const char* text = "待命";
    if (strcmp(emotion, "neutral") == 0)         text = "待命";
    else if (strcmp(emotion, "happy") == 0)      text = "开心";
    else if (strcmp(emotion, "laughing") == 0)   text = "大笑";
    else if (strcmp(emotion, "funny") == 0)      text = "搞笑";
    else if (strcmp(emotion, "sad") == 0)        text = "难过";
    else if (strcmp(emotion, "angry") == 0)      text = "生气";
    else if (strcmp(emotion, "crying") == 0)     text = "哭泣";
    else if (strcmp(emotion, "loving") == 0)     text = "喜爱";
    else if (strcmp(emotion, "embarrassed") == 0) text = "害羞";
    else if (strcmp(emotion, "surprised") == 0)  text = "惊讶";
    else if (strcmp(emotion, "shocked") == 0)    text = "震惊";
    else if (strcmp(emotion, "thinking") == 0)   text = "思考";
    else if (strcmp(emotion, "winking") == 0)    text = "眨眼";
    else if (strcmp(emotion, "cool") == 0)       text = "耍酷";
    else if (strcmp(emotion, "relaxed") == 0)    text = "放松";
    else if (strcmp(emotion, "delicious") == 0)  text = "好吃";
    else if (strcmp(emotion, "kissy") == 0)      text = "亲亲";
    else if (strcmp(emotion, "confident") == 0)  text = "自信";
    else if (strcmp(emotion, "sleepy") == 0)     text = "犯困";
    else if (strcmp(emotion, "silly") == 0)      text = "调皮";
    else if (strcmp(emotion, "confused") == 0)   text = "困惑";
    // 额外状态
    else if (strcmp(emotion, "fear") == 0)       text = "害怕";
    else if (strcmp(emotion, "disgusted") == 0)  text = "嫌弃";
    else if (strcmp(emotion, "microchip_ai") == 0) text = "就绪";
    // 未知情绪也显示中文，不显示英文原文
    else                                         text = "待命";
    
    if (emotion_label_) {
        lv_label_set_text(emotion_label_, text);
    }
    if (music_emotion_label_) {
        lv_label_set_text(music_emotion_label_, text);
    }
    if (pomo_emotion_label_) {
        lv_label_set_text(pomo_emotion_label_, text);
    }
    
    // 2. 尝试加载小智自带的 emoji 图片（天气页 + 音乐页 + 番茄钟页同步更新）
    if (current_theme_) {
        auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
        auto image = emoji_collection ? emoji_collection->GetEmojiImage(emotion) : nullptr;
        bool has_image = (image && !image->IsGif());
        
        // 天气页 emoji
        if (emotion_img_) {
            if (has_image) {
                lv_image_set_src(emotion_img_, image->image_dsc());
                lv_obj_remove_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // 音乐页 emoji（同步显示相同的表情图片）
        if (music_emotion_img_) {
            if (has_image) {
                lv_image_set_src(music_emotion_img_, image->image_dsc());
                lv_obj_remove_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // 番茄钟页 emoji
        if (pomo_emotion_img_) {
            if (has_image) {
                lv_image_set_src(pomo_emotion_img_, image->image_dsc());
                lv_obj_remove_flag(pomo_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(pomo_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void CustomLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_status_label_) lv_label_set_text(chat_status_label_, "");
    if (music_chat_status_label_) lv_label_set_text(music_chat_status_label_, "");
    if (pomo_chat_status_label_) lv_label_set_text(pomo_chat_status_label_, "");
    // 表情不清除，保持常驻
}

// ===== 重写状态栏更新（禁用基类的 Font Awesome 文字更新）=====

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    // 不调用基类实现！
    // 基类会尝试用 lv_label_set_text 更新 network_label_ 和 battery_label_，
    // 但那些是隐藏的占位标签。我们自己的图片图标由 DataUpdateTask 管理。
}

// ===== 重写主题切换 =====

void CustomLcdDisplay::SetTheme(Theme* theme) {
    // RLCD 是 1-bit 单色屏，只有黑白两色，不需要主题切换。
    // 基类的 SetTheme 会操作 container_、content_、top_bar_ 等控件，
    // 我们的天气站 UI 没有创建这些，直接跳过避免崩溃。
    
    // 但需要保存 theme 指针，SetEmotion 需要用它来加载 emoji 图片
    current_theme_ = theme;
    ESP_LOGI(TAG, "RLCD 单色屏，跳过主题切换（已保存 theme 指针）");
}

void CustomLcdDisplay::ApplyDisplayMode() {
    // 先隐藏所有页面
    if (weather_page_) lv_obj_add_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
    if (calendar_page_) lv_obj_add_flag(calendar_page_, LV_OBJ_FLAG_HIDDEN);
    if (forecast_page_) lv_obj_add_flag(forecast_page_, LV_OBJ_FLAG_HIDDEN);
    if (quota_page_) lv_obj_add_flag(quota_page_, LV_OBJ_FLAG_HIDDEN);
    if (todo_page_) lv_obj_add_flag(todo_page_, LV_OBJ_FLAG_HIDDEN);

    // 显示当前页面
    switch (display_mode_) {
        case MODE_OVERVIEW:
            if (weather_page_) lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_CALENDAR:
            if (calendar_page_) lv_obj_remove_flag(calendar_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_FORECAST:
            if (forecast_page_) lv_obj_remove_flag(forecast_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_QUOTA:
            if (quota_page_) lv_obj_remove_flag(quota_page_, LV_OBJ_FLAG_HIDDEN);
            quota_page_changed_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
            RenderQuotaPageInternal();
            break;
        case MODE_TODO:
            if (todo_page_) lv_obj_remove_flag(todo_page_, LV_OBJ_FLAG_HIDDEN);
            UpdateTodoPageInternal();
            break;
    }
}

void CustomLcdDisplay::CycleDisplayMode() {
    DisplayLockGuard lock(this);
    auto cards = QuotaManager::GetInstance().GetCards();
    const size_t quota_pages = std::max<size_t>(1, (cards.size() + 3) / 4);
    if (display_mode_ == MODE_QUOTA && quota_subpage_ + 1 < quota_pages) {
        quota_subpage_++;
        quota_page_changed_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
        RenderQuotaPageInternal();
        return;
    }

    std::vector<DisplayMode> enabled;
    auto settings = QuotaManager::GetInstance().GetPageSettings();
    std::sort(settings.begin(), settings.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
    for (const auto& page : settings) {
        if (!page.enabled) continue;
        if (page.id == "overview") enabled.push_back(MODE_OVERVIEW);
        else if (page.id == "calendar") enabled.push_back(MODE_CALENDAR);
        else if (page.id == "forecast") enabled.push_back(MODE_FORECAST);
        else if (page.id == "quota") enabled.push_back(MODE_QUOTA);
        else if (page.id == "todo") enabled.push_back(MODE_TODO);
    }
    if (enabled.empty()) enabled.push_back(MODE_OVERVIEW);
    auto current = std::find(enabled.begin(), enabled.end(), display_mode_);
    display_mode_ = current == enabled.end() ? enabled.front() : enabled[(current - enabled.begin() + 1) % enabled.size()];
    if (display_mode_ == MODE_QUOTA) quota_subpage_ = 0;
    ApplyDisplayMode();
    const char* name = "未知";
    switch (display_mode_) {
        case MODE_OVERVIEW: name = "综合页"; break;
        case MODE_CALENDAR: name = "日历页"; break;
        case MODE_FORECAST: name = "天气页"; break;
        case MODE_QUOTA: name = "AI页"; break;
        case MODE_TODO: name = "待办页"; break;
    }
    ESP_LOGI(TAG, "页面切换: %s", name);
}

void CustomLcdDisplay::SetMusicInfo(const char* title, const char* artist) {
    DisplayLockGuard lock(this);
    if (music_title_label_ == nullptr || music_artist_label_ == nullptr) {
        return;
    }
    lv_label_set_text(music_title_label_, (title && strlen(title) > 0) ? title : "未知歌曲");
    lv_label_set_text(music_artist_label_, (artist && strlen(artist) > 0) ? artist : "未知歌手");
}

void CustomLcdDisplay::SetMusicLyric(const char* lyric) {
    DisplayLockGuard lock(this);
    if (music_lyric_label_ == nullptr) {
        return;
    }

    // 歌词格式："上一句\n当前句\n下一句"（由 application.cc 拼接）
    // 如果没有 \n 分隔符，说明是单行文本（如错误提示），直接显示在当前行
    std::string text(lyric ? lyric : "");
    std::string prev_line, curr_line, next_line;

    size_t first_nl = text.find('\n');
    if (first_nl != std::string::npos) {
        prev_line = text.substr(0, first_nl);
        size_t second_nl = text.find('\n', first_nl + 1);
        if (second_nl != std::string::npos) {
            curr_line = text.substr(first_nl + 1, second_nl - first_nl - 1);
            next_line = text.substr(second_nl + 1);
        } else {
            curr_line = text.substr(first_nl + 1);
        }
    } else {
        // 单行文本（错误提示等），只显示在当前行
        curr_line = text;
    }

    // 更新三个 label
    if (music_lyric_prev_label_) {
        lv_label_set_text(music_lyric_prev_label_, prev_line.c_str());
    }
    lv_label_set_text(music_lyric_label_, curr_line.c_str());
    if (music_lyric_next_label_) {
        lv_label_set_text(music_lyric_next_label_, next_line.c_str());
    }
}

void CustomLcdDisplay::SetMusicProgress(uint32_t current_ms, uint32_t total_ms) {
    DisplayLockGuard lock(this);
    if (music_progress_bar_ == nullptr || music_progress_label_ == nullptr) {
        return;
    }

    if (total_ms > 0) {
        // 有总时长（来自歌词）：正常显示进度条和 "当前 / 总时长"
        if (current_ms > total_ms) {
            current_ms = total_ms;
        }
        lv_bar_set_range(music_progress_bar_, 0, static_cast<int32_t>(total_ms));
        lv_bar_set_value(music_progress_bar_, static_cast<int32_t>(current_ms), LV_ANIM_OFF);

        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu / %02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60),
                 static_cast<unsigned long>(total_ms / 60000),
                 static_cast<unsigned long>((total_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    } else {
        // 无总时长（没有歌词）：进度条不动，只显示已播放时间
        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    }
}

void CustomLcdDisplay::SwitchToMusicPage() {
    SwitchToWeatherPage();
}

void CustomLcdDisplay::SwitchToWeatherPage() {
    DisplayLockGuard lock(this);
    if (display_mode_ != MODE_OVERVIEW) {
        display_mode_ = MODE_OVERVIEW;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到综合页");
    }
}

// ===== 番茄钟页面方法 =====

void CustomLcdDisplay::SwitchToPomodoroPage() {
    SwitchToWeatherPage();
}

void CustomLcdDisplay::SwitchToCalendarPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_CALENDAR;
    ApplyDisplayMode();
}

void CustomLcdDisplay::SwitchToForecastPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_FORECAST;
    ApplyDisplayMode();
}

void CustomLcdDisplay::SwitchToQuotaPage() {
    DisplayLockGuard lock(this);
    display_mode_ = MODE_QUOTA;
    quota_subpage_ = 0;
    ApplyDisplayMode();
}

void CustomLcdDisplay::TickQuotaPage() {
    auto& manager = QuotaManager::GetInstance();
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool needs_render = quota_rendered_revision_ != manager.GetRevision();
    auto settings = manager.GetPageSettings();
    auto mode_for_id = [](const std::string& id) {
        if (id == "calendar") return MODE_CALENDAR;
        if (id == "forecast") return MODE_FORECAST;
        if (id == "quota") return MODE_QUOTA;
        if (id == "todo") return MODE_TODO;
        return MODE_OVERVIEW;
    };
    bool current_enabled = false;
    for (const auto& page : settings) {
        if (page.enabled && mode_for_id(page.id) == display_mode_) current_enabled = true;
    }
    if (!current_enabled) {
        std::sort(settings.begin(), settings.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
        for (const auto& page : settings) {
            if (!page.enabled) continue;
            DisplayLockGuard lock(this);
            display_mode_ = mode_for_id(page.id);
            quota_subpage_ = 0;
            ApplyDisplayMode();
            return;
        }
    }
    if (display_mode_ == MODE_QUOTA && now - quota_page_changed_ms_ >= 10000) {
        auto cards = manager.GetCards();
        const size_t pages = std::max<size_t>(1, (cards.size() + 3) / 4);
        quota_subpage_ = (quota_subpage_ + 1) % pages;
        quota_page_changed_ms_ = now;
        needs_render = true;
    }
    if (needs_render) {
        DisplayLockGuard lock(this);
        RenderQuotaPageInternal();
    }
}

void CustomLcdDisplay::UpdatePomodoroDisplay(const char* state_text, const char* countdown_text,
                                              int progress_permille, const char* info_text) {
    DisplayLockGuard lock(this);
    if (pomo_state_label_ && state_text) {
        lv_label_set_text(pomo_state_label_, state_text);
    }
    if (pomo_countdown_label_ && countdown_text) {
        lv_label_set_text(pomo_countdown_label_, countdown_text);
    }
    if (pomo_progress_bar_) {
        lv_bar_set_value(pomo_progress_bar_, progress_permille, LV_ANIM_OFF);
    }
    if (pomo_info_label_ && info_text) {
        lv_label_set_text(pomo_info_label_, info_text);
    }
}

// ===== 省电模式 =====

void CustomLcdDisplay::NotifyUserActivity() {
    last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (power_saving_) {
        power_saving_ = false;
        ESP_LOGI(TAG, "用户活动检测到，退出省电模式");
    }
}

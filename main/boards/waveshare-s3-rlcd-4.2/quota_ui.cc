#include "custom_lcd_display.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "managers/manager_safety.h"
#include "managers/quota_manager.h"
#include "wifi_manager.h"

LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);
LV_IMAGE_DECLARE(ui_img_quota_codex);
LV_IMAGE_DECLARE(ui_img_quota_kimi);
LV_IMAGE_DECLARE(ui_img_quota_glm);
LV_IMAGE_DECLARE(ui_img_quota_deepseek);

namespace {
// 子对象坐标以卡片 6px 顶部内边距为原点，以下值已扣除该偏移。
constexpr int QUOTA_COMPACT_VALUE_Y = 16;
constexpr int QUOTA_COMPACT_PRIMARY_Y = 68;
constexpr int QUOTA_COMPACT_BAR_Y = 86;
constexpr int QUOTA_COMPACT_SECONDARY_Y = 93;

void StyleText(lv_obj_t* label, const lv_font_t* font, lv_color_t color,
               lv_opa_t opa = LV_OPA_COVER) {
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, opa, 0);
}

int RemainingPercent(const QuotaTier& tier) {
    if (tier.used_percent < 0) return -1;
    return std::clamp(100 - tier.used_percent, 0, 100);
}

void FormatReset(int64_t reset_at, char* out, size_t size) {
    if (reset_at <= 0) {
        out[0] = '\0';
        return;
    }
    int64_t seconds = reset_at - time(nullptr);
    if (seconds <= 0) {
        snprintf(out, size, "即将重置");
    } else if (seconds >= 86400) {
        const int days = static_cast<int>(seconds / 86400);
        const int hours = static_cast<int>((seconds % 86400) / 3600);
        if (hours > 0) snprintf(out, size, "%d天%d小时后重置", days, hours);
        else snprintf(out, size, "%d天后重置", days);
    } else if (seconds >= 3600) {
        const int hours = static_cast<int>(seconds / 3600);
        const int minutes = static_cast<int>((seconds % 3600) / 60);
        if (minutes > 0) snprintf(out, size, "%d小时%d分钟后重置", hours, minutes);
        else snprintf(out, size, "%d小时后重置", hours);
    } else {
        snprintf(out, size, "%d分钟后重置",
                 static_cast<int>(std::max<int64_t>(1, seconds / 60)));
    }
}

const lv_image_dsc_t* ProviderLogo(const std::string& provider) {
    if (provider == "codex") return &ui_img_quota_codex;
    if (provider == "kimi") return &ui_img_quota_kimi;
    if (provider == "glm-cn" || provider == "glm-global") return &ui_img_quota_glm;
    if (provider == "deepseek") return &ui_img_quota_deepseek;
    return nullptr;
}

const char* ProviderFallback(const std::string& provider) {
    if (provider == "generic-json") return "JSON";
    if (provider == "manual") return "M";
    return "AI";
}

size_t PrimaryTier(const QuotaCard& card) {
    size_t selected = 0;
    int lowest_remaining = 101;
    for (size_t i = 0; i < card.tiers.size(); ++i) {
        int remaining = RemainingPercent(card.tiers[i]);
        if (remaining >= 0 && remaining < lowest_remaining) {
            selected = i;
            lowest_remaining = remaining;
        }
    }
    return selected;
}
}  // namespace

void CustomLcdDisplay::SetupQuotaUI() {
    DisplayLockGuard lock(this);
    lv_obj_t* root = lv_screen_active();
    quota_page_ = lv_obj_create(root);
    lv_obj_set_size(quota_page_, 400, 300);
    lv_obj_set_pos(quota_page_, 0, 0);
    lv_obj_set_style_bg_color(quota_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(quota_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(quota_page_, 0, 0);
    lv_obj_set_style_pad_all(quota_page_, 0, 0);
    lv_obj_set_style_radius(quota_page_, 0, 0);
    lv_obj_remove_flag(quota_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(quota_page_, LV_OBJ_FLAG_HIDDEN);

    quota_header_label_ = lv_label_create(quota_page_);
    StyleText(quota_header_label_, &alibaba_puhui_24, lv_color_white());
    lv_obj_set_pos(quota_header_label_, 8, 10);
    lv_label_set_text(quota_header_label_, "AI");

    quota_admin_label_ = lv_label_create(quota_page_);
    StyleText(quota_admin_label_, &font_puhui_14_1, lv_color_white(), LV_OPA_80);
    lv_obj_set_pos(quota_admin_label_, 82, 3);
    lv_obj_set_width(quota_admin_label_, 308);
    lv_obj_set_style_text_align(quota_admin_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(quota_admin_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(quota_admin_label_, "管理后台 · 等待网络");

    quota_refresh_label_ = lv_label_create(quota_page_);
    StyleText(quota_refresh_label_, &font_puhui_14_1, lv_color_white(), LV_OPA_80);
    lv_obj_set_pos(quota_refresh_label_, 240, 25);
    lv_obj_set_width(quota_refresh_label_, 150);
    lv_obj_set_style_text_align(quota_refresh_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(quota_refresh_label_, "等待刷新");

    for (size_t i = 0; i < 4; ++i) {
        const int col = i % 2, row = i / 2;
        // 4 张卡片统一白底黑字（原来上下排交替黑白，导致背景不统一）。
        const lv_color_t foreground = lv_color_black();
        const lv_color_t background = lv_color_white();

        auto* card = lv_obj_create(quota_page_); quota_cards_[i] = card;
        lv_obj_set_size(card, 191, 118);
        lv_obj_set_pos(card, 6 + col * 197, 54 + row * 122);
        lv_obj_set_style_bg_color(card, background, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 0, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        // 底部的重置时间使用显式坐标，不能再被卡片默认的 6px 内边距裁切。
        lv_obj_set_style_pad_bottom(card, 0, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        quota_logo_images_[i] = lv_image_create(card);
        lv_obj_set_size(quota_logo_images_[i], 22, 22);
        lv_obj_set_pos(quota_logo_images_[i], 0, 0);
        lv_image_set_src(quota_logo_images_[i], &ui_img_quota_codex);

        quota_logo_fallback_labels_[i] = lv_label_create(card);
        StyleText(quota_logo_fallback_labels_[i], &font_puhui_14_1, foreground);
        lv_obj_set_size(quota_logo_fallback_labels_[i], 26, 22);
        lv_obj_set_pos(quota_logo_fallback_labels_[i], 0, -1);
        lv_obj_set_style_text_align(quota_logo_fallback_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(quota_logo_fallback_labels_[i], "AI");
        lv_obj_add_flag(quota_logo_fallback_labels_[i], LV_OBJ_FLAG_HIDDEN);

        quota_name_labels_[i] = lv_label_create(card);
        StyleText(quota_name_labels_[i], &font_puhui_16_4, foreground);
        lv_obj_set_pos(quota_name_labels_[i], 28, 0);
        lv_obj_set_width(quota_name_labels_[i], 149);
        lv_label_set_long_mode(quota_name_labels_[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(quota_name_labels_[i], "--");

        // Reuse the second slot in the bar array for the dominant numeric label;
        // this keeps CustomLcdDisplay's object layout compatible with existing code.
        quota_bars_[i][1] = lv_label_create(card);
        StyleText(quota_bars_[i][1], &alibaba_puhui_48, foreground);
        lv_obj_set_pos(quota_bars_[i][1], 2, QUOTA_COMPACT_VALUE_Y);
        lv_obj_set_width(quota_bars_[i][1], 172);
        lv_label_set_text(quota_bars_[i][1], "--");

        quota_tier_labels_[i][0] = lv_label_create(card);
        StyleText(quota_tier_labels_[i][0], &font_puhui_14_1, foreground);
        lv_obj_set_pos(quota_tier_labels_[i][0], 4, QUOTA_COMPACT_PRIMARY_Y);
        lv_obj_set_width(quota_tier_labels_[i][0], 170);
        lv_label_set_long_mode(quota_tier_labels_[i][0], LV_LABEL_LONG_DOT);
        lv_label_set_text(quota_tier_labels_[i][0], "等待刷新");

        quota_bars_[i][0] = lv_bar_create(card);
        lv_obj_set_size(quota_bars_[i][0], 172, 6);
        lv_obj_set_pos(quota_bars_[i][0], 3, QUOTA_COMPACT_BAR_Y);
        lv_bar_set_range(quota_bars_[i][0], 0, 100);
        lv_bar_set_value(quota_bars_[i][0], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(quota_bars_[i][0], foreground, 0);
        lv_obj_set_style_bg_opa(quota_bars_[i][0], LV_OPA_30, 0);
        lv_obj_set_style_border_color(quota_bars_[i][0], foreground, 0);
        lv_obj_set_style_border_width(quota_bars_[i][0], 1, 0);
        lv_obj_set_style_radius(quota_bars_[i][0], 0, 0);
        lv_obj_set_style_bg_color(quota_bars_[i][0], foreground, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(quota_bars_[i][0], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(quota_bars_[i][0], 0, LV_PART_INDICATOR);

        quota_tier_labels_[i][1] = lv_label_create(card);
        StyleText(quota_tier_labels_[i][1], &font_puhui_14_1, foreground, LV_OPA_80);
        lv_obj_set_pos(quota_tier_labels_[i][1], 4, QUOTA_COMPACT_SECONDARY_Y);
        lv_obj_set_width(quota_tier_labels_[i][1], 170);
        lv_label_set_long_mode(quota_tier_labels_[i][1], LV_LABEL_LONG_DOT);
        lv_label_set_text(quota_tier_labels_[i][1], "--");

    }

    quota_empty_label_ = lv_label_create(quota_page_);
    StyleText(quota_empty_label_, &font_puhui_16_4, lv_color_white(), LV_OPA_80);
    lv_obj_set_width(quota_empty_label_, 350);
    lv_obj_set_style_text_align(quota_empty_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(quota_empty_label_, "暂无额度项\n请打开上方地址添加");
    lv_obj_center(quota_empty_label_);
    lv_obj_add_flag(quota_empty_label_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::RenderQuotaPageInternal() {
    auto& manager = QuotaManager::GetInstance();
    auto cards = manager.GetCards();
    const size_t pages = std::max<size_t>(1, (cards.size() + 3) / 4);
    if (quota_subpage_ >= pages) quota_subpage_ = 0;

    char text[128];
    lv_label_set_text(quota_header_label_, "AI");
    const std::string admin_address =
        rlcd::FormatAdminAddress(WifiManager::GetInstance().GetIpAddress());
    lv_label_set_text(quota_admin_label_, admin_address.c_str());

    time_t refreshed = manager.GetLastAllSuccessAt();
    if (refreshed > 1700000000) {
        const int64_t age = std::max<int64_t>(0, time(nullptr) - refreshed);
        if (manager.IsRefreshing()) {
            snprintf(text, sizeof(text), "正在刷新");
        } else if (age < 60) {
            snprintf(text, sizeof(text), "刚刚更新");
        } else if (age < 3600) {
            snprintf(text, sizeof(text), "%d分钟前更新", static_cast<int>(age / 60));
        } else {
            struct tm info; localtime_r(&refreshed, &info);
            snprintf(text, sizeof(text), "%02d:%02d 更新", info.tm_hour, info.tm_min);
        }
    } else {
        snprintf(text, sizeof(text), manager.IsRefreshing() ? "正在刷新" : "等待刷新");
    }
    lv_label_set_text(quota_refresh_label_, text);

    const bool empty = cards.empty();
    if (empty) lv_obj_remove_flag(quota_empty_label_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(quota_empty_label_, LV_OBJ_FLAG_HIDDEN);

    const size_t page_start = quota_subpage_ * 4;
    const size_t visible_count = empty ? 0 : std::min<size_t>(4, cards.size() - page_start);
    for (size_t slot = 0; slot < visible_count; ++slot) {
        int x = 6, y = 54, width = 388, height = 240;
        if (visible_count == 2) {
            width = 191;
            x = 6 + static_cast<int>(slot) * 197;
        } else if (visible_count == 3) {
            width = 191;
            if (slot == 0) {
                x = 6;
            } else {
                x = 203;
                y = slot == 1 ? 54 : 176;
                height = 118;
            }
        } else if (visible_count == 4) {
            width = 191;
            height = 118;
            x = 6 + static_cast<int>(slot % 2) * 197;
            y = 54 + static_cast<int>(slot / 2) * 122;
        }
        lv_obj_set_size(quota_cards_[slot], width, height);
        lv_obj_set_pos(quota_cards_[slot], x, y);
        lv_obj_set_width(quota_name_labels_[slot], width - 44);
        lv_obj_set_width(quota_bars_[slot][1], width - 19);
        lv_obj_set_pos(quota_bars_[slot][1], 2,
                       height == 118 ? QUOTA_COMPACT_VALUE_Y : (height - 48) / 2 - 6);
        lv_obj_set_width(quota_tier_labels_[slot][0], width - 21);
        lv_obj_set_pos(quota_tier_labels_[slot][0], 4,
                       height == 118 ? QUOTA_COMPACT_PRIMARY_Y : height - 52);
        lv_obj_set_size(quota_bars_[slot][0], width - 19, height == 118 ? 6 : 7);
        lv_obj_set_pos(quota_bars_[slot][0], 3,
                       height == 118 ? QUOTA_COMPACT_BAR_Y : height - 35);
        lv_obj_set_width(quota_tier_labels_[slot][1], width - 21);
        lv_obj_set_pos(quota_tier_labels_[slot][1], 4,
                       height == 118 ? QUOTA_COMPACT_SECONDARY_Y : height - 25);
    }

    for (size_t slot = 0; slot < 4; ++slot) {
        const size_t index = page_start + slot;
        if (empty || index >= cards.size()) {
            lv_obj_add_flag(quota_cards_[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(quota_cards_[slot], LV_OBJ_FLAG_HIDDEN);
        const auto& card = cards[index];
        const lv_image_dsc_t* logo = ProviderLogo(card.provider);
        if (logo) {
            lv_image_set_src(quota_logo_images_[slot], logo);
            lv_obj_remove_flag(quota_logo_images_[slot], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(quota_logo_fallback_labels_[slot], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(quota_logo_fallback_labels_[slot], ProviderFallback(card.provider));
            lv_obj_add_flag(quota_logo_images_[slot], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(quota_logo_fallback_labels_[slot], LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(text, sizeof(text), "%s%s", card.name.c_str(),
                 (card.stale || !card.error.empty()) ? "  !" : "");
        lv_label_set_text(quota_name_labels_[slot], text);

        if (card.tiers.empty()) {
            lv_label_set_text(quota_bars_[slot][1], "--");
            lv_label_set_text(quota_tier_labels_[slot][0],
                              card.error.empty() ? "等待刷新" : card.error.c_str());
            lv_label_set_text(quota_tier_labels_[slot][1], card.enabled ? "" : "已停用");
            lv_obj_add_flag(quota_bars_[slot][0], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const size_t primary_index = PrimaryTier(card);
        const auto& primary = card.tiers[primary_index];
        const int remaining = RemainingPercent(primary);
        if (remaining >= 0) {
            snprintf(text, sizeof(text), "%d%%", remaining);
            lv_label_set_text(quota_bars_[slot][1], text);
            snprintf(text, sizeof(text), "%s 剩余", primary.label.c_str());
            lv_label_set_text(quota_tier_labels_[slot][0], text);
            lv_obj_remove_flag(quota_bars_[slot][0], LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(quota_bars_[slot][0], remaining, LV_ANIM_OFF);
        } else {
            snprintf(text, sizeof(text), "%.0f", primary.remaining);
            lv_label_set_text(quota_bars_[slot][1], text);
            snprintf(text, sizeof(text), "%s 剩余 %s", primary.label.c_str(), primary.unit.c_str());
            lv_label_set_text(quota_tier_labels_[slot][0], text);
            lv_obj_add_flag(quota_bars_[slot][0], LV_OBJ_FLAG_HIDDEN);
        }

        char reset[64];
        FormatReset(primary.reset_at, reset, sizeof(reset));
        if (card.tiers.size() > 1) {
            const size_t other_index = primary_index == 0 ? 1 : 0;
            const auto& other = card.tiers[other_index];
            const int other_remaining = RemainingPercent(other);
            if (other_remaining >= 0) {
                snprintf(text, sizeof(text), "%s %d%% · %s", other.label.c_str(),
                         other_remaining, reset);
            } else {
                snprintf(text, sizeof(text), "%s", reset);
            }
        } else {
            snprintf(text, sizeof(text), "%s", reset);
        }
        lv_label_set_text(quota_tier_labels_[slot][1], text);
    }
    quota_rendered_revision_ = manager.GetRevision();
}

// Штурман: лицо робота — светящиеся глаза вместо стандартных смайликов XiaoZhi.
//
// Поверх обычного интерфейса XiaoZhi (строка статуса сверху, субтитры снизу)
// рисуется прозрачный слой с двумя глазами. Эмоция, которую присылает ИИ,
// меняет форму, цвет и наклон глаз. Глаза сами моргают раз в 3–6 секунд.

#pragma once

#include <esp_random.h>
#include <lvgl.h>
#include <cstring>

#include "display/lcd_display.h"
#include "lvgl_theme.h"

class ShturmanFaceDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        auto dark = LvglThemeManager::GetInstance().GetTheme("dark");
        if (dark != nullptr) {
            SetTheme(dark);
        }

        DisplayLockGuard lock(this);
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }

        face_ = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(face_);
        lv_obj_set_size(face_, LV_HOR_RES, LV_VER_RES);
        lv_obj_center(face_);
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < 2; i++) {
            eye_[i] = lv_obj_create(face_);
            lv_obj_remove_style_all(eye_[i]);
            lv_obj_set_style_bg_opa(eye_[i], LV_OPA_COVER, 0);
            lv_obj_remove_flag(eye_[i], LV_OBJ_FLAG_SCROLLABLE);

            arc_[i] = lv_arc_create(face_);
            lv_obj_remove_style(arc_[i], nullptr, LV_PART_KNOB);
            lv_obj_remove_flag(arc_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_opa(arc_[i], LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(arc_[i], 14, LV_PART_MAIN);
            lv_obj_set_style_arc_rounded(arc_[i], true, LV_PART_MAIN);
            lv_obj_set_size(arc_[i], 76, 76);
            lv_arc_set_bg_angles(arc_[i], 205, 335);

            cheek_[i] = lv_obj_create(face_);
            lv_obj_remove_style_all(cheek_[i]);
            lv_obj_set_size(cheek_[i], 34, 18);
            lv_obj_set_style_radius(cheek_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(cheek_[i], lv_color_hex(0xFF6FA3), 0);
            lv_obj_set_style_bg_opa(cheek_[i], LV_OPA_60, 0);
            lv_obj_align(cheek_[i], LV_ALIGN_CENTER, i == 0 ? -100 : 100, 48);
        }

        ApplyEmotion("neutral");
        blink_timer_ = lv_timer_create(BlinkTick, 60, this);
    }

    bool SetEmotion(const char* emotion) override {
        if (face_ == nullptr) {
            return false;
        }
        DisplayLockGuard lock(this);
        ApplyEmotion(emotion != nullptr ? emotion : "neutral");
        return true;
    }

private:
    enum Style { kPill, kRound, kHappy, kLine, kWink, kGlasses };

    struct Shape {
        Style style;
        int w, h;          // размер глаза
        int dx, dy;        // смещение взгляда
        int tilt;          // наклон левого глаза в десятых долях градуса, по часовой (правый — зеркально)
        uint32_t color;
        bool cheeks;
        bool small_left;   // левый глаз меньше (задумчивость)
    };

    static constexpr int kEyeGap = 62;  // расстояние от центра до глаза
    static constexpr int kEyeY = -18;   // глаза чуть выше центра экрана

    lv_obj_t* face_ = nullptr;
    lv_obj_t* eye_[2] = {nullptr, nullptr};
    lv_obj_t* arc_[2] = {nullptr, nullptr};
    lv_obj_t* cheek_[2] = {nullptr, nullptr};
    lv_timer_t* blink_timer_ = nullptr;
    Shape shape_{};
    uint32_t next_blink_ms_ = 3000;
    uint32_t blink_end_ms_ = 0;

    static Shape ShapeFor(const char* e) {
        auto is = [e](const char* name) { return strcmp(e, name) == 0; };
        const uint32_t cyan = 0x5BE3F0, pink = 0xFF6FA3, amber = 0xFFB547, red = 0xFF5A5A,
                       blue = 0x7FB2FF;
        if (is("happy") || is("laughing") || is("funny") || is("delicious") || is("silly"))
            return {kHappy, 76, 76, 0, 0, 0, cyan, false, false};
        if (is("loving") || is("kissy"))
            return {kHappy, 76, 76, 0, 0, 0, pink, true, false};
        if (is("embarrassed"))
            return {kPill, 50, 78, 0, 8, 0, cyan, true, false};
        if (is("sad") || is("crying"))
            return {kGlasses, 64, 40, 0, 16, -140, blue, false, false};
        if (is("angry"))
            return {kGlasses, 74, 34, 0, 4, 160, red, false, false};
        if (is("surprised") || is("shocked"))
            return {kRound, 82, 82, 0, -6, 0, cyan, false, false};
        if (is("thinking") || is("confused"))
            return {kPill, 50, 80, 18, -14, 0, cyan, false, true};
        if (is("sleepy"))
            return {kLine, 64, 10, 0, 18, 0, cyan, false, false};
        if (is("winking"))
            return {kWink, 56, 96, 0, 0, 0, cyan, false, false};
        if (is("cool") || is("confident"))
            return {kGlasses, 96, 44, 0, 0, 0, amber, false, false};
        // neutral, relaxed и всё неизвестное
        return {kPill, 56, 96, 0, 0, 0, cyan, false, false};
    }

    void ApplyEmotion(const char* emotion) {
        shape_ = ShapeFor(emotion);
        DrawEyes(false);
    }

    void DrawEyes(bool blinking) {
        const Shape& s = shape_;
        lv_color_t color = lv_color_hex(s.color);
        for (int i = 0; i < 2; i++) {
            int side = i == 0 ? -1 : 1;
            int x = side * kEyeGap + s.dx;
            int y = kEyeY + s.dy;
            bool happy = s.style == kHappy;

            lv_obj_set_flag(arc_[i], LV_OBJ_FLAG_HIDDEN, !happy);
            lv_obj_set_flag(eye_[i], LV_OBJ_FLAG_HIDDEN, happy);
            lv_obj_set_flag(cheek_[i], LV_OBJ_FLAG_HIDDEN, !s.cheeks);

            if (happy) {
                lv_obj_set_style_arc_color(arc_[i], color, LV_PART_MAIN);
                lv_obj_align(arc_[i], LV_ALIGN_CENTER, x, y + 20);
                continue;
            }

            int w = s.w, h = s.h, radius = LV_RADIUS_CIRCLE;
            if (s.style == kLine || blinking || (s.style == kWink && i == 1)) {
                h = 10;
            }
            if (s.style == kGlasses) {
                radius = 12;
            }
            if (s.small_left && i == 0) {
                w = w * 3 / 4;
                h = h * 3 / 4;
            }
            lv_obj_set_size(eye_[i], w, h);
            lv_obj_set_style_radius(eye_[i], radius, 0);
            lv_obj_set_style_bg_color(eye_[i], color, 0);
            lv_obj_set_style_shadow_color(eye_[i], color, 0);
            lv_obj_set_style_shadow_width(eye_[i], 24, 0);
            lv_obj_set_style_shadow_opa(eye_[i], LV_OPA_50, 0);
            lv_obj_set_style_transform_pivot_x(eye_[i], w / 2, 0);
            lv_obj_set_style_transform_pivot_y(eye_[i], h / 2, 0);
            lv_obj_set_style_transform_rotation(eye_[i], i == 0 ? s.tilt : -s.tilt, 0);
            lv_obj_align(eye_[i], LV_ALIGN_CENTER, x, y);
        }
    }

    // Моргание: раз в 3–6 с глаза на 120 мс превращаются в полоски.
    static void BlinkTick(lv_timer_t* timer) {
        auto self = static_cast<ShturmanFaceDisplay*>(lv_timer_get_user_data(timer));
        uint32_t now = lv_tick_get();
        Style st = self->shape_.style;
        bool can_blink = st == kPill || st == kRound;
        if (self->blink_end_ms_ != 0 && now >= self->blink_end_ms_) {
            self->blink_end_ms_ = 0;
            self->DrawEyes(false);
        } else if (can_blink && self->blink_end_ms_ == 0 && now >= self->next_blink_ms_) {
            self->blink_end_ms_ = now + 120;
            self->next_blink_ms_ = now + 3000 + esp_random() % 3000;
            self->DrawEyes(true);
        }
    }
};

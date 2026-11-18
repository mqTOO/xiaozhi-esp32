// Штурман: лицо робота — светящиеся глаза вместо стандартных смайликов XiaoZhi.
//
// Поверх обычного интерфейса XiaoZhi (строка статуса сверху, субтитры снизу)
// рисуется прозрачный слой с двумя глазами. Эмоция, которую присылает ИИ,
// задаёт целевую форму, цвет и наклон глаз, а таймер анимации каждые 16 мс
// плавно подтягивает текущую картинку к цели:
//   - смена эмоции перетекает за ~0,2 с (форма, цвет, наклон, румянец);
//   - переход в «улыбку-дугу» и обратно — через прищур: глаз схлопывается
//     в полоску, и из неё раскрывается новая форма;
//   - моргание — быстрое схлопывание и раскрытие, раз в 3–6 с;
//   - в спокойном состоянии взгляд изредка плавно смещается в сторону.

#pragma once

#include <esp_random.h>
#include <lvgl.h>
#include <cmath>
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
            cheek_[i] = lv_obj_create(face_);
            lv_obj_remove_style_all(cheek_[i]);
            lv_obj_set_size(cheek_[i], 34, 18);
            lv_obj_set_style_radius(cheek_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(cheek_[i], lv_color_hex(0xFF6FA3), 0);
            lv_obj_set_style_bg_opa(cheek_[i], LV_OPA_COVER, 0);
            lv_obj_align(cheek_[i], LV_ALIGN_CENTER, i == 0 ? -100 : 100, 48);

            eye_[i] = lv_obj_create(face_);
            lv_obj_remove_style_all(eye_[i]);
            lv_obj_set_style_bg_opa(eye_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(eye_[i], 24, 0);
            lv_obj_set_style_shadow_opa(eye_[i], LV_OPA_50, 0);
            lv_obj_remove_flag(eye_[i], LV_OBJ_FLAG_SCROLLABLE);

            arc_[i] = lv_arc_create(face_);
            lv_obj_remove_style(arc_[i], nullptr, LV_PART_KNOB);
            lv_obj_remove_flag(arc_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_opa(arc_[i], LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(arc_[i], 14, LV_PART_MAIN);
            lv_obj_set_style_arc_rounded(arc_[i], true, LV_PART_MAIN);
            lv_obj_set_size(arc_[i], kArcSize, kArcSize);
            lv_arc_set_bg_angles(arc_[i], 205, 335);
            lv_obj_set_style_transform_pivot_x(arc_[i], kArcSize / 2, 0);
            lv_obj_set_style_transform_pivot_y(arc_[i], kArcSize / 2, 0);
        }

        // Первый кадр — сразу в нейтральной позе, без анимации.
        shape_ = ShapeFor("neutral");
        for (int i = 0; i < 2; i++) {
            eye_cur_[i] = RectTarget(i, false);
            arc_cur_[i] = ArcTarget(i, false);
        }
        cheek_cur_ = 0.0f;
        shown_arc_ = false;
        last_tick_ = lv_tick_get();
        next_blink_ms_ = last_tick_ + 3000;
        next_gaze_ms_ = last_tick_ + 4000;
        Render(true);

        anim_timer_ = lv_timer_create(AnimTick, 16, this);
    }

    bool SetEmotion(const char* emotion) override {
        if (face_ == nullptr) {
            return false;
        }
        DisplayLockGuard lock(this);
        shape_ = ShapeFor(emotion != nullptr ? emotion : "neutral");
        if (!CanWander()) {
            gaze_x_ = gaze_y_ = 0.0f;
        }
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

    // Текущее или целевое состояние глаза-прямоугольника (все величины — float для плавности).
    struct EyePose {
        float x, y, w, h, tilt, radius;
        float r, g, b;
    };

    // Состояние глаза-дуги (улыбка).
    struct ArcPose {
        float x, y, scale, opa;
        float r, g, b;
    };

    static constexpr int kEyeGap = 62;     // расстояние от центра до глаза
    static constexpr int kEyeY = -18;      // глаза чуть выше центра экрана
    static constexpr int kArcSize = 76;
    static constexpr float kClosedH = 8.0f;  // высота «схлопнутого» глаза

    // Время сглаживания, мс: чем меньше — тем резче движение.
    static constexpr float kTauShape = 45.0f;
    static constexpr float kTauBlink = 18.0f;
    static constexpr float kTauGaze = 110.0f;

    lv_obj_t* face_ = nullptr;
    lv_obj_t* eye_[2] = {nullptr, nullptr};
    lv_obj_t* arc_[2] = {nullptr, nullptr};
    lv_obj_t* cheek_[2] = {nullptr, nullptr};
    lv_timer_t* anim_timer_ = nullptr;

    Shape shape_{};
    EyePose eye_cur_[2]{};
    ArcPose arc_cur_[2]{};
    float cheek_cur_ = 0.0f;
    bool shown_arc_ = false;  // что сейчас на экране: дуги или прямоугольники

    uint32_t last_tick_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_end_ms_ = 0;
    uint32_t fast_until_ms_ = 0;  // после моргания глаз раскрывается быстро
    uint32_t next_gaze_ms_ = 0;
    float gaze_x_ = 0.0f, gaze_y_ = 0.0f;          // целевое смещение взгляда
    float gaze_cur_x_ = 0.0f, gaze_cur_y_ = 0.0f;  // текущее

    // Последние значения, отправленные в LVGL (чтобы не перерисовывать без изменений).
    struct Applied {
        int x = -9999, y, w, h, tilt, radius, opa, scale;
        uint32_t color;
    };
    Applied eye_applied_[2];
    Applied arc_applied_[2];
    int cheek_applied_ = -1;
    bool arc_visible_applied_ = true;

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

    // Блуждать взглядом и моргать можно только в «обычных» глазах.
    bool CanWander() const {
        return (shape_.style == kPill || shape_.style == kRound) && !shape_.small_left;
    }

    static void SplitColor(uint32_t c, float* r, float* g, float* b) {
        *r = (c >> 16) & 0xFF;
        *g = (c >> 8) & 0xFF;
        *b = c & 0xFF;
    }

    EyePose RectTarget(int i, bool closed) const {
        const Shape& s = shape_;
        int side = i == 0 ? -1 : 1;
        EyePose p{};
        p.x = side * kEyeGap + s.dx + gaze_cur_x_;
        p.y = kEyeY + s.dy + gaze_cur_y_;
        p.w = s.w;
        p.h = s.h;
        if (s.style == kLine || (s.style == kWink && i == 1)) {
            p.h = 10;
        }
        if (s.small_left && i == 0) {
            p.w = s.w * 3 / 4;
            p.h = s.h * 3 / 4;
        }
        if (closed) {
            p.h = kClosedH;
        }
        p.tilt = i == 0 ? s.tilt : -s.tilt;
        p.radius = s.style == kGlasses ? 12.0f : std::fmin(p.w, p.h) / 2.0f;
        SplitColor(s.color, &p.r, &p.g, &p.b);
        return p;
    }

    ArcPose ArcTarget(int i, bool visible) const {
        int side = i == 0 ? -1 : 1;
        ArcPose p{};
        p.x = side * kEyeGap + shape_.dx;
        p.y = kEyeY + shape_.dy + 20;
        p.scale = visible ? 1.0f : 0.45f;
        p.opa = visible ? 1.0f : 0.0f;
        SplitColor(shape_.color, &p.r, &p.g, &p.b);
        return p;
    }

    static void Approach(float* v, float target, float a) { *v += (target - *v) * a; }

    static void AnimTick(lv_timer_t* timer) {
        static_cast<ShturmanFaceDisplay*>(lv_timer_get_user_data(timer))->Step();
    }

    void Step() {
        uint32_t now = lv_tick_get();
        float dt = static_cast<float>(lv_tick_diff(now, last_tick_));
        last_tick_ = now;
        if (dt > 100.0f) dt = 100.0f;
        if (dt <= 0.0f) return;

        bool want_arc = shape_.style == kHappy;
        bool switching = want_arc != shown_arc_;

        // Моргание.
        bool can_blink = CanWander() && !shown_arc_ && !switching;
        if (blink_end_ms_ != 0 && now >= blink_end_ms_) {
            blink_end_ms_ = 0;
            fast_until_ms_ = now + 150;
        } else if (can_blink && blink_end_ms_ == 0 && now >= next_blink_ms_) {
            blink_end_ms_ = now + 90;
            next_blink_ms_ = now + 3000 + esp_random() % 3000;
        }
        bool blinking = blink_end_ms_ != 0;

        // Блуждание взгляда: раз в 2,5–6 с новая точка, часто — снова в центр.
        if (now >= next_gaze_ms_) {
            next_gaze_ms_ = now + 2500 + esp_random() % 3500;
            if (CanWander() && esp_random() % 100 >= 40) {
                gaze_x_ = static_cast<float>(static_cast<int>(esp_random() % 33) - 16);
                gaze_y_ = static_cast<float>(static_cast<int>(esp_random() % 19) - 10);
            } else {
                gaze_x_ = gaze_y_ = 0.0f;
            }
        }
        float a_gaze = 1.0f - std::exp(-dt / kTauGaze);
        Approach(&gaze_cur_x_, gaze_x_, a_gaze);
        Approach(&gaze_cur_y_, gaze_y_, a_gaze);

        bool fast = blinking || now < fast_until_ms_;
        float a = 1.0f - std::exp(-dt / (fast ? kTauBlink : kTauShape));

        for (int i = 0; i < 2; i++) {
            // Прямоугольник: при переходе в дугу — схлопывается, иначе к своей цели.
            EyePose t = RectTarget(i, blinking || (switching && !shown_arc_));
            EyePose& c = eye_cur_[i];
            Approach(&c.x, t.x, a);
            Approach(&c.y, t.y, a);
            Approach(&c.w, t.w, a);
            Approach(&c.h, t.h, a);
            Approach(&c.tilt, t.tilt, a);
            Approach(&c.radius, t.radius, a);
            Approach(&c.r, t.r, a);
            Approach(&c.g, t.g, a);
            Approach(&c.b, t.b, a);

            // Дуга: при переходе в прямоугольник — сжимается и гаснет.
            ArcPose at = ArcTarget(i, want_arc && shown_arc_);
            ArcPose& ac = arc_cur_[i];
            Approach(&ac.x, at.x, a);
            Approach(&ac.y, at.y, a);
            Approach(&ac.scale, at.scale, a);
            Approach(&ac.opa, at.opa, a);
            Approach(&ac.r, at.r, a);
            Approach(&ac.g, at.g, a);
            Approach(&ac.b, at.b, a);
        }

        // Смена прямоугольник <-> дуга, когда старая форма почти исчезла.
        if (switching) {
            if (!shown_arc_ && eye_cur_[0].h < kClosedH + 3 && eye_cur_[1].h < kClosedH + 3) {
                shown_arc_ = true;
                for (int i = 0; i < 2; i++) arc_cur_[i] = ArcTarget(i, false);
            } else if (shown_arc_ && arc_cur_[0].opa < 0.08f && arc_cur_[1].opa < 0.08f) {
                shown_arc_ = false;
                for (int i = 0; i < 2; i++) {
                    eye_cur_[i] = RectTarget(i, false);
                    eye_cur_[i].h = kClosedH;  // новый глаз раскрывается из полоски
                }
            }
        }

        Approach(&cheek_cur_, shape_.cheeks ? 1.0f : 0.0f, a);
        Render(false);
    }

    static uint32_t PackColor(float r, float g, float b) {
        auto c = [](float v) { return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f)); };
        return (c(r) << 16) | (c(g) << 8) | c(b);
    }

    void Render(bool force) {
        for (int i = 0; i < 2; i++) {
            lv_obj_set_flag(eye_[i], LV_OBJ_FLAG_HIDDEN, shown_arc_);
            if (!shown_arc_) {
                const EyePose& c = eye_cur_[i];
                Applied n;
                n.x = lroundf(c.x);
                n.y = lroundf(c.y);
                n.w = lroundf(c.w);
                n.h = lroundf(c.h);
                n.tilt = lroundf(c.tilt);
                n.radius = lroundf(c.radius);
                n.color = PackColor(c.r, c.g, c.b);
                Applied& o = eye_applied_[i];
                if (force || n.w != o.w || n.h != o.h) {
                    lv_obj_set_size(eye_[i], n.w, n.h);
                    lv_obj_set_style_transform_pivot_x(eye_[i], n.w / 2, 0);
                    lv_obj_set_style_transform_pivot_y(eye_[i], n.h / 2, 0);
                }
                if (force || n.radius != o.radius) lv_obj_set_style_radius(eye_[i], n.radius, 0);
                if (force || n.tilt != o.tilt) lv_obj_set_style_transform_rotation(eye_[i], n.tilt, 0);
                if (force || n.color != o.color) {
                    lv_obj_set_style_bg_color(eye_[i], lv_color_hex(n.color), 0);
                    lv_obj_set_style_shadow_color(eye_[i], lv_color_hex(n.color), 0);
                }
                if (force || n.x != o.x || n.y != o.y || n.w != o.w || n.h != o.h) {
                    lv_obj_align(eye_[i], LV_ALIGN_CENTER, n.x, n.y);
                }
                o = n;
            } else {
                eye_applied_[i].x = -9999;  // при возврате применить всё заново
            }

            const ArcPose& ac = arc_cur_[i];
            if (force || shown_arc_ != arc_visible_applied_) {
                lv_obj_set_flag(arc_[i], LV_OBJ_FLAG_HIDDEN, !shown_arc_);
            }
            if (shown_arc_) {
                Applied n;
                n.x = lroundf(ac.x);
                n.y = lroundf(ac.y);
                n.scale = lroundf(ac.scale * 256.0f);
                n.opa = lroundf(ac.opa * 255.0f);
                n.color = PackColor(ac.r, ac.g, ac.b);
                Applied& o = arc_applied_[i];
                if (force || o.x == -9999 || n.x != o.x || n.y != o.y)
                    lv_obj_align(arc_[i], LV_ALIGN_CENTER, n.x, n.y);
                if (force || o.x == -9999 || n.scale != o.scale)
                    lv_obj_set_style_transform_scale(arc_[i], n.scale, 0);
                if (force || o.x == -9999 || n.opa != o.opa)
                    lv_obj_set_style_opa(arc_[i], n.opa, 0);
                if (force || o.x == -9999 || n.color != o.color)
                    lv_obj_set_style_arc_color(arc_[i], lv_color_hex(n.color), LV_PART_MAIN);
                o = n;
            } else {
                arc_applied_[i].x = -9999;
            }
        }
        arc_visible_applied_ = shown_arc_;

        int cheek = lroundf(cheek_cur_ * 153.0f);  // 60% непрозрачности
        if (force || cheek != cheek_applied_) {
            for (int i = 0; i < 2; i++) {
                lv_obj_set_flag(cheek_[i], LV_OBJ_FLAG_HIDDEN, cheek < 3);
                lv_obj_set_style_opa(cheek_[i], cheek, 0);
            }
            cheek_applied_ = cheek;
        }
    }
};

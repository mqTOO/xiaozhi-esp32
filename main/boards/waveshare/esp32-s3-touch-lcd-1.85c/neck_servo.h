// Штурман: шея робота на сервоприводе MG90S.
//
// Сигнал сервопривода выдаётся одновременно на GPIO44 (RX) и GPIO43 (TX)
// разъёма UART: в макете серво висит на одном из них, второй провод заизолирован.
// Консоль переведена на USB (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG), поэтому
// эти выводы свободны.
//
// Поведение по состоянию ассистента:
//   слушает   — смотрит прямо (на собеседника);
//   говорит   — плавно покачивает головой, как при разговоре;
//   ожидание  — изредка оглядывается и возвращается в центр;
//   прочее    — смотрит прямо.
// Голосовые команды (MCP) временно перехватывают управление на HOLD_MS.

#pragma once

#include <atomic>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

class NeckServo {
public:
    static constexpr gpio_num_t kPinA = GPIO_NUM_44;
    static constexpr gpio_num_t kPinB = GPIO_NUM_43;
    static constexpr int kLimitDeg = 60;      // механический предел шеи, ±градусы
    static constexpr int kPulseMinUs = 600;   // импульс при -90°
    static constexpr int kPulseMaxUs = 2400;  // импульс при +90°
    static constexpr bool kInvert = false;    // поменять, если «влево» крутит вправо
    static constexpr int64_t kHoldMs = 6000;  // сколько держать позу после голосовой команды

    void Start() {
        ledc_timer_config_t timer = {};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_14_BIT;
        timer.timer_num = LEDC_TIMER_1;  // таймер 0 занят подсветкой экрана
        timer.freq_hz = 50;
        timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer));

        ConfigChannel(kPinA, LEDC_CHANNEL_1);
        ConfigChannel(kPinB, LEDC_CHANNEL_2);
        WriteAngle(0.0f);

        xTaskCreate([](void* arg) { static_cast<NeckServo*>(arg)->Loop(); },
                    "neck", 4096, this, 3, nullptr);
    }

    // Повернуть голову: -60 = влево, 0 = прямо, +60 = вправо.
    void LookAt(int deg, int speed_deg_per_s = 120) {
        manual_target_ = Clamp(static_cast<float>(deg));
        manual_speed_ = speed_deg_per_s;
        manual_until_ms_ = NowMs() + kHoldMs;
        manual_gesture_ = kGestureNone;
    }

    // Жест «нет»: несколько быстрых покачиваний.
    void Shake() { StartGesture(kGestureShake); }

    // Осмотреться по сторонам.
    void LookAround() { StartGesture(kGestureLookAround); }

    int CurrentAngle() const { return static_cast<int>(current_); }

private:
    enum Gesture { kGestureNone, kGestureShake, kGestureLookAround };

    float current_ = 0.0f;
    std::atomic<float> manual_target_{0.0f};
    std::atomic<int> manual_speed_{120};
    std::atomic<int64_t> manual_until_ms_{0};
    std::atomic<int> manual_gesture_{kGestureNone};
    int64_t gesture_start_ms_ = 0;
    int64_t next_glance_ms_ = 0;
    int64_t glance_until_ms_ = 0;
    float glance_target_ = 0.0f;

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    static float Clamp(float deg) {
        if (deg > kLimitDeg) return kLimitDeg;
        if (deg < -kLimitDeg) return -kLimitDeg;
        return deg;
    }

    static void ConfigChannel(gpio_num_t pin, ledc_channel_t channel) {
        ledc_channel_config_t ch = {};
        ch.gpio_num = pin;
        ch.speed_mode = LEDC_LOW_SPEED_MODE;
        ch.channel = channel;
        ch.intr_type = LEDC_INTR_DISABLE;
        ch.timer_sel = LEDC_TIMER_1;
        ch.duty = 0;
        ch.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }

    void WriteAngle(float deg) {
        deg = Clamp(deg);
        float servo_deg = (kInvert ? -deg : deg) + 90.0f;  // 0..180
        float pulse_us = kPulseMinUs + (kPulseMaxUs - kPulseMinUs) * servo_deg / 180.0f;
        uint32_t duty = static_cast<uint32_t>(pulse_us * ((1 << 14) - 1) / 20000.0f);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
        current_ = deg;
    }

    void StartGesture(Gesture g) {
        gesture_start_ms_ = NowMs();
        manual_gesture_ = g;
        manual_until_ms_ = NowMs() + (g == kGestureShake ? 1800 : 5000);
    }

    // Целевой угол жеста в момент t (мс от начала).
    float GestureTarget(Gesture g, int64_t t) {
        if (g == kGestureShake) {
            float phase = (t % 450) / 450.0f;               // период 0,45 с
            return 20.0f * (phase < 0.5f ? 1.0f : -1.0f);
        }
        if (g == kGestureLookAround) {
            if (t < 1500) return -50.0f;
            if (t < 3200) return 50.0f;
            return 0.0f;
        }
        return 0.0f;
    }

    // Целевой угол по состоянию ассистента.
    float StateTarget(int64_t now, int* speed) {
        auto state = Application::GetInstance().GetDeviceState();
        switch (state) {
            case kDeviceStateListening:
                *speed = 150;
                return 0.0f;
            case kDeviceStateSpeaking: {
                // мягкое покачивание ±10° с периодом ~3 с
                *speed = 25;
                float phase = (now % 3000) / 3000.0f;
                return phase < 0.5f ? 10.0f : -10.0f;
            }
            case kDeviceStateIdle: {
                *speed = 40;
                if (now >= next_glance_ms_) {
                    glance_target_ = static_cast<float>(
                        static_cast<int>(esp_random() % 61) - 30);  // -30..30
                    glance_until_ms_ = now + 2500;
                    next_glance_ms_ = now + 20000 + esp_random() % 25000;  // каждые 20–45 с
                }
                return now < glance_until_ms_ ? glance_target_ : 0.0f;
            }
            default:
                *speed = 90;
                return 0.0f;
        }
    }

    void Loop() {
        vTaskDelay(pdMS_TO_TICKS(3000));  // дать ассистенту запуститься
        next_glance_ms_ = NowMs() + 15000;
        const int kTickMs = 20;
        while (true) {
            int64_t now = NowMs();
            float target;
            int speed;
            if (now < manual_until_ms_) {
                Gesture g = static_cast<Gesture>(manual_gesture_.load());
                if (g != kGestureNone) {
                    target = GestureTarget(g, now - gesture_start_ms_);
                    speed = g == kGestureShake ? 300 : 90;
                } else {
                    target = manual_target_;
                    speed = manual_speed_;
                }
            } else {
                manual_gesture_ = kGestureNone;
                target = StateTarget(now, &speed);
            }

            float max_step = speed * kTickMs / 1000.0f;
            float diff = target - current_;
            if (diff > max_step) diff = max_step;
            if (diff < -max_step) diff = -max_step;
            if (diff > 0.05f || diff < -0.05f) {
                WriteAngle(current_ + diff);
            }
            vTaskDelay(pdMS_TO_TICKS(kTickMs));
        }
    }
};

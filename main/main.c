#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"

static const char *TAG = "demo";

#define UI_PERIOD_MS 50
#define GAME_RESULT_MS 3000
#define DIRECTION_STABLE_MS 80

#define DIRECTION_MASK ((1u << BTN_UP) | (1u << BTN_DOWN) | (1u << BTN_LEFT) | (1u << BTN_RIGHT))

typedef struct {
    const char *name;
    uint32_t mask;
} direction_t;

static const direction_t s_directions[] = {
    {"1", (1u << BTN_UP) | (1u << BTN_LEFT)},
    {"2", (1u << BTN_UP) | (1u << BTN_RIGHT)},
    {"3", (1u << BTN_DOWN) | (1u << BTN_RIGHT)},
    {"4", (1u << BTN_DOWN) | (1u << BTN_LEFT)},
};

static uint32_t next_direction(uint32_t current, uint32_t count)
{
    uint32_t next = esp_random() % (count - 1);
    if (next >= current) {
        next++;
    }
    return next;
}

static const int s_button_freq[BTN_COUNT] = {
    [BTN_UP] = 659,
    [BTN_DOWN] = 440,
    [BTN_LEFT] = 494,
    [BTN_RIGHT] = 587,
    [BTN_CENTER] = 784,
    [BTN_KEY1] = 262,
    [BTN_KEY2] = 330,
    [BTN_KEY3] = 392,
    [BTN_KEY4] = 523,
};

static void draw_key(int x, int y, int size, bool pressed)
{
    if (pressed) {
        display_fill_rect(x, y, size, size, true);
    } else {
        display_fill_rect(x, y, size, size, false);
        display_draw_rect(x, y, size, size);
    }
}

static void draw_ui(uint32_t pressed, int mic_level, bool audio_ok, const char *game_status)
{
    char line[24];

    display_clear();
    display_text(28, 1, "APURATE CAPO", true);

    display_text(0, 14, "LOUD", false);
    display_draw_rect(27, 13, 77, 10);
    display_fill_rect(29, 15, mic_level * 73 / 100, 6, true);
    if (audio_ok) {
        snprintf(line, sizeof(line), "%3d", mic_level);
        display_text(107, 14, line, false);
    } else {
        display_text(107, 14, "ERR", false);
    }

    draw_key(15, 28, 10, pressed & (1u << BTN_UP));
    draw_key(4, 39, 10, pressed & (1u << BTN_LEFT));
    draw_key(15, 39, 10, pressed & (1u << BTN_CENTER));
    draw_key(26, 39, 10, pressed & (1u << BTN_RIGHT));
    draw_key(15, 50, 10, pressed & (1u << BTN_DOWN));

    display_text(7, 29, "1", false);
    display_text(29, 29, "2", false);
    display_text(29, 52, "3", false);
    display_text(7, 52, "4", false);

    display_text(50, 29, game_status, false);
    for (int i = 0; i < 4; i++) {
        bool on = pressed & (1u << (BTN_KEY1 + i));
        int x = 50 + i * 15;
        draw_key(x, 40, 13, on);
        char num[2] = {"RGBS"[i], 0};
        display_text(x + 4, 43, num, on);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Hackathon board demo starting");

    bool display_ok = display_init() == ESP_OK;
    if (!display_ok) {
        ESP_LOGE(TAG, "display init failed - continuing without screen");
    }

    ESP_ERROR_CHECK(buttons_init());

    if (display_ok) {
        display_clear();
        display_text(10, 8, "Hello, Hackathon!", false);
        display_text(10, 24, "ESP32-S3 dev board", false);
        display_text(10, 40, "Self test...", false);
        display_flush();
    }

    bool audio_ok = audio_init() == ESP_OK;
    if (audio_ok) {
        const int notes[] = {523, 659, 784};
        for (int i = 0; i < 3; i++) {
            audio_play_tone(notes[i], 120);
        }
    } else {
        ESP_LOGE(TAG, "audio init failed - continuing without sound");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    QueueHandle_t events = buttons_event_queue();
    const uint32_t direction_count = (uint32_t)(sizeof(s_directions) / sizeof(s_directions[0]));
    uint32_t target = esp_random() % direction_count;
    bool showing_result = false;
    bool wait_for_neutral = true;
    bool answer_correct = false;
    uint32_t candidate = 0;
    TickType_t candidate_since = 0;
    TickType_t result_since = 0;
    int log_div = 0;
    while (1) {
        button_event_t ev;
        if (xQueueReceive(events, &ev, pdMS_TO_TICKS(UI_PERIOD_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "button %s %s", buttons_name(ev.id), ev.pressed ? "pressed" : "released");
            if (ev.pressed && audio_ok) {
                audio_play_tone(s_button_freq[ev.id], 100);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        const uint32_t direction = buttons_read() & DIRECTION_MASK;
        if (showing_result) {
            if (direction == 0) {
                wait_for_neutral = false;
            }
            if (now - result_since >= pdMS_TO_TICKS(GAME_RESULT_MS)) {
                showing_result = false;
                target = next_direction(target, direction_count);
                candidate = 0;
                if (direction != 0) {
                    wait_for_neutral = true;
                }
            }
        } else if (wait_for_neutral) {
            if (direction == 0) {
                wait_for_neutral = false;
            }
        } else if (direction == 0) {
            candidate = 0;
        } else if (direction != candidate) {
            candidate = direction;
            candidate_since = now;
        } else if (now - candidate_since >= pdMS_TO_TICKS(DIRECTION_STABLE_MS)) {
            answer_correct = direction == s_directions[target].mask;
            showing_result = true;
            result_since = now;
            wait_for_neutral = true;
            candidate = 0;
        }

        int mic_level = audio_ok ? audio_mic_level() : 0;
        if (display_ok) {
            char game_status[16];
            if (showing_result) {
                snprintf(game_status, sizeof(game_status), "%s", answer_correct ? "RIGHT" : "WRONG");
            } else {
                snprintf(game_status, sizeof(game_status), "%s", s_directions[target].name);
            }
            draw_ui(buttons_read(), mic_level, audio_ok, game_status);
            display_flush();
        }

        if (audio_ok && ++log_div >= 1000 / UI_PERIOD_MS) {
            log_div = 0;
            ESP_LOGI(TAG, "mic level: %d", mic_level);
        }
    }
}

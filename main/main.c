#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"

static const char *TAG = "demo";

#define UI_PERIOD_MS 50
#define GAME_RESULT_MS 1000
#define GAME_TIMER_MS 10000
#define DIRECTION_STABLE_MS 80
#define SILENCE_LEVEL 90
#define SILENCE_SCREEN_MS 5000
#define MIC_REARM_DELAY_MS 500
#define MIC_STARTUP_DELAY_MS 2000

#define DIRECTION_MASK ((1u << BTN_UP) | (1u << BTN_DOWN) | (1u << BTN_LEFT) | (1u << BTN_RIGHT))
#define MODIFIER_MASK ((1u << BTN_KEY1) | (1u << BTN_KEY2) | (1u << BTN_KEY3))

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

static const uint32_t s_modifier_masks[] = {
    0,
    1u << BTN_KEY1,
    1u << BTN_KEY2,
    (1u << BTN_KEY1) | (1u << BTN_KEY2),
    1u << BTN_KEY3,
    (1u << BTN_KEY1) | (1u << BTN_KEY3),
    (1u << BTN_KEY2) | (1u << BTN_KEY3),
    (1u << BTN_KEY1) | (1u << BTN_KEY2) | (1u << BTN_KEY3),
};

static const char *s_modifier_labels[] = {
    "R",
    "G",
    "B",
};

static const char *s_modifier_names[] = {
    "",
    "R",
    "G",
    "R+G",
    "B",
    "R+B",
    "G+B",
    "R+G+B",
};

static const char *s_volume_messages[] = {
    "¡Perdiste por hablar\nmuy alto!",
    "¡Habla menos!",
    "Shhhh! Despertaste\na todo el barrio!",
    "Shhhhhhhhhhhhhhhhhhh!"
};

static const char *s_timeout_messages[] = {
    "Te quedaste\nsin tiempo!",
    "Manuelita?",
    "Si no corres se te\nva el bondi...",
    "Fui yo Barry."
};

// Elige un objetivo aleatorio distinto del actual.
static uint32_t next_direction(uint32_t current, uint32_t count)
{
    uint32_t next = esp_random() % (count - 1);
    if (next >= current) {
        next++;
    }
    return next;
}

// Formatea los colores requeridos y la dirección objetivo.
static void format_target(char *buffer, size_t size, uint32_t target, uint32_t direction_count)
{
    const uint32_t direction_index = target % direction_count;
    const uint32_t modifier_index = target / direction_count;
    snprintf(buffer, size, "%s%s%s",
             s_modifier_names[modifier_index],
             s_modifier_names[modifier_index][0] ? "+" : "",
             s_directions[direction_index].name);
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

// Dibuja un botón como bloque lleno o como contorno según su estado.
static void draw_key(int x, int y, int size, bool pressed)
{
    if (pressed) {
        display_fill_rect(x, y, size, size, true);
    } else {
        display_fill_rect(x, y, size, size, false);
        display_draw_rect(x, y, size, size);
    }
}

// Dibuja BOOM a gran escala usando patrones de píxeles.
static void draw_boom_screen(void)
{
    static const uint8_t glyphs[][7] = {
        {0x04, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04},
        {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
        {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
    };
    const int scale_x = 3;
    const int scale_y = 8;
    const int glyph_advance = 20;
    const int start_x = 7;
    const int start_y = 4;

    display_clear();
    for (int glyph = 0; glyph < 6; glyph++) {
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (glyphs[glyph][row] & (1u << (4 - col))) {
                    display_fill_rect(start_x + glyph * glyph_advance + col * scale_x,
                                      start_y + row * scale_y,
                                      scale_x, scale_y, true);
                }
            }
        }
    }
}

// Dibuja líneas centradas en un área, respetando los saltos de línea del mensaje.
static void draw_centered_message(const char *message, int area_y, int area_height)
{
    int line_count = 1;
    for (const char *p = message; *p; p++) {
        if (*p == '\n') {
            line_count++;
        }
    }

    int y = area_y + (area_height - line_count * DISPLAY_FONT_H) / 2;
    const char *line_start = message;
    const char *line_end = message;
    while (1) {
        while (*line_end && *line_end != '\n') {
            line_end++;
        }

        const size_t length = (size_t)(line_end - line_start);
        char line[22];
        const size_t copy_length = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
        memcpy(line, line_start, copy_length);
        line[copy_length] = '\0';
        const int x = (128 - (int)copy_length * DISPLAY_FONT_W) / 2;
        display_text(x, y, line, false);
        y += DISPLAY_FONT_H;

        if (*line_end == '\0') {
            break;
        }
        line_start = line_end + 1;
        line_end = line_start;
    }
}

// Muestra el inicio y el mensaje aleatorio seleccionado, si lo hay.
static void draw_start_screen(const char *message)
{
    display_clear();
    if (message != NULL) {
        draw_centered_message(message, 12, 24);
        display_text(22, 46, "PRESIONA START", false);
    } else {
        display_text(22, 28, "PRESIONA START", false);
    }
}

// Muestra el mensaje de derrota como único contenido de la pantalla.
static void draw_timeout_screen(const char *message)
{
    display_clear();
    draw_centered_message(message, 0, 64);
}

// Dibuja la interfaz del juego, el temporizador y los controles.
static void draw_ui(uint32_t pressed, uint32_t active_colors, int mic_level, bool audio_ok,
                    const char *game_status, int timer_ms, int timer_duration_ms)
{
    char line[24];

    display_clear();
    display_text(0, 1, "TIME", false);
    display_draw_rect(27, 0, 77, 10);
    display_fill_rect(29, 2, 73 * timer_ms / timer_duration_ms, 6, true);
    snprintf(line, sizeof(line), "%2d", (timer_ms + 999) / 1000);
    display_text(107, 1, line, false);

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
    for (int i = 0; i < 3; i++) {
        bool on = active_colors & (1u << (BTN_KEY1 + i));
        int x = 50 + i * 15;
        draw_key(x, 40, 13, on);
        char color[2] = {s_modifier_labels[i][0], 0};
        display_text(x + 4, 43, color, on);
    }
}

// Inicializa los periféricos y ejecuta el juego y la interfaz en bucle.
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
    const uint32_t modifier_count = (uint32_t)(sizeof(s_modifier_masks) / sizeof(s_modifier_masks[0]));
    const uint32_t target_count = direction_count * modifier_count;
    uint32_t target = esp_random() % target_count;
    bool showing_result = false;
    bool game_active = false;
    bool showing_timeout_message = false;
    bool timeout_boom = false;
    uint32_t volume_message = 0;
    uint32_t timeout_message = 0;
    bool wait_for_neutral = true;
    bool answer_correct = false;
    bool start_armed = true;
    uint32_t active_colors = 0;
    int round_timer_ms = GAME_TIMER_MS;
    uint32_t previous_buttons = 0;
    uint32_t candidate = 0;
    TickType_t candidate_since = 0;
    TickType_t result_since = 0;
    TickType_t game_started = 0;
    TickType_t silence_since = 0;
    TickType_t mic_detection_after = 0;
    bool showing_silence = false;
    bool loud_noise_candidate = false;
    bool volume_boom = false;
    bool showing_volume_message = false;
    int log_div = 0;
    while (1) {
        button_event_t ev;
        if (xQueueReceive(events, &ev, pdMS_TO_TICKS(UI_PERIOD_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "button %s %s", buttons_name(ev.id), ev.pressed ? "pressed" : "released");
            if (ev.pressed && audio_ok) {
                mic_detection_after = xTaskGetTickCount() + pdMS_TO_TICKS(MIC_REARM_DELAY_MS);
                audio_play_tone(s_button_freq[ev.id], 100);
            }
        }

        const TickType_t now = xTaskGetTickCount();
        const uint32_t all_buttons = buttons_read();
        const int mic_level = audio_ok ? audio_mic_level() : 0;
        const bool can_detect_noise = game_active && !showing_result && !showing_silence &&
                                      now >= mic_detection_after;
        if (!can_detect_noise) {
            loud_noise_candidate = false;
        } else if (mic_level >= SILENCE_LEVEL) {
            loud_noise_candidate = true;
        } else {
            loud_noise_candidate = false;
        }
        if (game_active && !showing_result && !showing_silence && loud_noise_candidate &&
            can_detect_noise) {
            showing_silence = true;
            silence_since = now;
            game_active = false;
            showing_result = false;
            showing_timeout_message = false;
            timeout_boom = false;
            volume_boom = true;
            active_colors = 0;
            loud_noise_candidate = false;
            start_armed = false;
            wait_for_neutral = true;
            candidate = 0;
        }

        if (showing_silence && now - silence_since >= pdMS_TO_TICKS(SILENCE_SCREEN_MS)) {
            showing_silence = false;
            if (timeout_boom) {
                const uint32_t message_count =
                    (uint32_t)(sizeof(s_timeout_messages) / sizeof(s_timeout_messages[0]));
                timeout_message = esp_random() % message_count;
                showing_timeout_message = true;
                timeout_boom = false;
                start_armed = false;
            } else if (volume_boom) {
                const uint32_t message_count =
                    (uint32_t)(sizeof(s_volume_messages) / sizeof(s_volume_messages[0]));
                volume_message = esp_random() % message_count;
                showing_volume_message = true;
                volume_boom = false;
                start_armed = false;
            }
        }
        const uint32_t direction = all_buttons & DIRECTION_MASK;
        const uint32_t color_buttons = all_buttons & MODIFIER_MASK;
        const uint32_t color_pressed = color_buttons & ~previous_buttons;
        previous_buttons = color_buttons;
        if ((showing_timeout_message || showing_volume_message) && !showing_silence) {
            if (!(all_buttons & (1u << BTN_KEY4))) {
                start_armed = true;
            } else if (start_armed) {
                showing_timeout_message = false;
                showing_volume_message = false;
                start_armed = false;
            }
        } else if (!game_active && !showing_silence && !showing_result) {
            if (!(all_buttons & (1u << BTN_KEY4))) {
                start_armed = true;
            } else if (start_armed) {
                start_armed = false;
                game_active = true;
                game_started = now;
                mic_detection_after = now + pdMS_TO_TICKS(MIC_STARTUP_DELAY_MS);
                round_timer_ms = GAME_TIMER_MS;
                target = esp_random() % target_count;
                active_colors = 0;
                loud_noise_candidate = false;
                wait_for_neutral = true;
            }
        }

        if (game_active && !showing_silence && !showing_result) {
            for (int i = 0; i < 3; i++) {
                const uint32_t color = 1u << (BTN_KEY1 + i);
                if (color_pressed & color) {
                    active_colors ^= color;
                }
            }
        }

        if (game_active && !showing_silence && !showing_result &&
            now - game_started >= pdMS_TO_TICKS(round_timer_ms)) {
            game_active = false;
            showing_silence = true;
            timeout_boom = true;
            silence_since = now;
            showing_timeout_message = false;
            active_colors = 0;
            start_armed = false;
            wait_for_neutral = true;
            candidate = 0;
        }

        if (showing_silence) {
            wait_for_neutral = true;
            candidate = 0;
        } else if (showing_result) {
            if (direction == 0) {
                wait_for_neutral = false;
            }
            if (now - result_since >= pdMS_TO_TICKS(GAME_RESULT_MS)) {
                showing_result = false;
                candidate = 0;
                target = next_direction(target, target_count);
                active_colors = 0;
                if (answer_correct) {
                    round_timer_ms = round_timer_ms * 995 / 1000;
                    if (round_timer_ms < 1) {
                        round_timer_ms = 1;
                    }
                }
                game_started = now;
                wait_for_neutral = direction != 0;
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
            const uint32_t direction_index = target % direction_count;
            const uint32_t modifier_index = target / direction_count;
            answer_correct = direction == s_directions[direction_index].mask &&
                             active_colors == s_modifier_masks[modifier_index];
            showing_result = true;
            result_since = now;
            wait_for_neutral = true;
            candidate = 0;
        }

        if (display_ok) {
            if (showing_silence) {
                draw_boom_screen();
            } else if (showing_timeout_message) {
                draw_timeout_screen(s_timeout_messages[timeout_message]);
            } else if (showing_volume_message) {
                draw_timeout_screen(s_volume_messages[volume_message]);
            } else if (!game_active) {
                draw_start_screen(NULL);
            } else {
                char game_status[16];
                TickType_t elapsed = now - game_started;
                int timer_ms = elapsed >= pdMS_TO_TICKS(round_timer_ms)
                                   ? 0
                                   : round_timer_ms - (int)pdTICKS_TO_MS(elapsed);
                if (showing_result) {
                    snprintf(game_status, sizeof(game_status), "%s", answer_correct ? "RIGHT" : "WRONG");
                } else {
                    format_target(game_status, sizeof(game_status), target, direction_count);
                }
                draw_ui(buttons_read(), active_colors, mic_level, audio_ok, game_status, timer_ms,
                        round_timer_ms);
            }
            display_flush();
        }

        if (audio_ok && ++log_div >= 1000 / UI_PERIOD_MS) {
            log_div = 0;
            ESP_LOGI(TAG, "mic level: %d", mic_level);
        }
    }
}

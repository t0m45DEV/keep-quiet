#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "peer_link.h"

static const char *TAG = "demo";

#define UI_PERIOD_MS 50
#define RIGHT_RESULT_MS 1000
#define WRONG_RESULT_MS 2000
#define GAME_TIMER_MS (20 * 1000)
#define BOOM_FRAME_MS 80
#define BOOM_FRAME_COUNT 5
#define BOOM_TITLE_HOLD_MS 600
#define PEER_PACKET_RETRY_MS 500
#define DIRECTION_STABLE_MS 80
#define SILENCE_LEVEL 80
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
    "Y",
    "B",
    "M",
    "C",
    "W",
};

static const char *s_volume_messages[] = {
//    "Perdiste por hablar\nmuy alto!",
//    "Habla menos!",
    "Despertaste a\ntodo el barrio!",
    "Shhhhhhhhhhhhhhhhhhh!",
    "Ruidoso como gato\na la madrugada.",
    "Estornudaste? Salud!",
    "Sneak decreased\nto -100",
    "SILENZIO, BRUNO!",
    "Snake? Snake!?\nSNAKEEEEE!!!!"
};

static const char *s_timeout_messages[] = {
    "Te quedaste\nsin tiempo!",
    "Manuelita?",
    "Si no corres, se te\nva el bondi...",
    "Fui yo, Barry.\nTe hice perder tiempo.",
    "Corre, Forrest,\nCORRE.",
    "\"Meep, meep.\"\n-El correcaminos.",
    "Kuchau.",
    "Gotta go fast!",
    "Se te enfria\nel mate, pibe.",
    "Tick Tock\non the clock.",
    "Mission failed.\nWe'll get em next time."
};

static bool is_c4_target(uint32_t target, uint32_t direction_count){
    const uint32_t direction_index = target % direction_count;
    const uint32_t modifier_index = target / direction_count;
    return modifier_index == 6 && direction_index == 3;
}

// Formatea los colores requeridos y la dirección objetivo.
static void format_target(char *buffer, size_t size, uint32_t target, uint32_t direction_count){
    const uint32_t direction_index = target % direction_count;
    const uint32_t modifier_index = target / direction_count;
    snprintf(buffer, size, "%s%s%s",
             modifier_index == 0 ? "K" : s_modifier_names[modifier_index],
             s_directions[direction_index].name,
             is_c4_target(target, direction_count) ? "!!!!" : "");
}

static const char *modifier_name_for_mask(uint32_t mask)
{
    if (mask == 0) {
        return "K";
    }
    for (int i = 1; i < 8; i++) {
        if (mask == s_modifier_masks[i]) {
            return s_modifier_names[i];
        }
    }
    return "";
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

// Dibuja una explosión arcade original que se expande hasta cubrir la pantalla.
static void draw_explosion_frame(uint32_t frame)
{
    static const int radius_by_frame[BOOM_FRAME_COUNT] = {
        7, 15, 25, 37, 50
    };

    display_clear();
    if (frame == BOOM_FRAME_COUNT - 1) {
        display_fill_rect(0, 0, 128, 64, true);
        for (int y = 1; y < 64; y += 3) {
            for (int x = (y / 3) % 2; x < 128; x += 3) {
                if ((x / 3 * 7 + y / 3 * 11) % 17 == 0) {
                    display_fill_rect(x, y, 1, 1, false);
                }
            }
        }
        return;
    }

    const int radius = radius_by_frame[frame];
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) {
            const int dx = (x - 63) / 2;
            const int dy = y - 31;
            const int abs_x = dx < 0 ? -dx : dx;
            const int abs_y = dy < 0 ? -dy : dy;
            const int major = abs_x > abs_y ? abs_x : abs_y;
            const int minor = abs_x > abs_y ? abs_y : abs_x;
            const int distance = major + minor / 2;
            const int diagonal_delta = abs_x > abs_y ? abs_x - abs_y : abs_y - abs_x;
            const bool ray = major > 4 && (minor <= 1 || diagonal_delta <= 1);
            const int ray_extension = ray ? 8 + (int)frame * 2 : 0;
            const int edge_noise = ((x / 3 * 17 + y / 3 * 31 + frame * 13) % 9) - 4;
            const bool inside = distance <= radius + edge_noise + ray_extension;
            const int texture = (x / 3 * 5 + y / 3 * 3 + frame * 2) % 11;
            const bool core = distance < radius / 3;
            if (inside && (frame < 2 || core || texture != 0)) {
                display_set_pixel(x, y, true);
            }
        }
    }

    static const int particle_dx[] = {-1, 1, 0, 0, -1, 1, -2, 2};
    static const int particle_dy[] = {-1, -1, 1, 1, 0, 0, -1, 1};
    const int spread = 5 + (int)frame * 5;
    for (int i = 0; i < 8; i++) {
        const int particle_x = 63 + particle_dx[i] * spread * 2;
        const int particle_y = 31 + particle_dy[i] * spread;
        display_fill_rect(particle_x, particle_y, 2, 2, true);
    }
}

// Dibuja ¡BOOM! en grande y espera la confirmación de START.
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
    const int scale_y = 6;
    const int glyph_advance = 20;
    const int start_x = 7;
    const int start_y = 2;

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
    display_text(22, 54, "PRESIONE START", false);
}

static void draw_title_burst(int center_x, int center_y, uint32_t phase)
{
    display_fill_rect(center_x - 1, center_y - 1, 3, 3, true);
    display_fill_rect(center_x - 1, center_y - 6, 3, 2, true);
    display_fill_rect(center_x - 1, center_y + 4, 3, 2, true);
    display_fill_rect(center_x - 6, center_y - 1, 2, 3, true);
    display_fill_rect(center_x + 4, center_y - 1, 2, 3, true);
    if (phase & 1u) {
        display_set_pixel(center_x - 4, center_y - 4, true);
        display_set_pixel(center_x + 4, center_y - 4, true);
        display_set_pixel(center_x - 4, center_y + 4, true);
        display_set_pixel(center_x + 4, center_y + 4, true);
    }
}

static void draw_large_title_word(const char *word, int y)
{
    static const struct {
        char letter;
        uint8_t rows[7];
    } glyphs[] = {
        {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    };
    const int scale = 2;
    const int char_advance = 12;
    const int word_width = (int)strlen(word) * char_advance - 2;
    int x = (128 - word_width) / 2;

    for (const char *letter = word; *letter; letter++, x += char_advance) {
        for (size_t glyph = 0; glyph < sizeof(glyphs) / sizeof(glyphs[0]); glyph++) {
            if (glyphs[glyph].letter != *letter) {
                continue;
            }
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (glyphs[glyph].rows[row] & (1u << (4 - col))) {
                        display_fill_rect(x + col * scale, y + row * scale,
                                          scale, scale, true);
                    }
                }
            }
            break;
        }
    }
}

static void draw_title_screen(uint32_t phase)
{
    display_clear();
    draw_title_burst(20, 24, phase);
    draw_title_burst(108, 24, phase + 1);
    draw_title_burst(20, 43, phase + 1);
    draw_title_burst(108, 43, phase);
    draw_large_title_word("KEEP", 19);
    draw_large_title_word("QUIET", 35);
}

static void draw_intro_screen(void)
{
    display_clear();
    display_text(10, 8, "Un juego hecho", false);
    display_text(10, 22, "por daltonicos,", false);
    display_text(10, 36, "para daltonicos...", false);
    display_text(22, 54, "PRESIONE START", false);
}

static void draw_centered_text(const char *text, int y){
    const int x = (128 - (int)strlen(text) * DISPLAY_FONT_W) / 2;
    display_text(x, y, text, false);
}

static void draw_waiting_for_player(void){
    display_clear();
    draw_centered_text("ESPERANDO", 20);
    draw_centered_text("AL OTRO JUGADOR", 34);
}

static void draw_peer_link_screen(bool connected, bool local_is_a){
    display_clear();
    draw_centered_text(connected ? "PLACAS CONECTADAS" : "BUSCANDO OTRA", 16);
    draw_centered_text(connected ? (local_is_a ? "SOY PLACA A" : "SOY PLACA B")
                                 : "PLACA...", 30);
    if (!connected) {
        draw_centered_text("ENCIENDAN LAS DOS", 46);
    }
}

// Dibuja líneas centradas en un área, respetando los saltos de línea del mensaje.
static void draw_centered_message(const char *message, int area_y, int area_height){
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
        draw_centered_text(line, y);
        y += DISPLAY_FONT_H;

        if (*line_end == '\0') {
            break;
        }
        line_start = line_end + 1;
        line_end = line_start;
    }
}

// Muestra el inicio y el mensaje aleatorio seleccionado, si lo hay.
static void draw_start_screen(const char *message, bool hard_mode){
    display_clear();
    if (message != NULL) {
        draw_centered_message(message, 12, 24);
        display_text(22, 46, "PRESIONE START", false);
    } else {
        display_text(16, 7, "DIFICULTAD:", false);
        display_text(26, hard_mode ? 38 : 24, ">", false);
        display_text(35, 24, "NORMALITO", false);
        display_text(35, 38, "RE DIFICIL", false);
        display_text(22, 54, "APRETAME EL START  V", false);
    }
}

// Muestra el mensaje de derrota, el puntaje y las acciones disponibles.
static void draw_defeat_screen(const char *message, uint32_t total_score, uint32_t selection){
    char score_line[32];

    display_clear();
    draw_centered_message(message, 0, 24);
    const int score_length = snprintf(score_line, sizeof(score_line), "Personas salvadas: %u",
                                      (unsigned int)total_score);
    const char *score_text = score_line;
    if (score_length * DISPLAY_FONT_W > 128) {
        snprintf(score_line, sizeof(score_line), "Puntos: %u", (unsigned int)total_score);
    }
    draw_centered_text(score_text, 26);
    display_text(6, 42, selection == 0 ? ">" : " ", false);
    display_text(18, 42, "Volver a jugar", false);
    display_text(6, 54, selection == 1 ? ">" : " ", false);
    display_text(18, 54, "Cambiar modo", false);
}

// Dibuja la interfaz del juego, el temporizador y los controles.
static void draw_ui(uint32_t pressed, uint32_t active_colors, int mic_level, bool audio_ok,
                    const char *game_status, int timer_ms, int timer_duration_ms,
                    uint32_t streak){
    char line[24];
    char order_line[24];
    char sending_line[24];

    display_clear();
    display_text(0, 1, "TIME", false);
    display_draw_rect(27, 0, 77, 10);
    display_fill_rect(29, 2, 73 * timer_ms / timer_duration_ms, 6, true);
    snprintf(line, sizeof(line), "%2d", (timer_ms + 999) / 1000);
    display_text(107, 1, line, false);

    display_text(0, 14, "LOUD", false);
    display_draw_rect(27, 13, 77, 10);
    const int mic_bar_level = mic_level >= SILENCE_LEVEL
                                  ? 100
                                  : 20 + mic_level * 80 / SILENCE_LEVEL;
    display_fill_rect(29, 15, mic_bar_level * 73 / 100, 6, true);
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

    snprintf(order_line, sizeof(order_line), "Order: %s", game_status);
    display_text(44, 29, order_line, false);
    for (int i = 0; i < 3; i++) {
        bool on = active_colors & (1u << (BTN_KEY1 + i));
        int x = 46 + i * 15;
        draw_key(x, 40, 13, on);
        char color[2] = {s_modifier_labels[i][0], 0};
        display_text(x + 4, 43, color, on);
    }

    snprintf(sending_line, sizeof(sending_line), "Sending: %s",
             modifier_name_for_mask(active_colors));
    display_text(44, 56, sending_line, false);

    // Racha
    if (streak > 0) {
        snprintf(line, sizeof(line), "%u", (unsigned int)streak);
        const int x = 128 - (int)strlen(line) * DISPLAY_FONT_W;
        display_text(x, 56, line, false);
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
        for (uint32_t frame = 0; frame < 30; frame++) {
            draw_title_screen(frame);
            display_flush();
            vTaskDelay(pdMS_TO_TICKS(BOOM_FRAME_MS));
        }

        const uint32_t start_button_mask = 1u << BTN_KEY4;
        draw_intro_screen();
        display_flush();
        while (buttons_read() & start_button_mask) {
            vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
        }
        while (!(buttons_read() & start_button_mask)) {
            vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
        }
        while (buttons_read() & start_button_mask) {
            vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
        }
    }

    if (display_ok) {
        draw_peer_link_screen(false, false);
        display_flush();
    }
    ESP_ERROR_CHECK(peer_link_start());
    ESP_ERROR_CHECK(peer_link_wait());
    const bool local_is_a = peer_link_is_a();
    if (display_ok) {
        draw_peer_link_screen(true, local_is_a);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
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
    uint32_t peer_target = 0;
    uint32_t local_code_round = 0;
    uint32_t peer_code_round = 0;
    bool peer_target_ready = false;
    bool local_answer_ready = false;
    bool showing_result = false;
    bool game_active = false;
    bool waiting_for_peer_start = false;
    bool local_ready = false;
    bool peer_ready = false;
    bool ready_mode = false;
    TickType_t last_ready_sent = 0;
    TickType_t last_code_sent = 0;
    TickType_t last_result_sent = 0;
    TickType_t last_failure_sent = 0;
    TickType_t last_score_sent = 0;
    uint32_t failure_reason = 0;
    bool failure_from_peer = false;
    int local_remaining_ms = GAME_TIMER_MS;
    bool showing_timeout_message = false;
    bool timeout_boom = false;
    uint32_t volume_message = 0;
    uint32_t timeout_message = 0;
    uint32_t defeat_selection = 0;
    bool wait_for_neutral = true;
    bool answer_correct = false;
    bool start_armed = true;
    uint32_t streak = 0;
    uint32_t total_score = 0;
    uint32_t peer_total_score = 0;
    uint32_t active_colors = 0;
    int round_timer_ms = GAME_TIMER_MS;
    uint32_t previous_buttons = 0;
    uint32_t previous_menu_buttons = 0;
    bool hard_mode = false;
    uint32_t candidate = 0;
    TickType_t candidate_since = 0;
    TickType_t result_since = 0;
    TickType_t game_started = 0;
    TickType_t mic_detection_after = 0;
    bool showing_silence = false;
    TickType_t boom_started = 0;
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
        peer_link_message_t peer_message;
        while (peer_link_receive(&peer_message)) {
            if (peer_message.type == PEER_LINK_MESSAGE_SCORE) {
                if (showing_silence || showing_timeout_message || showing_volume_message) {
                    peer_total_score = peer_message.value;
                }
            } else if (peer_message.type == PEER_LINK_MESSAGE_READY) {
                if (peer_message.value > 1) {
                    continue;
                }
                peer_ready = true;
            } else if (peer_message.type == PEER_LINK_MESSAGE_FAILURE &&
                       (peer_message.value == 1 || peer_message.value == 2) &&
                       game_active && !showing_silence) {
                game_active = false;
                showing_silence = true;
                showing_result = false;
                boom_started = now;
                failure_reason = peer_message.value;
                failure_from_peer = true;
                timeout_boom = failure_reason == 1;
                volume_boom = failure_reason == 2;
                showing_timeout_message = false;
                showing_volume_message = false;
                local_ready = false;
                peer_ready = false;
                waiting_for_peer_start = false;
                active_colors = 0;
                start_armed = false;
                wait_for_neutral = true;
                candidate = 0;
            } else if (showing_silence) {
                continue;
            } else if (peer_message.type == PEER_LINK_MESSAGE_CODE &&
                       game_active && peer_message.value < target_count &&
                       (!peer_target_ready || peer_message.round > peer_code_round)) {
                peer_target = peer_message.value;
                peer_code_round = peer_message.round;
                peer_target_ready = true;
                local_ready = false;
                showing_result = false;
                local_answer_ready = false;
                last_result_sent = 0;
                game_started = now;
                local_remaining_ms = round_timer_ms;
                mic_detection_after = now + pdMS_TO_TICKS(MIC_STARTUP_DELAY_MS);
                active_colors = 0;
                wait_for_neutral = true;
                candidate = 0;
            } else if (peer_message.type == PEER_LINK_MESSAGE_RESULT &&
                       game_active &&
                       peer_message.round == local_code_round &&
                       peer_message.value <= 1) {
                if (peer_message.value != 0) {
                    target = esp_random() % target_count;
                    local_code_round++;
                    last_code_sent = 0;
                }
            }
        }

        if (local_ready &&
            (waiting_for_peer_start || (game_active && !peer_target_ready)) &&
            (last_ready_sent == 0 ||
             now - last_ready_sent >= pdMS_TO_TICKS(PEER_PACKET_RETRY_MS))) {
            const esp_err_t ready_err = peer_link_send_ready(ready_mode);
            if (ready_err != ESP_OK) {
                ESP_LOGW(TAG, "could not announce game start: %s", esp_err_to_name(ready_err));
            }
            last_ready_sent = now;
        }

        if (waiting_for_peer_start && local_ready && peer_ready) {
            hard_mode = ready_mode;
            waiting_for_peer_start = false;
            peer_ready = false;
            game_active = true;
            game_started = now;
            mic_detection_after = now + pdMS_TO_TICKS(MIC_STARTUP_DELAY_MS);
            round_timer_ms = GAME_TIMER_MS;
            local_code_round = 0;
            peer_code_round = 0;
            target = esp_random() % target_count;
            peer_target_ready = false;
            local_answer_ready = false;
            showing_result = false;
            local_remaining_ms = round_timer_ms;
            last_code_sent = 0;
            last_result_sent = 0;
            last_failure_sent = 0;
            last_score_sent = 0;
            failure_from_peer = false;
            active_colors = 0;
            streak = 0;
            total_score = 0;
            peer_total_score = 0;
            loud_noise_candidate = false;
            wait_for_neutral = true;
        }

        if (game_active &&
            (last_code_sent == 0 ||
             now - last_code_sent >= pdMS_TO_TICKS(PEER_PACKET_RETRY_MS))) {
            const esp_err_t code_err = peer_link_send_code(local_code_round, target);
            if (code_err != ESP_OK) {
                ESP_LOGW(TAG, "could not send round code: %s", esp_err_to_name(code_err));
            }
            last_code_sent = now;
        }

        if (local_answer_ready &&
            (last_result_sent == 0 ||
             now - last_result_sent >= pdMS_TO_TICKS(PEER_PACKET_RETRY_MS))) {
            const esp_err_t result_err =
                peer_link_send_result(peer_code_round, answer_correct);
            if (result_err != ESP_OK) {
                ESP_LOGW(TAG, "could not send round result: %s", esp_err_to_name(result_err));
            }
            last_result_sent = now;
        }

        if (showing_silence && !failure_from_peer &&
            (last_failure_sent == 0 ||
             now - last_failure_sent >= pdMS_TO_TICKS(PEER_PACKET_RETRY_MS))) {
            const esp_err_t failure_err =
                peer_link_send_failure(local_code_round, failure_reason == 1);
            if (failure_err != ESP_OK) {
                ESP_LOGW(TAG, "could not send game over: %s", esp_err_to_name(failure_err));
            }
            last_failure_sent = now;
        }

        if ((showing_silence || showing_timeout_message || showing_volume_message) &&
            (last_score_sent == 0 ||
             now - last_score_sent >= pdMS_TO_TICKS(PEER_PACKET_RETRY_MS))) {
            const esp_err_t score_err = peer_link_send_score(total_score);
            if (score_err != ESP_OK) {
                ESP_LOGW(TAG, "could not send final score: %s", esp_err_to_name(score_err));
            }
            last_score_sent = now;
        }

        const bool can_detect_noise = game_active && peer_target_ready &&
                                      !local_answer_ready && !showing_result &&
                                      !showing_silence && now >= mic_detection_after;
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
            boom_started = now;
            game_active = false;
            showing_result = false;
            showing_timeout_message = false;
            timeout_boom = false;
            volume_boom = true;
            failure_reason = 2;
            failure_from_peer = false;
            last_failure_sent = 0;
            local_ready = false;
            peer_ready = false;
            waiting_for_peer_start = false;
            active_colors = 0;
            loud_noise_candidate = false;
            start_armed = false;
            wait_for_neutral = true;
            candidate = 0;
        }

        const uint32_t direction = all_buttons & DIRECTION_MASK;
        const uint32_t menu_buttons = all_buttons & ((1u << BTN_UP) | (1u << BTN_DOWN));
        const uint32_t menu_button_pressed = menu_buttons & ~previous_menu_buttons;
        previous_menu_buttons = menu_buttons;
        const uint32_t color_buttons = all_buttons & MODIFIER_MASK;
        const uint32_t color_pressed = color_buttons & ~previous_buttons;
        previous_buttons = color_buttons;
        if (showing_silence) {
            const TickType_t boom_duration =
                pdMS_TO_TICKS(BOOM_FRAME_MS * BOOM_FRAME_COUNT + BOOM_TITLE_HOLD_MS);
            if (now - boom_started >= boom_duration &&
                !(all_buttons & (1u << BTN_KEY4))) {
                start_armed = true;
            } else if (now - boom_started >= boom_duration && start_armed) {
                showing_silence = false;
                start_armed = false;
                if (timeout_boom) {
                    const uint32_t message_count =
                        (uint32_t)(sizeof(s_timeout_messages) / sizeof(s_timeout_messages[0]));
                    timeout_message = esp_random() % message_count;
                    showing_timeout_message = true;
                    timeout_boom = false;
                } else if (volume_boom) {
                    const uint32_t message_count =
                        (uint32_t)(sizeof(s_volume_messages) / sizeof(s_volume_messages[0]));
                    volume_message = esp_random() % message_count;
                    showing_volume_message = true;
                    volume_boom = false;
                }
                defeat_selection = 0;
            }
        } else if (showing_timeout_message || showing_volume_message) {
            if (menu_button_pressed & (1u << BTN_UP)) {
                defeat_selection = 0;
            } else if (menu_button_pressed & (1u << BTN_DOWN)) {
                defeat_selection = 1;
            }
            if (!(all_buttons & (1u << BTN_KEY4))) {
                start_armed = true;
            } else if (start_armed) {
                showing_timeout_message = false;
                showing_volume_message = false;
                start_armed = false;
                if (defeat_selection == 0) {
                    waiting_for_peer_start = true;
                    local_ready = true;
                    ready_mode = hard_mode;
                    last_ready_sent = 0;
                }
            }
        } else if (!game_active && !waiting_for_peer_start &&
                   !showing_silence && !showing_result) {
            if (!showing_timeout_message && !showing_volume_message) {
                if (menu_button_pressed & (1u << BTN_UP)) {
                    hard_mode = false;
                } else if (menu_button_pressed & (1u << BTN_DOWN)) {
                    hard_mode = true;
                }
            }
            if (!(all_buttons & (1u << BTN_KEY4))) {
                start_armed = true;
            } else if (start_armed) {
                start_armed = false;
                waiting_for_peer_start = true;
                local_ready = true;
                ready_mode = hard_mode;
                last_ready_sent = 0;
            }
        }

        if (game_active && peer_target_ready && !local_answer_ready &&
            !showing_silence && !showing_result) {
            for (int i = 0; i < 3; i++) {
                const uint32_t color = 1u << (BTN_KEY1 + i);
                if (color_pressed & color) {
                    active_colors ^= color;
                }
            }
        }

        if (game_active && peer_target_ready && !local_answer_ready &&
            !showing_silence && !showing_result &&
            now - game_started >= pdMS_TO_TICKS(round_timer_ms)) {
            game_active = false;
            showing_silence = true;
            boom_started = now;
            timeout_boom = true;
            volume_boom = false;
            failure_reason = 1;
            failure_from_peer = false;
            last_failure_sent = 0;
            local_ready = false;
            peer_ready = false;
            waiting_for_peer_start = false;
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
            const int result_duration_ms = answer_correct ? RIGHT_RESULT_MS : WRONG_RESULT_MS;
            if (now - result_since >= pdMS_TO_TICKS(result_duration_ms)) {
                showing_result = false;
                candidate = 0;
                if (!answer_correct) {
                    local_answer_ready = false;
                    active_colors = 0;
                }
                wait_for_neutral = direction != 0;
            }
        } else if (!peer_target_ready || local_answer_ready) {
            candidate = 0;
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
            const uint32_t direction_index = peer_target % direction_count;
            const uint32_t modifier_index = peer_target / direction_count;
            answer_correct = direction == s_directions[direction_index].mask &&
                             active_colors == s_modifier_masks[modifier_index];
            streak = answer_correct ? (streak % 10) + 1 : 0;
            if (answer_correct) {
                total_score++;
            }
            local_answer_ready = true;
            const TickType_t elapsed = now - game_started;
            local_remaining_ms = elapsed >= pdMS_TO_TICKS(round_timer_ms)
                                     ? 0
                                     : round_timer_ms - (int)pdTICKS_TO_MS(elapsed);
            if (answer_correct) {
                round_timer_ms = round_timer_ms * 995 / 1000;
                if (round_timer_ms < 1) {
                    round_timer_ms = 1;
                }
            }
            showing_result = true;
            result_since = now;
            last_result_sent = 0;
            wait_for_neutral = true;
            candidate = 0;
        }

        if (display_ok) {
            if (showing_silence) {
                const uint32_t frame = (uint32_t)pdTICKS_TO_MS(now - boom_started) /
                                       BOOM_FRAME_MS;
                if (frame < BOOM_FRAME_COUNT) {
                    draw_explosion_frame(frame);
                } else {
                    draw_boom_screen();
                }
            } else if (showing_timeout_message) {
                draw_defeat_screen(s_timeout_messages[timeout_message],
                                   total_score + peer_total_score,
                                   defeat_selection);
            } else if (showing_volume_message) {
                draw_defeat_screen(s_volume_messages[volume_message],
                                   total_score + peer_total_score,
                                   defeat_selection);
            } else if (!game_active) {
                if (waiting_for_peer_start) {
                    draw_waiting_for_player();
                } else {
                    draw_start_screen(NULL, hard_mode);
                }
            } else {
                char game_status[16];
                int timer_ms = local_remaining_ms;
                if (peer_target_ready && !local_answer_ready) {
                    TickType_t elapsed = now - game_started;
                    timer_ms = elapsed >= pdMS_TO_TICKS(round_timer_ms)
                                   ? 0
                                   : round_timer_ms - (int)pdTICKS_TO_MS(elapsed);
                }
                format_target(game_status, sizeof(game_status), target, direction_count);
                draw_ui(buttons_read(), active_colors, mic_level, audio_ok, game_status, timer_ms,
                        round_timer_ms, streak);
            }
            display_flush();
        }

        if (audio_ok && ++log_div >= 1000 / UI_PERIOD_MS) {
            log_div = 0;
            ESP_LOGI(TAG, "mic level: %d", mic_level);
        }
    }
}

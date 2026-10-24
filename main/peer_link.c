#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "peer_link.h"

static const char *TAG = "peer_link";

#define PEER_LINK_CHANNEL 1
#define PEER_LINK_DISCOVERY_MS 500
#define PEER_LINK_QUEUE_LENGTH 8
#define PEER_LINK_HELLO_SEEN_BIT BIT0
#define PEER_LINK_ACK_SEEN_BIT BIT1
#define PEER_LINK_CONNECTED_BITS (PEER_LINK_HELLO_SEEN_BIT | PEER_LINK_ACK_SEEN_BIT)
#define PEER_LINK_GAME_QUEUE_LENGTH 8

typedef enum {
    PEER_PACKET_HELLO = 1,
    PEER_PACKET_ACK,
    PEER_PACKET_CODE,
    PEER_PACKET_RESULT,
    PEER_PACKET_FAILURE,
    PEER_PACKET_READY,
    PEER_PACKET_SCORE,
} peer_packet_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t type;
    uint32_t round;
    uint32_t value;
} peer_packet_t;

typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    peer_packet_type_t type;
} peer_event_t;

static const uint32_t s_packet_magic = 0x4B51504Cu;
static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static QueueHandle_t s_peer_queue;
static QueueHandle_t s_game_queue;
static EventGroupHandle_t s_peer_events;
static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static bool s_local_is_a;

static void peer_receive_callback(const esp_now_recv_info_t *info, const uint8_t *data,
                                 int data_len)
{
    if (info == NULL || data == NULL || data_len < (int)sizeof(uint32_t)) {
        return;
    }

    uint32_t magic = 0;
    memcpy(&magic, data, sizeof(magic));
    if (magic != s_packet_magic) {
        return;
    }

    if (data_len == (int)sizeof(peer_packet_t)) {
        peer_packet_t packet;
        memcpy(&packet, data, sizeof(packet));
        if (packet.type != PEER_PACKET_CODE && packet.type != PEER_PACKET_RESULT &&
            packet.type != PEER_PACKET_FAILURE && packet.type != PEER_PACKET_READY &&
            packet.type != PEER_PACKET_SCORE) {
            return;
        }
        if (memcmp(info->src_addr, s_peer_mac, ESP_NOW_ETH_ALEN) != 0) {
            return;
        }
        const peer_link_message_t message = {
            .type = packet.type == PEER_PACKET_CODE ? PEER_LINK_MESSAGE_CODE
                    : packet.type == PEER_PACKET_RESULT ? PEER_LINK_MESSAGE_RESULT
                    : packet.type == PEER_PACKET_FAILURE ? PEER_LINK_MESSAGE_FAILURE
                    : packet.type == PEER_PACKET_READY ? PEER_LINK_MESSAGE_READY
                                                       : PEER_LINK_MESSAGE_SCORE,
            .round = packet.round,
            .value = packet.value,
        };
        if (xQueueSend(s_game_queue, &message, 0) != pdTRUE) {
            ESP_LOGW(TAG, "game message queue full; message dropped");
        }
        return;
    }

    if (data_len != (int)(sizeof(uint32_t) + sizeof(uint8_t))) {
        return;
    }
    peer_packet_t packet = {0};
    memcpy(&packet, data, (size_t)data_len);
    if (packet.type != PEER_PACKET_HELLO && packet.type != PEER_PACKET_ACK) {
        return;
    }
    peer_event_t event = {
        .type = (peer_packet_type_t)packet.type,
    };
    memcpy(event.mac, info->src_addr, ESP_NOW_ETH_ALEN);
    if (xQueueSend(s_peer_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "peer discovery queue full; will retry");
    }
}

static esp_err_t ensure_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {
        .channel = PEER_LINK_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    return esp_now_add_peer(&peer);
}

static esp_err_t send_peer_packet(const uint8_t *mac, peer_packet_type_t type)
{
    const peer_packet_t packet = {
        .magic = s_packet_magic,
        .type = (uint8_t)type,
        .round = 0,
        .value = 0,
    };
    return esp_now_send(mac, (const uint8_t *)&packet, sizeof(uint32_t) + sizeof(uint8_t));
}

static esp_err_t send_game_packet(peer_packet_type_t type, uint32_t round, uint32_t value)
{
    const peer_packet_t packet = {
        .magic = s_packet_magic,
        .type = (uint8_t)type,
        .round = round,
        .value = value,
    };
    return esp_now_send(s_peer_mac, (const uint8_t *)&packet, sizeof(packet));
}

static void peer_discovery_task(void *arg)
{
    uint8_t selected_peer[ESP_NOW_ETH_ALEN] = {0};
    bool has_selected_peer = false;
    EventBits_t peer_bits = 0;
    TickType_t last_hello = 0;
    peer_event_t event;

    while ((peer_bits & PEER_LINK_CONNECTED_BITS) != PEER_LINK_CONNECTED_BITS) {
        const TickType_t now = xTaskGetTickCount();
        if (now - last_hello >= pdMS_TO_TICKS(PEER_LINK_DISCOVERY_MS)) {
            const esp_err_t err = send_peer_packet(s_broadcast_mac, PEER_PACKET_HELLO);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "broadcast discovery failed: %s", esp_err_to_name(err));
            }
            last_hello = now;
        }

        if (xQueueReceive(s_peer_queue, &event, pdMS_TO_TICKS(50)) != pdTRUE) {
            peer_bits = xEventGroupGetBits(s_peer_events);
            continue;
        }

        if (has_selected_peer && memcmp(selected_peer, event.mac, ESP_NOW_ETH_ALEN) != 0) {
            continue;
        }
        if (!has_selected_peer) {
            memcpy(selected_peer, event.mac, ESP_NOW_ETH_ALEN);
            memcpy(s_peer_mac, event.mac, ESP_NOW_ETH_ALEN);
            uint8_t local_mac[ESP_NOW_ETH_ALEN];
            const esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, local_mac);
            if (mac_err != ESP_OK) {
                ESP_LOGE(TAG, "could not read station MAC: %s", esp_err_to_name(mac_err));
                continue;
            }
            s_local_is_a = memcmp(local_mac, s_peer_mac, ESP_NOW_ETH_ALEN) < 0;
            has_selected_peer = true;
        }

        esp_err_t err = ensure_peer(selected_peer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not add peer: %s", esp_err_to_name(err));
            continue;
        }

        if (event.type == PEER_PACKET_HELLO) {
            err = send_peer_packet(selected_peer, PEER_PACKET_ACK);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "peer acknowledgement failed: %s", esp_err_to_name(err));
                continue;
            }
            xEventGroupSetBits(s_peer_events, PEER_LINK_HELLO_SEEN_BIT);
        } else {
            xEventGroupSetBits(s_peer_events, PEER_LINK_ACK_SEEN_BIT);
        }
        peer_bits = xEventGroupGetBits(s_peer_events);
    }

    ESP_LOGI(TAG, "bidirectional ESP-NOW peer established");
    vTaskDelete(NULL);
}

esp_err_t peer_link_start(void)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "network interface init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");

    const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(PEER_LINK_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG,
                        "Wi-Fi channel setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power save setup failed");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "ESP-NOW init failed");

    s_peer_queue = xQueueCreate(PEER_LINK_QUEUE_LENGTH, sizeof(peer_event_t));
    ESP_RETURN_ON_FALSE(s_peer_queue != NULL, ESP_ERR_NO_MEM, TAG, "peer queue allocation failed");
    s_game_queue = xQueueCreate(PEER_LINK_GAME_QUEUE_LENGTH, sizeof(peer_link_message_t));
    ESP_RETURN_ON_FALSE(s_game_queue != NULL, ESP_ERR_NO_MEM, TAG, "game queue allocation failed");
    s_peer_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_peer_events != NULL, ESP_ERR_NO_MEM, TAG, "peer events allocation failed");

    esp_now_peer_info_t broadcast_peer = {
        .channel = PEER_LINK_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(broadcast_peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&broadcast_peer), TAG, "broadcast peer setup failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(peer_receive_callback), TAG,
                        "ESP-NOW receive callback setup failed");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(peer_discovery_task, "peer_discovery", 4096, NULL, 5, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "peer discovery task creation failed");

    return ESP_OK;
}

esp_err_t peer_link_wait(void)
{
    const EventBits_t bits = xEventGroupWaitBits(s_peer_events, PEER_LINK_CONNECTED_BITS,
                                                 pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_RETURN_ON_FALSE((bits & PEER_LINK_CONNECTED_BITS) == PEER_LINK_CONNECTED_BITS,
                        ESP_FAIL, TAG,
                        "peer connection ended unexpectedly");
    return ESP_OK;
}

bool peer_link_is_a(void)
{
    return s_local_is_a;
}

esp_err_t peer_link_send_code(uint32_t round, uint32_t target)
{
    return send_game_packet(PEER_PACKET_CODE, round, target);
}

esp_err_t peer_link_send_result(uint32_t round, bool correct)
{
    return send_game_packet(PEER_PACKET_RESULT, round, correct ? 1u : 0u);
}

esp_err_t peer_link_send_failure(uint32_t round, bool timeout)
{
    return send_game_packet(PEER_PACKET_FAILURE, round, timeout ? 1u : 2u);
}

esp_err_t peer_link_send_ready(bool hard_mode)
{
    return send_game_packet(PEER_PACKET_READY, 0, hard_mode ? 1u : 0u);
}

esp_err_t peer_link_send_score(uint32_t score)
{
    return send_game_packet(PEER_PACKET_SCORE, 0, score);
}

bool peer_link_receive(peer_link_message_t *message)
{
    return message != NULL && xQueueReceive(s_game_queue, message, 0) == pdTRUE;
}

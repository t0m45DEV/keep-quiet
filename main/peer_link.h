#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PEER_LINK_MESSAGE_CODE,
    PEER_LINK_MESSAGE_RESULT,
    PEER_LINK_MESSAGE_FAILURE,
    PEER_LINK_MESSAGE_READY,
    PEER_LINK_MESSAGE_SCORE,
} peer_link_message_type_t;

typedef struct {
    peer_link_message_type_t type;
    uint32_t round;
    uint32_t value;
} peer_link_message_t;

esp_err_t peer_link_start(void);
esp_err_t peer_link_wait(void);
bool peer_link_is_a(void);
esp_err_t peer_link_send_code(uint32_t round, uint32_t target);
esp_err_t peer_link_send_result(uint32_t round, bool correct);
esp_err_t peer_link_send_failure(uint32_t round, bool timeout);
esp_err_t peer_link_send_ready(bool hard_mode);
esp_err_t peer_link_send_score(uint32_t score);
bool peer_link_receive(peer_link_message_t *message);

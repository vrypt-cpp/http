#pragma once

#include "platform.h"
#include "conn.h"
#include "error.h"
#include <stdint.h>

typedef struct {
    const uint8_t *buf_200;
    uint32_t       len_200;
    const uint8_t *buf_404;
    uint32_t       len_404;
    const uint8_t *buf_405;
    uint32_t       len_405;
} http_responses_t;

extern const http_responses_t g_responses;

void         http_responses_init(void);
http_response_t http_parse(conn_t *c);

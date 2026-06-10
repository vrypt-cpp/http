#include "http.h"
#include "platform.h"
#include <stdint.h>
#include <stddef.h>

static const char RESP_200_STR[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "OK";

static const char RESP_404_STR[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 9\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Not Found";

static const char RESP_405_STR[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 18\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Method Not Allowed";

const http_responses_t g_responses = {
    .buf_200 = (const uint8_t*)RESP_200_STR,
    .len_200 = sizeof(RESP_200_STR) - 1,
    .buf_404 = (const uint8_t*)RESP_404_STR,
    .len_404 = sizeof(RESP_404_STR) - 1,
    .buf_405 = (const uint8_t*)RESP_405_STR,
    .len_405 = sizeof(RESP_405_STR) - 1,
};

void http_responses_init(void) {}

#define STEP_G           0
#define STEP_E           1
#define STEP_T           2
#define STEP_SP1         3
#define STEP_SLASH       4
#define STEP_SP2         5
#define STEP_H           6
#define STEP_HT          7
#define STEP_HTT         8
#define STEP_HTTP        9
#define STEP_PROTO_SLASH 10
#define STEP_ONE         11
#define STEP_DOT         12
#define STEP_ONE2        13
#define STEP_CR1         14
#define STEP_LF1         15
#define STEP_HDR_SCAN    16
#define STEP_HDR_CR      17
#define STEP_HDR_LF      18
#define STEP_HDR_CR2     19
#define STEP_DONE        20
#define STEP_ERROR       255

FORCE_INLINE bool is_other_method_byte(uint8_t b)
{
    return b == 'P' || b == 'D' || b == 'H' || b == 'O';
}

HOT http_response_t http_parse(conn_t *c)
{
    const uint8_t *buf  = c->recv_buf + c->recv_off;
    uint32_t       len  = c->recv_len - c->recv_off;
    uint32_t       i    = 0;
    uint8_t        step = c->parser.step;

    if (UNLIKELY(step == STEP_ERROR))
        return RESP_404;

    while (i < len) {
        uint8_t ch = buf[i++];

        switch (step) {

        case STEP_G:
            if (LIKELY(ch == 'G')) {
                c->parser.method = METHOD_GET;
                step = STEP_E;
            } else {
                c->parser.method = is_other_method_byte(ch)
                                   ? METHOD_OTHER : METHOD_UNKNOWN;
                step = STEP_ERROR;
                goto done;
            }
            break;

        case STEP_E:
            if (LIKELY(ch == 'E')) { step = STEP_T;    break; }
            step = STEP_ERROR; goto done;

        case STEP_T:
            if (LIKELY(ch == 'T')) { step = STEP_SP1;  break; }
            step = STEP_ERROR; goto done;

        case STEP_SP1:
            if (LIKELY(ch == ' ')) { step = STEP_SLASH; break; }
            step = STEP_ERROR; goto done;

        case STEP_SLASH:
            if (LIKELY(ch == '/')) { step = STEP_SP2;  break; }
            step = STEP_ERROR; goto done;

        case STEP_SP2:
            if (LIKELY(ch == ' ')) { step = STEP_H;    break; }
            step = STEP_ERROR; goto done;

        case STEP_H:
            if (LIKELY(ch == 'H')) { step = STEP_HT;   break; }
            step = STEP_ERROR; goto done;

        case STEP_HT:
            if (LIKELY(ch == 'T')) { step = STEP_HTT;  break; }
            step = STEP_ERROR; goto done;

        case STEP_HTT:
            if (LIKELY(ch == 'T')) { step = STEP_HTTP; break; }
            step = STEP_ERROR; goto done;

        case STEP_HTTP:
            if (LIKELY(ch == 'P')) { step = STEP_PROTO_SLASH; break; }
            step = STEP_ERROR; goto done;

        case STEP_PROTO_SLASH:
            if (LIKELY(ch == '/')) { step = STEP_ONE;  break; }
            step = STEP_ERROR; goto done;

        case STEP_ONE:
            if (LIKELY(ch == '1')) { step = STEP_DOT;  break; }
            step = STEP_ERROR; goto done;

        case STEP_DOT:
            if (LIKELY(ch == '.')) { step = STEP_ONE2; break; }
            step = STEP_ERROR; goto done;

        case STEP_ONE2:
            if (LIKELY(ch == '1')) { step = STEP_CR1;  break; }
            step = STEP_ERROR; goto done;

        case STEP_CR1:
            if (LIKELY(ch == '\r')) { step = STEP_LF1; break; }
            step = STEP_ERROR; goto done;

        case STEP_LF1:
            if (LIKELY(ch == '\n')) { step = STEP_HDR_SCAN; break; }
            step = STEP_ERROR; goto done;

        case STEP_HDR_SCAN:
            if (ch == '\r') step = STEP_HDR_CR;
            break;

        case STEP_HDR_CR:
            step = (ch == '\n') ? STEP_HDR_LF : STEP_HDR_SCAN;
            break;

        case STEP_HDR_LF:
            if (ch == '\r') { step = STEP_HDR_CR2; break; }
            step = STEP_HDR_SCAN;
            break;

        case STEP_HDR_CR2:
            if (ch == '\n') { step = STEP_DONE; goto done; }
            step = STEP_HDR_SCAN;
            break;

        default:
            step = STEP_ERROR;
            goto done;
        }
    }

done:
    c->parser.step  = step;
    c->recv_off    += i;

    if (step == STEP_DONE) {
        c->parser.complete = true;
        return RESP_200;
    }

    if (step == STEP_ERROR) {
        c->parser.complete = true;
        return (c->parser.method == METHOD_OTHER) ? RESP_405 : RESP_404;
    }

    c->parser.complete = false;
    return RESP_200;
}

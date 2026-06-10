#pragma once

#include <stdint.h>

typedef enum {
    ERR_OK              =  0,
    ERR_SYSCALL         = -1,
    ERR_NOMEM           = -2,
    ERR_INVAL           = -3,
    ERR_AGAIN           = -4,
    ERR_CLOSED          = -5,
    ERR_FULL            = -6,
    ERR_UNSUPPORTED     = -7,
    ERR_OVERFLOW        = -8,
} err_t;

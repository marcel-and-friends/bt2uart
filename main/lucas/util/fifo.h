#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} lucas_fifo_t;

bool lucas_fifo_init(lucas_fifo_t*, size_t initial_cap);

void lucas_fifo_free(lucas_fifo_t*);

void lucas_fifo_clear(lucas_fifo_t*);

void lucas_fifo_push(lucas_fifo_t*, uint8_t* data, size_t len);

void lucas_fifo_pop(lucas_fifo_t*, size_t num);

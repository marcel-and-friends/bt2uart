#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} fifo_t;

bool fifo_init(fifo_t*, size_t initial_cap);

void fifo_free(fifo_t*);

void fifo_push(fifo_t*, uint8_t* data, size_t len);

uint8_t* fifo_pop_begin(fifo_t*, size_t num);

void fifo_pop(fifo_t*, size_t num);

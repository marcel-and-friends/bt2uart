#include "fifo.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <esp_log.h>

#define LOG_TAG "LUCAS_BT_SPP"
#define LOGW(...) ESP_LOGW(LOG_TAG, __VA_ARGS__)

bool fifo_init(fifo_t* this, size_t initial_cap) {
    this->data = malloc(initial_cap);
    if (this->data == NULL) {
        return false;
    }

    this->cap = initial_cap;
    this->len = 0;

    return true;
}

void fifo_free(fifo_t* this) {
    free(this->data);
    memset(this, 0, sizeof(fifo_t));
}

void fifo_push(fifo_t* this, uint8_t* data, size_t len) {
    const size_t available_space = this->cap - this->len;
    if (len > available_space) {
        LOGW("fifo_push: not enough space, reallocating");

        const size_t missing_space = len - available_space;
        const size_t new_cap = max(this->cap + missing_space, this->cap * 2);

        this->cap = new_cap;
        this->data = realloc(this->data, this->cap);
        assert(this->data != NULL);
    }

    memcpy(this->data + this->len, data, len);
    this->len += len;
}

void fifo_pop(fifo_t* this, size_t num) {
    assert(num <= this->len);

    if (num != this->len)
        memmove(this->data, this->data + num, this->len - num);

    this->len -= num;
}

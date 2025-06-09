#include "fifo.h"
#include <assert.h>
#include <bt2uart/util/math.h>
#include <stdlib.h>
#include <string.h>

esp_err_t bt2uart_fifo_init(bt2uart_fifo_t* this, size_t initial_cap) {
    if (initial_cap == 0)
        initial_cap = 1;

    this->data = malloc(initial_cap);
    if (this->data == NULL)
        return ESP_ERR_NO_MEM;

    this->cap = initial_cap;
    this->len = 0;

    return ESP_OK;
}

void bt2uart_fifo_clear(bt2uart_fifo_t* this) {
    this->len = 0;
}

void bt2uart_fifo_free(bt2uart_fifo_t* this) {
    free(this->data);
    memset(this, 0, sizeof(bt2uart_fifo_t));
}

void bt2uart_fifo_push(bt2uart_fifo_t* this, const uint8_t* data, size_t len) {
    if (len == 0)
        return;

    const size_t available_space = this->cap - this->len;
    if (len > available_space) {
        const size_t missing_space = len - available_space;
        const size_t new_cap = max(this->cap + missing_space, this->cap * 2);

        this->cap = new_cap;
        this->data = realloc(this->data, this->cap);
        assert(this->data != NULL);
    }

    memcpy(this->data + this->len, data, len);
    this->len += len;
}

void bt2uart_fifo_pop(bt2uart_fifo_t* this, size_t num) {
    if (num == 0)
        return;

    assert(num <= this->len);

    if (num != this->len)
        memmove(this->data, this->data + num, this->len - num);

    this->len -= num;
}

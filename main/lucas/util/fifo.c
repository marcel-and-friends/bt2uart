#include "fifo.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lucas/util/math.h>

bool lucas_fifo_init(lucas_fifo_t* this, size_t initial_cap) {
    this->data = malloc(initial_cap);
    if (this->data == NULL)
        return false;

    this->cap = initial_cap;
    this->len = 0;

    return true;
}

void lucas_fifo_clear(lucas_fifo_t* this) {
    this->len = 0;
}

void lucas_fifo_free(lucas_fifo_t* this) {
    free(this->data);
    memset(this, 0, sizeof(lucas_fifo_t));
}

void lucas_fifo_push(lucas_fifo_t* this, uint8_t* data, size_t len) {
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

void lucas_fifo_pop(lucas_fifo_t* this, size_t num) {
    assert(num <= this->len);

    if (num != this->len)
        memmove(this->data, this->data + num, this->len - num);

    this->len -= num;
}

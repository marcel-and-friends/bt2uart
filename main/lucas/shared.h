#pragma once

#include <stdint.h>

// FIXME: get rid of this somehow
struct lucas_shared_ctx_t {
    uint32_t spp_handle;
} extern g_shared_ctx;

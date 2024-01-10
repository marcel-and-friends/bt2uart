#include <esp_log.h>
#include <lucas/lucas.h>

void app_main(void) {
    if (lucas_init() != ESP_OK)
        ESP_LOGE("MAIN:", "INIT FAILED!");
}

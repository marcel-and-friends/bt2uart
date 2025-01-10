#include <esp_log.h>
#include <bt2uart/bt2uart.h>

void app_main(void) {
    if (bt2uart_init() != ESP_OK)
        ESP_LOGE("MAIN:", "INIT FAILED!");
}

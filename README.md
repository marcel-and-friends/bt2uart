# bt2uart — ESP32 Bluetooth-to-UART Bridge

Firmware para ESP32 WROOM-32U que faz ponte entre UART (STM32/Marlin) e Bluetooth Classic SPP, usado nas máquinas LUCAS.

Fork de [iniw/bt2uart](https://github.com/iniw/bt2uart) com correções de estabilidade de conexão.

## Arquitetura

```
STM32 (neo-lucas) ←UART (TX=17, RX=16, 115200)→ ESP32 (bt2uart) ←BT SPP→ Celular/Tablet (Coffe-App)
```

## Correções aplicadas

### Fix: timeout na fila de eventos (conexão BT caindo)

**Problema:** `bt2uart_event_send()` usava `portMAX_DELAY` no `xQueueSend`. Os callbacks SPP rodam na task BTC do Bluedroid — quando a fila de 20 slots enchia (tráfego pesado de UART), a task BTC travava, o stack BT inteiro parava de responder e a conexão caía por link supervision timeout.

**Sintoma:** conexão BT cai aleatoriamente durante uso da máquina. Trocar o ESP32 "resolve" temporariamente porque o power cycle limpa o estado.

**Fix:** timeout de 50ms para eventos de dados (UART_RECV, SPP_RECV) com drop gracioso, e 500ms para eventos de controle (WRITE_SUCCEEDED, CONGESTION_ENDED, RESET) para evitar estado preso.

## Build

Requer [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/).

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash
```

## Configuração por máquina

Antes de flashar, editar `main/bt2uart/bt.c`:

```c
#define DEVICE_NAME "LUCAS-XX"        /* trocar XX pelo número da máquina */
#define SERVER_NAME "LUCAS-XX-SERVER"
```

PIN de pareamento: `lucas-cafe` (hardcoded em `bt.c`).

## Máquinas já atualizadas (2026-06-18)

- LUCAS-6
- LUCAS-12
- LUCAS-14
- LUCAS-15
- LUCAS-16
- LUCAS-17
- LUCAS-19

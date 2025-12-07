#ifndef TASK_BLUETOOTH_H
#define TASK_BLUETOOTH_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Estados del Bluetooth
typedef enum {
    BT_STATE_DISABLED = 0,
    BT_STATE_ENABLED,
    BT_STATE_DISCOVERABLE,
    BT_STATE_CONNECTED,
    BT_STATE_CONFIGURING
} bluetooth_state_t;

// Estructura para configuración WiFi recibida por Bluetooth
typedef struct {
    char ssid[32];
    char password[64];
    bool received;
} wifi_config_from_bt_t;

// Funciones públicas
esp_err_t bluetooth_init(void);
esp_err_t bluetooth_start_config_mode(void);
esp_err_t bluetooth_stop_config_mode(void);
bluetooth_state_t bluetooth_get_state(void);
bool bluetooth_has_wifi_config(void);
esp_err_t bluetooth_get_wifi_config(char *ssid, char *password);
void bluetooth_clear_wifi_config(void);

// Tarea principal del Bluetooth
void task_bluetooth(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // TASK_BLUETOOTH_H
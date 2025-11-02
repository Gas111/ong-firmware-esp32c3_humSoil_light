#ifndef TASK_NVS_H
#define TASK_NVS_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

// Global variables for NVS handles
extern nvs_handle_t my_handle;
extern nvs_handle_t my_handle_pass;

// Flags for writing
extern bool flag_write_flash;
extern bool flag_write_flash_ssid;
extern bool flag_write_flash_pass;

// Read values
extern char *ssid_readed;
extern char *pass_readed;

// Scan code to store
extern char *scan_code_to_store;

// Functions
void nvs_init();
void nvs_init_pass();
char *nvs_read(char *ssid);
char *nvs_read_pass(char *pass);
void nvs_write(char *ssid);
void nvs_write_pass(char *pass);

// Sensor ID persistence helpers
esp_err_t nvs_save_sensor_id(const char *key, int32_t id);
esp_err_t nvs_get_sensor_id(const char *key, int32_t *out_id);

// Persist a boolean 'registered' flag for sensors/device
esp_err_t nvs_save_registered_flag(const char *key, bool value);
esp_err_t nvs_get_registered_flag(const char *key, bool *out_value);

// Task
void task_nvs_config(void *args);

#endif // TASK_NVS_H
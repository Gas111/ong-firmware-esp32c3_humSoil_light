#include "task_nvs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "task_main.h"
#include <string.h>

static const char *TAG = "NVS_TASK";

// Global variables
nvs_handle_t my_handle;
nvs_handle_t my_handle_pass;

// Flags for writing
bool flag_write_flash = false;
bool flag_write_flash_ssid = false;
bool flag_write_flash_pass = false;

// Read values
char ssid_buffer[32];
char pass_buffer[64];
char *ssid_readed = ssid_buffer;
char *pass_readed = pass_buffer;

// Scan code to store
char scan_code_buffer[64];
char *scan_code_to_store = scan_code_buffer;

// Functions
void nvs_init()
{
    esp_err_t err;
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS storage: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "✓ NVS storage inicializado");
    }
}

void nvs_init_pass()
{
    esp_err_t err;
    err = nvs_open("storage", NVS_READWRITE, &my_handle_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS storage para password: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "✓ NVS storage para password inicializado");
    }
}

char *nvs_read(char *ssid)
{
    esp_err_t err;
    ESP_LOGD(TAG, "Leyendo SSID de NVS...");
    
    size_t size_ssd = sizeof(ssid_buffer);
    err = nvs_get_str(my_handle, "ssd_value", ssid, &size_ssd);
    switch (err)
    {
    case ESP_OK:
        ESP_LOGI(TAG, "SSID leído de NVS: %s", ssid);
        break;
    case ESP_ERR_NVS_NOT_FOUND:
        ESP_LOGW(TAG, "SSID no encontrado en NVS");
        strcpy(ssid, "");
        break;
    default:
        ESP_LOGE(TAG, "Error leyendo SSID de NVS: %s", esp_err_to_name(err));
        strcpy(ssid, "");
    }
    return ssid;
}

char *nvs_read_pass(char *pass)
{
    esp_err_t err;
    ESP_LOGD(TAG, "Leyendo password de NVS...");
    
    size_t size_pass = sizeof(pass_buffer);
    err = nvs_get_str(my_handle_pass, "pass_value", pass, &size_pass);
    switch (err)
    {
    case ESP_OK:
        ESP_LOGI(TAG, "Password leído de NVS");
        break;
    case ESP_ERR_NVS_NOT_FOUND:
        ESP_LOGW(TAG, "Password no encontrado en NVS");
        strcpy(pass, "");
        break;
    default:
        ESP_LOGE(TAG, "Error leyendo password de NVS: %s", esp_err_to_name(err));
        strcpy(pass, "");
    }
    return pass;
}

void nvs_write(char *ssid)
{
    esp_err_t err = nvs_set_str(my_handle, "ssd_value", ssid);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SSID guardado en NVS: %s", ssid);
        nvs_commit(my_handle);
    } else {
        ESP_LOGE(TAG, "Error guardando SSID en NVS: %s", esp_err_to_name(err));
    }
}

void nvs_write_pass(char *pass)
{
    esp_err_t err = nvs_set_str(my_handle_pass, "pass_value", pass);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Password guardado en NVS");
        nvs_commit(my_handle_pass);
    } else {
        ESP_LOGE(TAG, "Error guardando password en NVS: %s", esp_err_to_name(err));
    }
}

// Task
void task_nvs_config(void *args)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA NVS ===");
    
    // Inicializar handles NVS
    nvs_init();
    nvs_init_pass();
    
    // Loop principal de la tarea NVS
    while (1) {
        // Verificar flags de escritura
        if (flag_write_flash_ssid) {
            nvs_write(scan_code_to_store);
            flag_write_flash_ssid = false;
        }
        
        if (flag_write_flash_pass) {
            nvs_write_pass(scan_code_to_store);
            flag_write_flash_pass = false;
        }
        
        // Enviar heartbeat cada 5 minutos
        static uint32_t last_heartbeat = 0;
        uint32_t current_time = xTaskGetTickCount();
        if ((current_time - last_heartbeat) > pdMS_TO_TICKS(300000)) { // 5 minutos
            task_send_heartbeat(TASK_TYPE_NVS, "NVS activo");
            last_heartbeat = current_time;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000)); // Verificar cada segundo
    }
}

// Persist a sensor id (e.g. "temp_id" or "hum_id") into NVS under namespace "sensor_ids"
esp_err_t nvs_save_sensor_id(const char *key, int32_t id)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sensor_ids", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS sensor_ids para escritura: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_i32(handle, key, id);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sensor ID guardado: %s = %ld", key, id);
        } else {
            ESP_LOGE(TAG, "Error committeando sensor ID: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Error guardando sensor ID: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

// Read a persisted sensor id from NVS (namespace "sensor_ids")
esp_err_t nvs_get_sensor_id(const char *key, int32_t *out_id)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sensor_ids", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS sensor_ids para lectura: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_i32(handle, key, out_id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sensor ID leído: %s = %ld", key, *out_id);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Sensor ID no encontrado: %s", key);
        *out_id = 0; // Default value
    } else {
        ESP_LOGE(TAG, "Error leyendo sensor ID: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

// Persist a boolean flag (0/1) under namespace "sensor_ids" indicating registration
esp_err_t nvs_save_registered_flag(const char *key, bool value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sensor_ids", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS sensor_ids para flag: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t flag_value = value ? 1 : 0;
    err = nvs_set_u8(handle, key, flag_value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Flag guardado: %s = %s", key, value ? "true" : "false");
        } else {
            ESP_LOGE(TAG, "Error committeando flag: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Error guardando flag: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

// Read the persisted registered flag
esp_err_t nvs_get_registered_flag(const char *key, bool *out_value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sensor_ids", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS sensor_ids para flag: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t flag_value;
    err = nvs_get_u8(handle, key, &flag_value);
    if (err == ESP_OK) {
        *out_value = (flag_value == 1);
        ESP_LOGI(TAG, "Flag leído: %s = %s", key, *out_value ? "true" : "false");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Flag no encontrado: %s", key);
        *out_value = false; // Default value
    } else {
        ESP_LOGE(TAG, "Error leyendo flag: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}
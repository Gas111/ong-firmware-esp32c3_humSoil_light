#include "task_initial_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_chip_info.h"
#include "config.h"
#include "task_main.h"
#include "esp_system.h"
#include "adc_shared.h"

static const char *TAG = "INIT_CONFIG";

// Verificación básica del ADC (sin inicialización completa)
static esp_err_t adc_basic_check(void)
{
    ESP_LOGI(TAG, "Inicializando ADC compartido...");

    // Inicializar ADC compartido
    esp_err_t ret = init_shared_adc();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando ADC compartido: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configurar canal del sensor de humedad
    ret = configure_adc_channel(SOIL_HUMIDITY_ADC_CHANNEL, ADC_ATTEN_DB_12);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando canal humedad: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configurar canal del sensor de luz
    ret = configure_adc_channel(LIGHT_SENSOR_ADC_CHANNEL, ADC_ATTEN_DB_12);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando canal luz: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✓ ADC compartido inicializado correctamente");
    ESP_LOGI(TAG, "Sensor Humedad - GPIO%d (D0) - ADC_CHANNEL_%d", SOIL_HUMIDITY_GPIO, SOIL_HUMIDITY_ADC_CHANNEL);
    ESP_LOGI(TAG, "Sensor Luz - GPIO%d (D1) - ADC_CHANNEL_%d", LIGHT_SENSOR_GPIO, LIGHT_SENSOR_ADC_CHANNEL);

    // ===== DIAGNÓSTICO DE SENSORES =====
    ESP_LOGI(TAG, "=== DIAGNÓSTICO DE SENSORES ADC ===");

    int raw_value, voltage_mv;

    // Probar sensor de humedad
    ESP_LOGI(TAG, "Probando sensor de humedad (GPIO%d)...", SOIL_HUMIDITY_GPIO);
    ret = read_adc_channel(SOIL_HUMIDITY_ADC_CHANNEL, &raw_value);
    if (ret == ESP_OK) {
        convert_adc_to_voltage(raw_value, &voltage_mv);
        ESP_LOGI(TAG, "Humedad - Raw: %d, Voltaje: %d mV", raw_value, voltage_mv);
    } else {
        ESP_LOGE(TAG, "Error leyendo humedad: %s", esp_err_to_name(ret));
    }

    // Probar sensor de luz
    ESP_LOGI(TAG, "Probando sensor de luz (GPIO%d)...", LIGHT_SENSOR_GPIO);
    ret = read_adc_channel(LIGHT_SENSOR_ADC_CHANNEL, &raw_value);
    if (ret == ESP_OK) {
        convert_adc_to_voltage(raw_value, &voltage_mv);
        ESP_LOGI(TAG, "Luz - Raw: %d, Voltaje: %d mV", raw_value, voltage_mv);
    } else {
        ESP_LOGE(TAG, "Error leyendo luz: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "=== FIN DIAGNÓSTICO ===");

    return ESP_OK;
}

// Verificación de memoria y sistema
static esp_err_t system_check(void)
{
    ESP_LOGI(TAG, "Verificando sistema...");
    
    uint32_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Memoria libre: %lu bytes", free_heap);
    
    if (free_heap < 50000) {
        ESP_LOGW(TAG, "Poca memoria disponible");
        return ESP_ERR_NO_MEM;
    }
    
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "ESP32-C3 - %d núcleos, rev %d", chip_info.cores, chip_info.revision);
    
    return ESP_OK;
}// Tarea de configuración inicial (se ejecuta una vez y termina)
void task_initial_config(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO CONFIGURACION INICIAL ESP32-C3 ===");

    esp_err_t ret;

    // Inicializar NVS
    ESP_LOGI(TAG, "Inicializando NVS Flash...");
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requiere limpieza, borrando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "✓ NVS Flash inicializado correctamente");

    // Verificar sistema básico
    ESP_LOGI(TAG, "Verificando sistema...");
    ret = system_check();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Error en verificación del sistema");
        task_report_error(TASK_TYPE_INITIAL_CONFIG, TASK_ERROR_UNKNOWN, "System check failed");
        // Continuar de todos modos
    } else {
        ESP_LOGI(TAG, "✓ Verificación del sistema exitosa");
    }

    // Pequeña pausa antes de verificar ADC
    vTaskDelay(pdMS_TO_TICKS(100));

    // Verificar configuración ADC (no inicializar completamente)
    ESP_LOGI(TAG, "Verificando configuración ADC...");
    ret = adc_basic_check();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "✗ Error en configuración ADC");
        task_report_error(TASK_TYPE_INITIAL_CONFIG, TASK_ERROR_ADC_INIT_FAILED, "ADC config invalid");
        // No es crítico, el sensor task manejará la inicialización real
    } else {
        ESP_LOGI(TAG, "✓ Configuración ADC verificada");
    }

    // Configuración específica para ESP32-C3
    ESP_LOGI(TAG, "Aplicando configuración específica ESP32-C3...");
    
    // Suprimir algunos logs verbosos
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    
    ESP_LOGI(TAG, "✓ Configuración específica aplicada");

    // Reportar finalización exitosa
    ESP_LOGI(TAG, "=== CONFIGURACION INICIAL COMPLETADA ===");
    ESP_LOGI(TAG, "Sistema: ESP32-C3");
    ESP_LOGI(TAG, "Sensor Humedad: GPIO%d (D0)", SOIL_HUMIDITY_GPIO);
    ESP_LOGI(TAG, "Sensor Luz: GPIO%d (D1)", LIGHT_SENSOR_GPIO);
    ESP_LOGI(TAG, "LED: GPIO%d (D2)", LED_RGB_GPIO);
    ESP_LOGI(TAG, "Memoria libre: %lu bytes", esp_get_free_heap_size());

    // Enviar status al supervisor
    task_send_status(TASK_TYPE_INITIAL_CONFIG, "Configuración inicial completa");

    // Notificar al supervisor que hemos terminado
    xSemaphoreGive(init_config_semaphore);

    ESP_LOGI(TAG, "✓ Tarea de configuración inicial finalizada");

    // Esta tarea se elimina
    vTaskDelete(NULL);
}
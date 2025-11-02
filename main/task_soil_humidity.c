#include "task_soil_humidity.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "task_main.h"
#include "adc_shared.h"
#include "task_sensor_config.h"

static const char *TAG = "SOIL_HUMIDITY";

// Convertir valor ADC a porcentaje de humedad de suelo
static float convert_to_humidity_percent(int raw_value)
{
    float humidity_percent;
    
    if (raw_value >= SOIL_HUMIDITY_DRY_VALUE) {
        humidity_percent = 0.0f;  // Completamente seco
    } else if (raw_value <= SOIL_HUMIDITY_WET_VALUE) {
        humidity_percent = 100.0f; // Completamente húmedo
    } else {
        // Interpolación lineal inversa
        humidity_percent = 100.0f - ((float)(raw_value - SOIL_HUMIDITY_WET_VALUE) * 100.0f / 
                          (float)(SOIL_HUMIDITY_DRY_VALUE - SOIL_HUMIDITY_WET_VALUE));
    }
    
    // Limitar entre 0 y 100%
    if (humidity_percent < 0.0f) humidity_percent = 0.0f;
    if (humidity_percent > 100.0f) humidity_percent = 100.0f;
    
    return humidity_percent;
}

// Inicializar calibración ADC
// Tarea de lectura del sensor de humedad de suelo
void task_soil_humidity_reading(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO SENSOR HUMEDAD SUELO ===");

    QueueHandle_t humidity_queue = (QueueHandle_t)pvParameters;

    if (humidity_queue == NULL) {
        ESP_LOGE(TAG, "Error: Cola es NULL");
        task_report_error(TASK_TYPE_SENSOR, TASK_ERROR_QUEUE_FULL, "Cola NULL");
        vTaskDelete(NULL);
        return;
    }

    // Verificar que el ADC compartido esté inicializado
    if (g_adc_handle == NULL) {
        ESP_LOGE(TAG, "Error: ADC compartido no inicializado");
        task_report_error(TASK_TYPE_SENSOR, TASK_ERROR_HARDWARE, "ADC shared not initialized");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "✓ Sensor inicializado - Lectura cada 5 segundos");
    
    sensor_data_t data;
    uint32_t read_count = 0;
    uint32_t send_count = 0;
    int raw_value, voltage_mv;
    
    // Esperar a que la configuración esté lista
    while (!g_sensor_humidity_config.config_loaded) {
        ESP_LOGI(TAG, "Esperando configuración del sensor de humedad...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "✓ Configuración cargada - Intervalo de envío: %d segundos", g_sensor_humidity_config.interval_s);
    
    while (1) {
        read_count++;
        
        // Leer ADC usando funciones compartidas
        esp_err_t ret = read_adc_channel(SOIL_HUMIDITY_ADC_CHANNEL, &raw_value);
        
        if (ret == ESP_OK) {
            // Convertir a voltaje usando función compartida
            ret = convert_adc_to_voltage(raw_value, &voltage_mv);
            if (ret != ESP_OK) {
                voltage_mv = raw_value; // Fallback
            }
            
            // Llenar estructura sensor_data_t
            data.type = SENSOR_TYPE_SOIL_HUMIDITY;
            data.raw_value = raw_value;
            data.adc_voltage = (float)voltage_mv;
            data.converted_value = convert_to_humidity_percent(raw_value); // HS%
            data.timestamp = xTaskGetTickCount();
            data.valid = true;
            
            // SIEMPRE mostrar logs cada 5 segundos
            ESP_LOGI(TAG, "💧 Lectura #%lu: Raw=%d, Voltaje=%.0f mV, HS=%.1f%%", 
                     (unsigned long)read_count, data.raw_value, data.adc_voltage, data.converted_value);
            
            // Solo enviar al servidor según el intervalo configurado
            if (g_sensor_humidity_config.state && (read_count % (g_sensor_humidity_config.interval_s / 5)) == 0) {
                send_count++;
                ESP_LOGI(TAG, "📤 Enviando datos #%lu al servidor (cada %d segundos)", 
                        (unsigned long)send_count, g_sensor_humidity_config.interval_s);
                
                // Enviar a cola (reemplazar si está llena)
                if (xQueueSend(humidity_queue, &data, 0) != pdTRUE) {
                    sensor_data_t dummy;
                    xQueueReceive(humidity_queue, &dummy, 0);
                    xQueueSend(humidity_queue, &data, 0);
                }
            }
            
            task_send_heartbeat(TASK_TYPE_SENSOR, "Humedad OK");
            
        } else {
            ESP_LOGE(TAG, "Error leyendo ADC: %s", esp_err_to_name(ret));
            task_report_error(TASK_TYPE_SENSOR, TASK_ERROR_SENSOR_READ, "ADC read failed");
        }
        
        // Leer cada 5 segundos
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

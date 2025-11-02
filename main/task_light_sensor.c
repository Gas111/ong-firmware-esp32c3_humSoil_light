#include "task_light_sensor.h"
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
#include <math.h>

static const char *TAG = "LIGHT_SENSOR";

// Convertir valor ADC a porcentaje de luminosidad (0-100%)
static float convert_to_light_percentage(int raw_value)
{
    float percentage;

    if (raw_value <= LIGHT_SENSOR_DARK_VALUE) {
        percentage = 100.0f; // Máxima luminosidad (valores bajos = mucha luz)
    } else if (raw_value >= LIGHT_SENSOR_BRIGHT_VALUE) {
        percentage = 0.0f;   // Oscuridad total (valores altos = poca luz)
    } else {
        // Interpolación lineal invertida: valores bajos = más luz (100%), valores altos = menos luz (0%)
        percentage = 100.0f - ((float)(raw_value - LIGHT_SENSOR_DARK_VALUE) * 100.0f) /
                    (float)(LIGHT_SENSOR_BRIGHT_VALUE - LIGHT_SENSOR_DARK_VALUE);
    }

    // Limitar entre 0% y 100%
    if (percentage < 0.0f) percentage = 0.0f;
    if (percentage > 100.0f) percentage = 100.0f;

    return percentage;
}

// Inicializar calibración ADC
// Tarea de lectura del sensor de luz
void task_light_sensor_reading(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO SENSOR DE LUZ ===");

    QueueHandle_t light_queue = (QueueHandle_t)pvParameters;

    if (light_queue == NULL) {
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
    while (!g_sensor_light_config.config_loaded) {
        ESP_LOGI(TAG, "Esperando configuración del sensor de luz...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "✓ Configuración cargada - Intervalo de envío: %d segundos", g_sensor_light_config.interval_s);
    
    while (1) {
        read_count++;
        
        // Leer ADC usando funciones compartidas con reintentos
        esp_err_t ret = ESP_FAIL;
        int retry_count = 0;
        const int max_retries = 3;
        
        while (retry_count < max_retries && ret != ESP_OK) {
            ret = read_adc_channel(LIGHT_SENSOR_ADC_CHANNEL, &raw_value);
            if (ret != ESP_OK) {
                retry_count++;
                ESP_LOGW(TAG, "Reintento %d/%d leyendo ADC canal %d: %s", 
                        retry_count, max_retries, LIGHT_SENSOR_ADC_CHANNEL, esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(100)); // Pequeña pausa entre reintentos
            }
        }
        
        if (ret == ESP_OK) {
            // Convertir a voltaje usando función compartida
            ret = convert_adc_to_voltage(raw_value, &voltage_mv);
            if (ret != ESP_OK) {
                voltage_mv = raw_value; // Fallback
            }
            
            // Llenar estructura sensor_data_t
            data.type = SENSOR_TYPE_LIGHT;
            data.raw_value = raw_value;
            data.adc_voltage = (float)voltage_mv;
            data.converted_value = convert_to_light_percentage(raw_value); // porcentaje LM%
            data.timestamp = xTaskGetTickCount();
            data.valid = true;
            
            // SIEMPRE mostrar logs cada 5 segundos
            ESP_LOGI(TAG, "💡 Lectura #%lu: Raw=%d, Voltaje=%.0f mV, Luz=%.0f LM%%", 
                     (unsigned long)read_count, data.raw_value, data.adc_voltage, data.converted_value);
            
            // Solo enviar al servidor según el intervalo configurado
            if (g_sensor_light_config.state && (read_count % (g_sensor_light_config.interval_s / 5)) == 0) {
                send_count++;
                ESP_LOGI(TAG, "📤 Enviando datos #%lu al servidor (cada %d segundos)", 
                        (unsigned long)send_count, g_sensor_light_config.interval_s);
                
                // Enviar a cola (reemplazar si está llena)
                if (xQueueSend(light_queue, &data, 0) != pdTRUE) {
                    sensor_data_t dummy;
                    xQueueReceive(light_queue, &dummy, 0);
                    xQueueSend(light_queue, &data, 0);
                }
            }
            
            task_send_heartbeat(TASK_TYPE_SENSOR, "Luz OK");
            
        } else {
            ESP_LOGE(TAG, "Error leyendo ADC después de %d reintentos: %s", max_retries, esp_err_to_name(ret));
            task_report_error(TASK_TYPE_SENSOR, TASK_ERROR_SENSOR_READ, "ADC read failed");
            
            // Enviar datos con valores por defecto para mantener la funcionalidad
            ESP_LOGW(TAG, "⚠ Usando valores por defecto para sensor de luz");
            data.type = SENSOR_TYPE_LIGHT;
            data.raw_value = 0;
            data.adc_voltage = 0.0f;
            data.converted_value = 0.0f; // 0% (oscuridad)
            data.timestamp = xTaskGetTickCount();
            data.valid = false; // Marcar como inválido pero enviar de todos modos
            
            // Solo enviar al servidor según el intervalo configurado (datos inválidos)
            if (g_sensor_light_config.state && (read_count % (g_sensor_light_config.interval_s / 5)) == 0) {
                send_count++;
                ESP_LOGW(TAG, "📤 Enviando datos inválidos #%lu al servidor (ADC falló)", 
                        (unsigned long)send_count);
                
                // Enviar a cola (reemplazar si está llena)
                if (xQueueSend(light_queue, &data, 0) != pdTRUE) {
                    sensor_data_t dummy;
                    xQueueReceive(light_queue, &dummy, 0);
                    xQueueSend(light_queue, &data, 0);
                }
            }
        }
        
        // Leer cada 5 segundos
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

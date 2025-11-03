#include "task_sensor_reader.h"
#include "esp_log.h"
#include "adc_shared.h"
#include "config.h"
#include "task_main.h"
#include "task_sensor_config.h"
#include "task_sensor.h"
#include <string.h>

static const char *TAG = "SENSOR_READER";

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

/**
 * @brief Tarea unificada de lectura de todos los sensores
 * 
 * Lee TODOS los sensores cada SENSOR_READING_INTERVAL_MS (5 segundos)
 * Envía datos al servidor según el interval_seconds de cada sensor
 */
void task_sensor_reader(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA UNIFICADA DE LECTURA DE SENSORES ===");
    
    // Recibir estructura con ambas colas de configuración
    struct {
        QueueHandle_t sensor_data_queue;
        QueueHandle_t humidity_config_queue;
        QueueHandle_t light_config_queue;
    } *queues = (typeof(queues))pvParameters;
    
    QueueHandle_t sensor_queue = queues->sensor_data_queue;
    QueueHandle_t humidity_config_queue = queues->humidity_config_queue;
    QueueHandle_t light_config_queue = queues->light_config_queue;

    if (sensor_queue == NULL || humidity_config_queue == NULL || light_config_queue == NULL) {
        ESP_LOGE(TAG, "Error: Una o más colas son NULL");
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
    
    ESP_LOGI(TAG, "✓ Sensores inicializados");
    ESP_LOGI(TAG, "  - Sensor Humedad: GPIO%d (Canal ADC %d)", SOIL_HUMIDITY_GPIO, SOIL_HUMIDITY_ADC_CHANNEL);
    ESP_LOGI(TAG, "  - Sensor Luz: GPIO%d (Canal ADC %d)", LIGHT_SENSOR_GPIO, LIGHT_SENSOR_ADC_CHANNEL);
    ESP_LOGI(TAG, "  - Frecuencia de lectura: cada %d ms", SENSOR_READING_INTERVAL_MS);
    
    // Esperar a que las configuraciones estén listas
    while (!g_sensor_humidity_config.config_loaded || !g_sensor_light_config.config_loaded) {
        ESP_LOGI(TAG, "Esperando configuración de sensores...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Intervalos dinámicos de envío (en segundos)
    int humidity_interval_s = g_sensor_humidity_config.interval_s;
    int light_interval_s = g_sensor_light_config.interval_s;
    
    ESP_LOGI(TAG, "✓ Configuración inicial:");
    ESP_LOGI(TAG, "  - Humedad: envío cada %d segundos", humidity_interval_s);
    ESP_LOGI(TAG, "  - Luz: envío cada %d segundos", light_interval_s);
    
    // Contadores de ciclos de lectura
    uint32_t read_cycle = 0;
    
    // Contadores para envío de datos
    uint32_t humidity_send_count = 0;
    uint32_t light_send_count = 0;
    
    sensor_data_t data;
    int raw_value, voltage_mv;
    
    while (1) {
        read_cycle++;
        
        // ===== VERIFICAR ACTUALIZACIONES DE CONFIGURACIÓN =====
        config_update_message_t config_msg;
        
        // Verificar actualización de humedad
        if (xQueueReceive(humidity_config_queue, &config_msg, 0) == pdTRUE) {
            if (config_msg.update_interval && config_msg.type == SENSOR_TYPE_SOIL_HUMIDITY) {
                humidity_interval_s = config_msg.new_interval_s;
                ESP_LOGI(TAG, "🔄 Intervalo de HUMEDAD actualizado dinámicamente a %d segundos", humidity_interval_s);
            }
        }
        
        // Verificar actualización de luz
        if (xQueueReceive(light_config_queue, &config_msg, 0) == pdTRUE) {
            if (config_msg.update_interval && config_msg.type == SENSOR_TYPE_LIGHT) {
                light_interval_s = config_msg.new_interval_s;
                ESP_LOGI(TAG, "🔄 Intervalo de LUZ actualizado dinámicamente a %d segundos", light_interval_s);
            }
        }
        
        // ===== LEER SENSOR DE HUMEDAD =====
        esp_err_t ret = read_adc_channel(SOIL_HUMIDITY_ADC_CHANNEL, &raw_value);
        
        if (ret == ESP_OK) {
            ret = convert_adc_to_voltage(raw_value, &voltage_mv);
            if (ret != ESP_OK) {
                voltage_mv = raw_value; // Fallback
            }
            
            // Llenar estructura sensor_data_t
            data.type = SENSOR_TYPE_SOIL_HUMIDITY;
            data.raw_value = raw_value;
            data.adc_voltage = (float)voltage_mv;
            data.converted_value = convert_to_humidity_percent(raw_value);
            data.timestamp = xTaskGetTickCount();
            data.valid = true;
            
            // Mostrar lectura cada 5 segundos
            ESP_LOGI(TAG, "💧 Humedad #%lu: Raw=%d, V=%.0f mV, HS=%.1f%%", 
                     (unsigned long)read_cycle, data.raw_value, data.adc_voltage, data.converted_value);
            
            // Enviar al servidor según el intervalo configurado
            if (g_sensor_humidity_config.state && (read_cycle % (humidity_interval_s / 5)) == 0) {
                humidity_send_count++;
                ESP_LOGI(TAG, "📤 Enviando humedad #%lu al servidor (intervalo: %d seg)", 
                        (unsigned long)humidity_send_count, humidity_interval_s);
                
                // Enviar a cola (reemplazar si está llena)
                if (xQueueSend(sensor_queue, &data, 0) != pdTRUE) {
                    sensor_data_t dummy;
                    xQueueReceive(sensor_queue, &dummy, 0);
                    xQueueSend(sensor_queue, &data, 0);
                }
            }
        } else {
            ESP_LOGE(TAG, "Error leyendo sensor humedad: %s", esp_err_to_name(ret));
        }
        
        // ===== LEER SENSOR DE LUZ =====
        ret = read_adc_channel(LIGHT_SENSOR_ADC_CHANNEL, &raw_value);
        
        if (ret == ESP_OK) {
            ret = convert_adc_to_voltage(raw_value, &voltage_mv);
            if (ret != ESP_OK) {
                voltage_mv = raw_value; // Fallback
            }
            
            // Llenar estructura sensor_data_t
            data.type = SENSOR_TYPE_LIGHT;
            data.raw_value = raw_value;
            data.adc_voltage = (float)voltage_mv;
            data.converted_value = convert_to_light_percentage(raw_value);
            data.timestamp = xTaskGetTickCount();
            data.valid = true;
            
            // Mostrar lectura cada 5 segundos
            ESP_LOGI(TAG, "💡 Luz #%lu: Raw=%d, V=%.0f mV, LM=%.0f%%", 
                     (unsigned long)read_cycle, data.raw_value, data.adc_voltage, data.converted_value);
            
            // Enviar al servidor según el intervalo configurado
            if (g_sensor_light_config.state && (read_cycle % (light_interval_s / 5)) == 0) {
                light_send_count++;
                ESP_LOGI(TAG, "📤 Enviando luz #%lu al servidor (intervalo: %d seg)", 
                        (unsigned long)light_send_count, light_interval_s);
                
                // Enviar a cola (reemplazar si está llena)
                if (xQueueSend(sensor_queue, &data, 0) != pdTRUE) {
                    sensor_data_t dummy;
                    xQueueReceive(sensor_queue, &dummy, 0);
                    xQueueSend(sensor_queue, &data, 0);
                }
            }
        } else {
            ESP_LOGE(TAG, "Error leyendo sensor luz: %s", esp_err_to_name(ret));
        }
        
        // Enviar heartbeat
        task_send_heartbeat(TASK_TYPE_SENSOR, "Sensores OK");
        
        // Esperar intervalo de lectura fijo (5 segundos)
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READING_INTERVAL_MS));
    }
}

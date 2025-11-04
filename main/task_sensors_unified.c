#include "task_sensors_unified.h"
#include "esp_log.h"
#include "config.h"
#include "task_main.h"
#include "adc_shared.h"
#include "task_sensor_config.h"
#include "task_sensor.h"
#include "task_error_logger.h"

static const char *TAG = "SENSORS_UNIFIED";

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

// Tarea unificada de lectura de sensores
void task_sensors_unified_reading(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA UNIFICADA DE SENSORES ===");
    
    QueueHandle_t sensor_queue = (QueueHandle_t)pvParameters;
    
    if (sensor_queue == NULL) {
        ESP_LOGE(TAG, "Error: Cola de sensores es NULL");
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
    
    // Esperar a que las configuraciones estén listas
    while (!g_sensor_humidity_config.config_loaded || !g_sensor_light_config.config_loaded) {
        ESP_LOGI(TAG, "Esperando configuración de sensores...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "✓ Configuraciones cargadas:");
    ESP_LOGI(TAG, "  - Humedad: ID=%d, Intervalo=%ds", 
             g_sensor_humidity_config.id_sensor, g_sensor_humidity_config.interval_s);
    ESP_LOGI(TAG, "  - Luz: ID=%d, Intervalo=%ds", 
             g_sensor_light_config.id_sensor, g_sensor_light_config.interval_s);
    ESP_LOGI(TAG, "✓ Lectura cada %d ms", SENSOR_READING_INTERVAL_MS);
    
    uint32_t read_count = 0;
    sensor_data_t data;
    int raw_value, voltage_mv;
    
    while (1) {
        read_count++;
        
        ESP_LOGI(TAG, "📖 Ciclo de lectura #%lu", (unsigned long)read_count);
        
        // ========== LEER SENSOR DE HUMEDAD ==========
        if (g_sensor_humidity_config.state) {
            esp_err_t ret = read_adc_channel(SOIL_HUMIDITY_ADC_CHANNEL, &raw_value);
            
            if (ret == ESP_OK) {
                ret = convert_adc_to_voltage(raw_value, &voltage_mv);
                if (ret != ESP_OK) {
                    voltage_mv = raw_value; // Fallback
                }
                
                data.type = SENSOR_TYPE_SOIL_HUMIDITY;
                data.raw_value = raw_value;
                data.adc_voltage = (float)voltage_mv;
                data.converted_value = convert_to_humidity_percent(raw_value);
                data.timestamp = xTaskGetTickCount();
                data.valid = true;
                
                ESP_LOGI(TAG, "💧 Humedad: %.1f%% (Raw=%d, V=%.0fmV)", 
                         data.converted_value, data.raw_value, data.adc_voltage);
                
                // Enviar a cola (reemplazar si está llena)
                if (xQueueSend(sensor_queue, &data, 0) != pdTRUE) {
                    sensor_data_t dummy;
                    xQueueReceive(sensor_queue, &dummy, 0);
                    if (xQueueSend(sensor_queue, &data, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "⚠ No se pudo enviar datos de humedad a cola");
                    }
                }
            } else {
                ESP_LOGE(TAG, "❌ Error leyendo sensor de humedad: %s", esp_err_to_name(ret));
                task_report_error(TASK_TYPE_SENSOR, TASK_ERROR_SENSOR_READ, "Humidity read failed");
                send_led_status(SYSTEM_STATE_ERROR, "Error sensor humedad");
                
                // Registrar error en el sistema de logs
                char details[256];
                snprintf(details, sizeof(details), 
                         "{\"error_esp\": \"%s\", \"sensor_type\": \"humidity\", \"attempts\": 1}",
                         esp_err_to_name(ret));
                error_logger_log_sensor(
                    g_sensor_humidity_config.id_sensor,
                    "SENSOR_READ_ERROR",
                    ERROR_SEVERITY_ERROR,
                    "Fallo al leer sensor de humedad",
                    details,
                    DEVICE_SERIAL_HUMIDITY
                );
            }
        } else {
            ESP_LOGD(TAG, "⏸ Sensor de humedad deshabilitado");
        }
        
        // ========== LEER SENSOR DE LUZ ==========
        if (g_sensor_light_config.state) {
            esp_err_t ret = read_adc_channel(LIGHT_SENSOR_ADC_CHANNEL, &raw_value);
            
            if (ret == ESP_OK) {
                ret = convert_adc_to_voltage(raw_value, &voltage_mv);
                if (ret != ESP_OK) {
                    voltage_mv = raw_value; // Fallback
                }
                
                data.type = SENSOR_TYPE_LIGHT;
                data.raw_value = raw_value;
                data.adc_voltage = (float)voltage_mv;
                data.converted_value = convert_to_light_percentage(raw_value);
                data.timestamp = xTaskGetTickCount();
                data.valid = true;
                
                ESP_LOGI(TAG, "💡 Luz: %.0f LM%% (Raw=%d, V=%.0fmV)", 
                         data.converted_value, data.raw_value, data.adc_voltage);
                
                // Enviar a cola (reemplazar si está llena)
                if (xQueueSend(sensor_queue, &data, 0) != pdTRUE) {
                    sensor_data_t dummy;
                    xQueueReceive(sensor_queue, &dummy, 0);
                    if (xQueueSend(sensor_queue, &data, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "⚠ No se pudo enviar datos de luz a cola");
                    }
                }
            } else {
                ESP_LOGE(TAG, "❌ Error leyendo sensor de luz: %s", esp_err_to_name(ret));
                task_report_error(TASK_TYPE_SENSOR, TASK_ERROR_SENSOR_READ, "Light read failed");
                send_led_status(SYSTEM_STATE_ERROR, "Error sensor luz");
                
                // Registrar error en el sistema de logs
                char details[256];
                snprintf(details, sizeof(details), 
                         "{\"error_esp\": \"%s\", \"sensor_type\": \"light\", \"attempts\": 1}",
                         esp_err_to_name(ret));
                error_logger_log_sensor(
                    g_sensor_light_config.id_sensor,
                    "SENSOR_READ_ERROR",
                    ERROR_SEVERITY_ERROR,
                    "Fallo al leer sensor de luz",
                    details,
                    DEVICE_SERIAL_LIGHT
                );
            }
        } else {
            ESP_LOGD(TAG, "⏸ Sensor de luz deshabilitado");
        }
        
        // Enviar heartbeat
        task_send_heartbeat(TASK_TYPE_SENSOR, "Sensores OK");
        
        // Esperar intervalo de lectura fijo (5 segundos)
        ESP_LOGD(TAG, "⏳ Esperando %d ms hasta próxima lectura", SENSOR_READING_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READING_INTERVAL_MS));
    }
}

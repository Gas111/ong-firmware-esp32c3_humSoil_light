#include "task_sensor.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "config.h"
#include "task_main.h"

static const char *TAG = "SENSOR_TASK";

// Handles para ADC oneshot y calibración
static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc1_cali_handle = NULL;
static bool do_calibration = false;

// Inicialización del ADC oneshot
static esp_err_t adc_oneshot_init(void)
{
    ESP_LOGI(TAG, "Inicializando ADC oneshot...");
    
    // Configuración de la unidad ADC
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config1, &adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando unidad ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configuración del canal ADC
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH,
        .atten = ADC_ATTEN,
    };
    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando canal ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✓ ADC oneshot inicializado - GPIO%d, Canal %d", ADC_GPIO_PIN, ADC_CHANNEL);
    return ESP_OK;
}

// Inicialización de la calibración del ADC
static esp_err_t adc_calibration_init(void)
{
    ESP_LOGI(TAG, "Inicializando calibración ADC...");
    
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "Intentando calibración por curve fitting...");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT,
            .chan = ADC_CHANNEL,
            .atten = ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle);
        if (ret == ESP_OK) {
            calibrated = true;
            ESP_LOGI(TAG, "✓ Calibración curve fitting establecida");
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "Intentando calibración por line fitting...");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT,
            .atten = ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc1_cali_handle);
        if (ret == ESP_OK) {
            calibrated = true;
            ESP_LOGI(TAG, "✓ Calibración line fitting establecida");
        }
    }
#endif

    if (calibrated) {
        do_calibration = true;
        ESP_LOGI(TAG, "✓ Calibración ADC exitosa");
    } else {
        ESP_LOGW(TAG, "⚠ Calibración ADC no disponible, usando valores crudos");
        do_calibration = false;
    }

    return ret;
}

// Lectura del ADC oneshot
static esp_err_t adc_read_value(int *raw_value, float *voltage_mv)
{
    if (adc1_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Leer valor crudo del ADC
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CHANNEL, raw_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error leyendo ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    // Convertir a voltaje si hay calibración disponible
    if (do_calibration && adc1_cali_handle != NULL) {
        int voltage_mv_int;
        ret = adc_cali_raw_to_voltage(adc1_cali_handle, *raw_value, &voltage_mv_int);
        if (ret == ESP_OK) {
            *voltage_mv = (float)voltage_mv_int;
        } else {
            ESP_LOGW(TAG, "Error en calibración, usando conversión lineal");
            // Conversión lineal aproximada para ESP32-C3 con atenuación 11dB
            *voltage_mv = (*raw_value * 3300.0f) / 4095.0f;
        }
    } else {
        // Conversión lineal aproximada para ESP32-C3 con atenuación 11dB
        *voltage_mv = (*raw_value * 3300.0f) / 4095.0f;
    }

    return ESP_OK;
}

// Tarea de lectura de sensores (ejecuta continuamente)
void task_sensor_reading(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA DE SENSORES ADC ===");

    // Inicializar ADC oneshot
    esp_err_t ret = adc_oneshot_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando ADC oneshot");
        task_report_error(TASK_TYPE_SENSOR, TASK_ERROR_ADC_INIT_FAILED, "ADC oneshot init failed");
        vTaskDelete(NULL);
        return;
    }

    // Inicializar calibración ADC (opcional)
    adc_calibration_init();

    ESP_LOGI(TAG, "Intervalo inicial de muestreo: %lu ms", sensor_sampling_interval_ms);
    ESP_LOGI(TAG, "ADC configurado en GPIO%d, Canal %d", ADC_GPIO_PIN, ADC_CHANNEL);
    ESP_LOGI(TAG, "Atenuación: %d, Bits: %d", ADC_ATTEN, ADC_BITWIDTH);
    
    QueueHandle_t sensor_queue = (QueueHandle_t)pvParameters;
    sensor_data_t sensor_data;
    
    uint32_t reading_count = 0;
    uint32_t error_count = 0;
    uint32_t last_stats_report = xTaskGetTickCount();
    
    while (1) {
        reading_count++;
        ESP_LOGI(TAG, "🔍 Iniciando lectura ADC #%lu desde GPIO%d...", reading_count, ADC_GPIO_PIN);
        
        // Leer ADC oneshot
        int raw_value = 0;
        float voltage_mv = 0.0f;
        
        ret = adc_read_value(&raw_value, &voltage_mv);
        if (ret == ESP_OK) {
            // Preparar datos del sensor
            sensor_data.adc_voltage = voltage_mv;
            sensor_data.raw_value = raw_value;
            sensor_data.timestamp = xTaskGetTickCount();
            sensor_data.valid = true;

            // Validar rango (opcional)
            if (voltage_mv >= sensor_adc_min && voltage_mv <= sensor_adc_max) {
                sensor_data.valid = true;
                
                // Enviar datos a la cola
                if (sensor_queue != NULL) {
                    if (xQueueSend(sensor_queue, &sensor_data, pdMS_TO_TICKS(100)) != pdTRUE) {
                        ESP_LOGW(TAG, "Cola de sensores llena, descartando lectura");
                        error_count++;
                    } else {
                        // Actualizar estado del LED
                        send_led_status(SYSTEM_STATE_SENSOR_READ, "Leyendo ADC");
                    }
                } else {
                    ESP_LOGW(TAG, "Cola de sensores no disponible");
                    error_count++;
                }
                
                // SIEMPRE mostrar lectura, independientemente de la cola
                ESP_LOGI(TAG, "📊 ADC LECTURA - Raw: %d, Voltaje: %.2f mV, Válido: %s", 
                        raw_value, voltage_mv, sensor_data.valid ? "SÍ" : "NO");
            } else {
                ESP_LOGW(TAG, "Lectura ADC fuera de rango: %.2f mV", voltage_mv);
                error_count++;
                sensor_data.valid = false;
            }
        } else {
            ESP_LOGE(TAG, "Error leyendo ADC: %s", esp_err_to_name(ret));
            error_count++;
            sensor_data.valid = false;
        }

        // Reportar estadísticas cada 60 segundos
        uint32_t current_time = xTaskGetTickCount();
        if ((current_time - last_stats_report) > pdMS_TO_TICKS(60000)) {
            float error_rate = error_count > 0 ? (error_count * 100.0f) / reading_count : 0.0f;
            ESP_LOGI(TAG, "📊 Estadísticas ADC - Lecturas: %lu, Errores: %lu (%.1f%%)", 
                    reading_count, error_count, error_rate);
            
            // Enviar heartbeat al supervisor
            char heartbeat_msg[32];
            snprintf(heartbeat_msg, sizeof(heartbeat_msg), "ADC OK %.1fmV", voltage_mv);
            task_send_heartbeat(TASK_TYPE_SENSOR, heartbeat_msg);
            
            last_stats_report = current_time;
            
            // Reset contadores si hay demasiados errores
            if (error_rate > 50.0f) {
                ESP_LOGW(TAG, "⚠ Alta tasa de errores ADC, reiniciando contadores");
                reading_count = 0;
                error_count = 0;
            }
        }

        // Esperar intervalo de muestreo
        vTaskDelay(pdMS_TO_TICKS(sensor_sampling_interval_ms));
    }
}
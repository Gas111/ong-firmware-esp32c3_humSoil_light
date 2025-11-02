#include "task_http.h"
#include "task_main.h"
#include "task_sensor.h"
#include "task_sensor_config.h"
#include "task_led_status.h"
#include "config.h"
#include "esp_log.h"
#include "task_nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "led_strip.h"
#include "cJSON.h"
#include <string.h>
#include <math.h>

static const char *TAG = "HTTP_TASK";
static int consecutive_failures = 0;

// Buffer global para almacenar respuesta HTTP
static char response_buffer[1024];
static int response_len = 0;

// Callback para manejar la respuesta HTTP
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client)) {
            // Capturar respuesta en buffer global
            if (evt->data_len > 0 && response_len + evt->data_len < sizeof(response_buffer) - 1) {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
                response_buffer[response_len] = '\0';
                ESP_LOGD(TAG, "Respuesta HTTP capturada: %.*s", evt->data_len, (char*)evt->data);
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

// Función para procesar respuesta del servidor y actualizar configuración
static esp_err_t process_server_response(sensor_type_t sensor_type)
{
    if (response_len == 0) {
        ESP_LOGD(TAG, "No hay respuesta del servidor");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "📥 Procesando respuesta del servidor: %s", response_buffer);

    // Parsear JSON de respuesta
    cJSON *json = cJSON_Parse(response_buffer);
    if (json == NULL) {
        ESP_LOGW(TAG, "⚠ Respuesta no es JSON válido");
        return ESP_FAIL;
    }

    // Buscar objeto sensorConfig en la respuesta
    cJSON *sensor_config = cJSON_GetObjectItem(json, "sensorConfig");
    if (sensor_config != NULL && cJSON_IsObject(sensor_config)) {
        ESP_LOGI(TAG, "🔧 Procesando configuración del sensor desde respuesta del servidor");

        // Extraer interval_seconds del sensorConfig
        cJSON *interval_item = cJSON_GetObjectItem(sensor_config, "interval_seconds");
        if (cJSON_IsNumber(interval_item)) {
            int new_interval = interval_item->valueint;
            if (new_interval > 0) {
                // Actualizar intervalo global
                if (new_interval != (sensor_post_interval_ms / 1000)) {
                    sensor_post_interval_ms = new_interval * 1000;
                    ESP_LOGI(TAG, "🔄 Intervalo de posting actualizado desde servidor: %d segundos (%lu ms)", 
                            new_interval, sensor_post_interval_ms);
                }
                
                // Actualizar configuración específica del sensor
                extern sensor_config_t g_sensor_humidity_config;
                extern sensor_config_t g_sensor_light_config;
                
                switch (sensor_type) {
                    case SENSOR_TYPE_SOIL_HUMIDITY:
                        if (g_sensor_humidity_config.interval_s != new_interval) {
                            g_sensor_humidity_config.interval_s = new_interval;
                            ESP_LOGI(TAG, "💧 Intervalo sensor humedad actualizado: %d segundos", new_interval);
                        }
                        break;
                    case SENSOR_TYPE_LIGHT:
                        if (g_sensor_light_config.interval_s != new_interval) {
                            g_sensor_light_config.interval_s = new_interval;
                            ESP_LOGI(TAG, "💡 Intervalo sensor luz actualizado: %d segundos", new_interval);
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        // Extraer id_sensor del sensorConfig
        cJSON *id_item = cJSON_GetObjectItem(sensor_config, "id_sensor");
        if (cJSON_IsNumber(id_item)) {
            int server_id_sensor = id_item->valueint;
            ESP_LOGI(TAG, "🆔 ID sensor recibido del servidor: %d", server_id_sensor);
            
            // Actualizar ID del sensor correspondiente
            extern sensor_config_t g_sensor_humidity_config;
            extern sensor_config_t g_sensor_light_config;
            
            switch (sensor_type) {
                case SENSOR_TYPE_SOIL_HUMIDITY:
                    if (g_sensor_humidity_config.id_sensor != server_id_sensor) {
                        g_sensor_humidity_config.id_sensor = server_id_sensor;
                        ESP_LOGI(TAG, "💧 ID sensor humedad actualizado: %d", server_id_sensor);
                    }
                    break;
                case SENSOR_TYPE_LIGHT:
                    if (g_sensor_light_config.id_sensor != server_id_sensor) {
                        g_sensor_light_config.id_sensor = server_id_sensor;
                        ESP_LOGI(TAG, "💡 ID sensor luz actualizado: %d", server_id_sensor);
                    }
                    break;
                default:
                    break;
            }
        }

        // Extraer state del sensorConfig
        cJSON *state_item = cJSON_GetObjectItem(sensor_config, "state");
        if (cJSON_IsBool(state_item)) {
            bool sensor_state = cJSON_IsTrue(state_item);
            ESP_LOGI(TAG, "📊 Estado del sensor recibido del servidor: %s", sensor_state ? "activo" : "inactivo");
            
            // Actualizar estado del sensor correspondiente
            extern sensor_config_t g_sensor_humidity_config;
            extern sensor_config_t g_sensor_light_config;
            
            switch (sensor_type) {
                case SENSOR_TYPE_SOIL_HUMIDITY:
                    if (g_sensor_humidity_config.state != sensor_state) {
                        g_sensor_humidity_config.state = sensor_state;
                        ESP_LOGI(TAG, "💧 Estado sensor humedad actualizado: %s", sensor_state ? "activo" : "inactivo");
                    }
                    break;
                case SENSOR_TYPE_LIGHT:
                    if (g_sensor_light_config.state != sensor_state) {
                        g_sensor_light_config.state = sensor_state;
                        ESP_LOGI(TAG, "💡 Estado sensor luz actualizado: %s", sensor_state ? "activo" : "inactivo");
                    }
                    break;
                default:
                    break;
            }
        }

    } else {
        ESP_LOGD(TAG, "ℹ No se encontró objeto sensorConfig en la respuesta");
        
        // Fallback: buscar interval_seconds directamente en el root (compatibilidad hacia atrás)
        cJSON *interval_item = cJSON_GetObjectItem(json, "interval_seconds");
        if (cJSON_IsNumber(interval_item)) {
            int new_interval = interval_item->valueint;
            if (new_interval > 0 && new_interval != (sensor_post_interval_ms / 1000)) {
                sensor_post_interval_ms = new_interval * 1000;
                ESP_LOGI(TAG, "🔄 Intervalo de posting actualizado (fallback): %d segundos (%lu ms)", 
                        new_interval, sensor_post_interval_ms);
            }
        }
    }

    cJSON_Delete(json);
    return ESP_OK;
}

// Función genérica para enviar datos de sensor al servidor
static esp_err_t send_sensor_value(const sensor_data_t *sensor_data, int id_sensor, const char *device_serial)
{
    // Determinar valor, unidad y tipo según el tipo de sensor
    float value_to_send;
    const char *unit;
    const char *sensor_type;

    switch (sensor_data->type) {
        case SENSOR_TYPE_SOIL_HUMIDITY:
            value_to_send = sensor_data->converted_value; // HS%
            unit = "%";
            sensor_type = "humidity";
            break;
        case SENSOR_TYPE_LIGHT:
            value_to_send = sensor_data->converted_value; // LM%
            unit = "LM%";
            sensor_type = "light";
            break;
        default:
            // Fallback para sensores desconocidos - enviar voltaje
            value_to_send = sensor_data->adc_voltage;
            unit = "mV";
            sensor_type = "voltage";
            break;
    }

    // Redondear valor a 2 decimales (excepto para porcentaje que puede ser 1 decimal)
    float value_rounded;
    if (sensor_data->type == SENSOR_TYPE_SOIL_HUMIDITY) {
        value_rounded = roundf(value_to_send * 10.0f) / 10.0f; // 1 decimal para %
    } else {
        value_rounded = roundf(value_to_send * 100.0f) / 100.0f; // 2 decimales para otros
    }

    // Crear JSON con los datos del sensor
    cJSON *root = cJSON_CreateObject();

    // Formatear el valor como string
    char value_str[16];
    if (sensor_data->type == SENSOR_TYPE_SOIL_HUMIDITY) {
        snprintf(value_str, sizeof(value_str), "%.1f", value_rounded);
    } else {
        snprintf(value_str, sizeof(value_str), "%.2f", value_rounded);
    }

    // Añadir campos al JSON
    cJSON_AddStringToObject(root, "value", value_str);
    cJSON_AddStringToObject(root, "unit", unit);
    cJSON_AddStringToObject(root, "type", sensor_type);
    cJSON_AddNumberToObject(root, "id_sensor", id_sensor);
    cJSON_AddNumberToObject(root, "raw_value", sensor_data->raw_value);

    // Obtener timestamp actual
    uint32_t timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
    cJSON_AddNumberToObject(root, "timestamp", timestamp);

    char *json_string = cJSON_Print(root);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Error creando JSON string");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "🚀 Enviando datos del sensor [%s]: %s", device_serial, json_string);

    // Limpiar buffer de respuesta antes de nueva petición
    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    // Configurar cliente HTTP
    esp_http_client_config_t config = {
        .url = HTTP_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Error inicializando cliente HTTP");
        free(json_string);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Configurar headers
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Configurar datos POST
    esp_http_client_set_post_field(client, json_string, strlen(json_string));

    // Realizar petición
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    // Limpiar cliente HTTP
    esp_http_client_cleanup(client);
    free(json_string);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        if (status_code >= 200 && status_code < 300) {
            ESP_LOGI(TAG, "✅ Datos del sensor [%s] enviados exitosamente (HTTP %d)", device_serial, status_code);
            
            // Procesar respuesta del servidor para actualizar configuración
            esp_err_t config_result = process_server_response(sensor_data->type);
            if (config_result != ESP_OK) {
                ESP_LOGW(TAG, "⚠ Error procesando configuración de respuesta");
            }
            
            // Limpiar buffer de respuesta para próxima petición
            response_len = 0;
            memset(response_buffer, 0, sizeof(response_buffer));
            
            consecutive_failures = 0;
            reset_http_backoff();
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "⚠ Servidor respondió con código HTTP %d", status_code);
            response_len = 0; // Limpiar buffer en caso de error
            consecutive_failures++;
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "❌ Error en petición HTTP: %s", esp_err_to_name(err));
        response_len = 0; // Limpiar buffer en caso de error
        consecutive_failures++;
        return ESP_FAIL;
    }
}

// Función para validar el dispositivo consultando el endpoint de sensores por serial (GET)
esp_err_t validate_device_serial(const char *device_serial)
{
    ESP_LOGI(TAG, "🔍 Validando dispositivo con serial: %s", device_serial);

    // Construir URL
    char url[256];
    snprintf(url, sizeof(url), "%s%s", HTTP_CONFIG_URL, device_serial);

    ESP_LOGI(TAG, "URL de validación: %s", url);

    // Configurar cliente HTTP
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Error inicializando cliente HTTP para validación");
        return ESP_FAIL;
    }

    // Limpiar buffer de respuesta antes de la petición
    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    // Realizar petición
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    // Mostrar respuesta del servidor para debugging
    ESP_LOGI(TAG, "📥 Respuesta validación [%s]: HTTP %d, Error: %s", device_serial, status_code, esp_err_to_name(err));
    if (response_len > 0) {
        ESP_LOGI(TAG, "📄 Contenido respuesta: %s", response_buffer);
    }

    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        if (status_code >= 200 && status_code < 300) {
            ESP_LOGI(TAG, "✅ Dispositivo [%s] validado exitosamente (HTTP %d)", device_serial, status_code);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "⚠ Dispositivo [%s] no válido (HTTP %d) - pero continuando con envío de datos", device_serial, status_code);
            // NO retornar ESP_FAIL aquí - permitir que el envío de datos funcione
            return ESP_OK;
        }
    } else {
        ESP_LOGE(TAG, "❌ Error validando dispositivo [%s]: %s - pero continuando con envío de datos", device_serial, esp_err_to_name(err));
        // NO retornar ESP_FAIL aquí - permitir que el envío de datos funcione
        return ESP_OK;
    }
}

// Función para enviar datos del sensor al servidor
esp_err_t send_sensor_data(const sensor_data_t *sensor_data)
{
    // Determinar qué mostrar en logs según el tipo de sensor
    switch (sensor_data->type) {
        case SENSOR_TYPE_SOIL_HUMIDITY:
            ESP_LOGI(TAG, "=== ENVIANDO DATOS HUMEDAD SUELO ===");
            ESP_LOGI(TAG, "HS: %.1f%%, Voltaje: %.0f mV, Raw: %d", 
                    sensor_data->converted_value, sensor_data->adc_voltage, sensor_data->raw_value);
            break;
        case SENSOR_TYPE_LIGHT:
            ESP_LOGI(TAG, "=== ENVIANDO DATOS LUZ ===");
            ESP_LOGI(TAG, "Luz: %.0f LM%%, Voltaje: %.0f mV, Raw: %d", 
                    sensor_data->converted_value, sensor_data->adc_voltage, sensor_data->raw_value);
            break;
        default:
            ESP_LOGI(TAG, "=== ENVIANDO DATOS SENSOR ===");
            ESP_LOGI(TAG, "Valor: %.2f, Voltaje: %.0f mV, Raw: %d", 
                    sensor_data->converted_value, sensor_data->adc_voltage, sensor_data->raw_value);
            break;
    }

    // Obtener ID del sensor desde la configuración específica del sensor
    extern sensor_config_t g_sensor_humidity_config;
    extern sensor_config_t g_sensor_light_config;
    
    int id_sensor = 0;
    switch (sensor_data->type) {
        case SENSOR_TYPE_SOIL_HUMIDITY:
            id_sensor = g_sensor_humidity_config.id_sensor;
            break;
        case SENSOR_TYPE_LIGHT:
            id_sensor = g_sensor_light_config.id_sensor;
            break;
        default:
            id_sensor = 1; // Valor por defecto
            break;
    }

    if (id_sensor == 0) {
        ESP_LOGW(TAG, "⚠ ID de sensor no configurado para tipo %d, usando valor por defecto", sensor_data->type);
        id_sensor = 1; // Valor por defecto
    }

    // Determinar el serial correcto según el tipo de sensor
    const char *device_serial;
    switch (sensor_data->type) {
        case SENSOR_TYPE_SOIL_HUMIDITY:
            device_serial = DEVICE_SERIAL_HUMIDITY;
            break;
        case SENSOR_TYPE_LIGHT:
            device_serial = DEVICE_SERIAL_LIGHT;
            break;
        default:
            device_serial = DEVICE_SERIAL_HUMIDITY; // fallback
            break;
    }

    // Enviar datos usando la nueva función genérica
    esp_err_t result = send_sensor_value(sensor_data, id_sensor, device_serial);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ Datos enviados exitosamente");
        send_led_status(SYSTEM_STATE_HTTP_SEND, "Datos enviados");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "❌ Error enviando datos");
        send_led_status(SYSTEM_STATE_ERROR, "Error HTTP");
        
        // Implementar backoff en caso de fallos consecutivos
        if (consecutive_failures >= 3) {
            ESP_LOGW(TAG, "⚠ Múltiples fallos HTTP (%d), activando backoff", consecutive_failures);
            pause_all_tasks_with_backoff();
        }
        
        return ESP_FAIL;
    }
}

// Tarea principal HTTP
void task_http_client(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA HTTP CLIENT ===");

    // NO validar dispositivos al inicio - se validarán cuando se reciba el primer dato de cada sensor
    ESP_LOGI(TAG, "🔍 Validación de sensores se hará cuando se reciba el primer dato de cada uno");

    QueueHandle_t sensor_queue = (QueueHandle_t)pvParameters;
    sensor_data_t received_data;

    // Variables para controlar validación por sensor
    static bool humidity_sensor_validated = false;
    static bool light_sensor_validated = false;

    uint32_t successful_posts = 0;
    uint32_t failed_posts = 0;
    uint32_t last_activity_log = xTaskGetTickCount();

    ESP_LOGI(TAG, "Intervalo de envío: %lu ms", sensor_post_interval_ms);
    ESP_LOGI(TAG, "✓ Tarea HTTP lista para recibir datos");

    while (1) {
        // Recibir datos de sensores con timeout
        if (xQueueReceive(sensor_queue, &received_data, pdMS_TO_TICKS(sensor_post_interval_ms)) == pdTRUE) {
            
            // Limpiar cola para obtener siempre el valor más reciente
            sensor_data_t temp_data;
            while (xQueueReceive(sensor_queue, &temp_data, 0) == pdTRUE) {
                received_data = temp_data; // Actualizar con el valor más nuevo
                ESP_LOGD(TAG, "📊 Descartando valor anterior, usando más reciente");
            }
            // Verificar si los datos son válidos
            if (received_data.valid) {
                // Validar sensor si no ha sido validado aún
                if (received_data.type == SENSOR_TYPE_SOIL_HUMIDITY && !humidity_sensor_validated) {
                    ESP_LOGI(TAG, "🔍 Validando sensor de humedad por primera vez...");
                    esp_err_t validation_result = validate_device_serial(DEVICE_SERIAL_HUMIDITY);
                    if (validation_result == ESP_OK) {
                        ESP_LOGI(TAG, "✅ Sensor de humedad (0x001C) validado exitosamente");
                    } else {
                        ESP_LOGI(TAG, "ℹ Sensor de humedad (0x001C) - validación omitida, enviando datos de todos modos");
                    }
                    humidity_sensor_validated = true;
                } else if (received_data.type == SENSOR_TYPE_LIGHT && !light_sensor_validated) {
                    ESP_LOGI(TAG, "🔍 Validando sensor de luz por primera vez...");
                    esp_err_t validation_result = validate_device_serial(DEVICE_SERIAL_LIGHT);
                    if (validation_result == ESP_OK) {
                        ESP_LOGI(TAG, "✅ Sensor de luz (0x001D) validado exitosamente");
                    } else {
                        ESP_LOGI(TAG, "ℹ Sensor de luz (0x001D) - validación omitida, enviando datos de todos modos");
                    }
                    light_sensor_validated = true;
                }

                // Mostrar información del sensor según su tipo
                switch (received_data.type) {
                    case SENSOR_TYPE_SOIL_HUMIDITY:
                        ESP_LOGI(TAG, "📊 Datos recibidos - HS: %.1f%%, Voltaje: %.0f mV, Raw: %d",
                                received_data.converted_value, received_data.adc_voltage, received_data.raw_value);
                        break;
                    case SENSOR_TYPE_LIGHT:
                        ESP_LOGI(TAG, "📊 Datos recibidos - Luz: %.0f LM%%, Voltaje: %.0f mV, Raw: %d",
                                received_data.converted_value, received_data.adc_voltage, received_data.raw_value);
                        break;
                    default:
                        ESP_LOGI(TAG, "📊 Datos recibidos - Voltaje: %.0f mV, Raw: %d",
                                received_data.adc_voltage, received_data.raw_value);
                        break;
                }

                // Enviar datos al servidor
                esp_err_t send_result = send_sensor_data(&received_data);

                if (send_result == ESP_OK) {
                    successful_posts++;
                    task_send_status(TASK_TYPE_HTTP, "Datos enviados OK");
                } else {
                    failed_posts++;
                    task_report_error(TASK_TYPE_HTTP, TASK_ERROR_TIMEOUT, "HTTP send failed");
                }
            } else {
                ESP_LOGW(TAG, "⚠ Datos de sensor inválidos recibidos, descartando");
            }
        } else {
            // Timeout - no se recibieron datos
            ESP_LOGD(TAG, "⏱ Timeout esperando datos del sensor");
        }

        // Reportar estadísticas cada 10 minutos
        uint32_t current_time = xTaskGetTickCount();
        if ((current_time - last_activity_log) > pdMS_TO_TICKS(600000)) { // 10 minutos
            float success_rate = (successful_posts + failed_posts) > 0 ? 
                                (successful_posts * 100.0f) / (successful_posts + failed_posts) : 0.0f;
            
            ESP_LOGI(TAG, "📈 Estadísticas HTTP - Exitosos: %lu, Fallidos: %lu (%.1f%% éxito)",
                    successful_posts, failed_posts, success_rate);

            // Enviar heartbeat
            char heartbeat_msg[32];
            snprintf(heartbeat_msg, sizeof(heartbeat_msg), "HTTP %.1f%% OK", success_rate);
            task_send_heartbeat(TASK_TYPE_HTTP, heartbeat_msg);

            last_activity_log = current_time;

            // Reset contadores si son muy altos
            if (successful_posts + failed_posts > 1000) {
                successful_posts = successful_posts / 10;
                failed_posts = failed_posts / 10;
            }
        }

        // Pequeña pausa para evitar consumo excesivo de CPU
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
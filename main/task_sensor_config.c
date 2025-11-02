#include "task_sensor_config.h"
#include "task_main.h"
#include "config.h"
#include "esp_log.h"
#include "task_nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>

// Declaración externa de la función de validación
extern esp_err_t validate_device_serial(const char *device_serial);

static const char *TAG = "SENSOR_CONFIG";

// Variables globales para las configuraciones de cada sensor
sensor_config_t g_sensor_humidity_config = {
    .id_sensor = 0,
    .description = "Soil Humidity Sensor",
    .interval_s = 5,
    .state = true,
    .config_loaded = false
};

sensor_config_t g_sensor_light_config = {
    .id_sensor = 0,
    .description = "Light Sensor",
    .interval_s = 5,
    .state = true,
    .config_loaded = false
};

// Callback para manejar la respuesta HTTP
static esp_err_t config_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            if (evt->data_len > 0) {
                // Log de la respuesta para debug
                char *response_data = malloc(evt->data_len + 1);
                if (response_data) {
                    memcpy(response_data, evt->data, evt->data_len);
                    response_data[evt->data_len] = '\0';
                    ESP_LOGD(TAG, "Respuesta config: %s", response_data);
                    free(response_data);
                }
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// Función para obtener configuración del sensor desde el servidor
static esp_err_t fetch_sensor_config(const char *serial_number, sensor_config_t *config, const char *sensor_type, const char *nvs_key_prefix)
{
    ESP_LOGI(TAG, "=== OBTENIENDO CONFIGURACIÓN SENSOR %s ===", sensor_type);
    ESP_LOGI(TAG, "Número de serie: %s", serial_number);

    // Construir URL: https://ong-controller.vercel.app/api/v1/sensors/serial/0x001C
    char url[256];
    snprintf(url, sizeof(url), "%s%s", HTTP_CONFIG_URL, serial_number);

    ESP_LOGI(TAG, "URL: %s", url);

    // Buffer para almacenar la respuesta
    char *response_buffer = NULL;
    int response_len = 0;

    // Configurar cliente HTTP con soporte HTTPS
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = config_http_event_handler,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Error inicializando cliente HTTP");
        return ESP_FAIL;
    }

    // Realizar petición GET
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo conexión HTTP: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // Obtener longitud del contenido
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d", status_code, content_length);

    if (status_code >= 200 && status_code < 300) {
        // Leer respuesta
        if (content_length > 0) {
            response_buffer = malloc(content_length + 1);
            if (response_buffer != NULL) {
                response_len = esp_http_client_read_response(client, response_buffer, content_length);
                response_buffer[response_len] = '\0';
                
                ESP_LOGI(TAG, "Respuesta recibida (%d bytes): %s", response_len, response_buffer);
                
                // Parsear JSON
                cJSON *json = cJSON_Parse(response_buffer);
                if (json != NULL) {
                    // Extraer campos del JSON
                    cJSON *id_sensor = cJSON_GetObjectItem(json, "id_sensor");
                    cJSON *description = cJSON_GetObjectItem(json, "description");
                    cJSON *interval_s = cJSON_GetObjectItem(json, "interval_s");
                    cJSON *state = cJSON_GetObjectItem(json, "state");

                    if (id_sensor && cJSON_IsNumber(id_sensor)) {
                        config->id_sensor = id_sensor->valueint;
                        ESP_LOGI(TAG, "ID Sensor: %d", config->id_sensor);
                    }

                    if (description && cJSON_IsString(description)) {
                        strncpy(config->description, description->valuestring, sizeof(config->description) - 1);
                        config->description[sizeof(config->description) - 1] = '\0';
                        ESP_LOGI(TAG, "Descripción: %s", config->description);
                    }

                    if (interval_s && cJSON_IsNumber(interval_s)) {
                        config->interval_s = interval_s->valueint;
                        ESP_LOGI(TAG, "Intervalo: %d segundos", config->interval_s);
                    }

                    if (state && cJSON_IsBool(state)) {
                        config->state = cJSON_IsTrue(state);
                        ESP_LOGI(TAG, "Estado: %s", config->state ? "activo" : "inactivo");
                    }

                    config->config_loaded = true;
                    ESP_LOGI(TAG, "✅ Configuración de %s cargada exitosamente", sensor_type);

                    // Guardar ID en NVS con prefijo específico
                    char id_key[32];
                    char reg_key[32];
                    snprintf(id_key, sizeof(id_key), "%s_id", nvs_key_prefix);
                    snprintf(reg_key, sizeof(reg_key), "%s_registered", nvs_key_prefix);
                    
                    nvs_save_sensor_id(id_key, config->id_sensor);
                    nvs_save_registered_flag(reg_key, true);

                    cJSON_Delete(json);
                    err = ESP_OK;
                } else {
                    ESP_LOGE(TAG, "Error parseando JSON de configuración");
                    err = ESP_FAIL;
                }
                
                free(response_buffer);
            } else {
                ESP_LOGE(TAG, "Error asignando memoria para respuesta");
                err = ESP_ERR_NO_MEM;
            }
        } else {
            ESP_LOGW(TAG, "Respuesta vacía del servidor");
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Error HTTP: %d", status_code);
        err = ESP_FAIL;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "⚠ Error obteniendo configuración, usando valores por defecto");
        // Cargar valores por defecto
        config->id_sensor = 1; // ID por defecto
        snprintf(config->description, sizeof(config->description), "%s Default", sensor_type);
        config->interval_s = 5;
        config->state = true;
        config->config_loaded = false; // Marcar como no cargado desde servidor
    }

    return err;
}

// Tarea de configuración de sensores
void task_sensor_config_init(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO CONFIGURACIÓN DE SENSORES ===");

    // Cargar configuración del sensor de humedad
    ESP_LOGI(TAG, "Cargando configuración del sensor de humedad...");
    int32_t stored_humidity_id = 0;
    bool humidity_registered = false;
    
    esp_err_t nvs_result_humidity = nvs_get_sensor_id("humidity_id", &stored_humidity_id);
    nvs_get_registered_flag("humidity_registered", &humidity_registered);
    
    if (nvs_result_humidity == ESP_OK && stored_humidity_id > 0 && humidity_registered) {
        ESP_LOGI(TAG, "Configuración de humedad encontrada en NVS (ID: %ld)", stored_humidity_id);
        g_sensor_humidity_config.id_sensor = stored_humidity_id;
        g_sensor_humidity_config.config_loaded = true;
    } else {
        ESP_LOGI(TAG, "No hay configuración de humedad en NVS, obteniendo del servidor...");
        fetch_sensor_config(DEVICE_SERIAL_HUMIDITY, &g_sensor_humidity_config, "HUMEDAD", "humidity");
    }

    // Cargar configuración del sensor de luz
    ESP_LOGI(TAG, "Cargando configuración del sensor de luz...");
    int32_t stored_light_id = 0;
    bool light_registered = false;
    
    esp_err_t nvs_result_light = nvs_get_sensor_id("light_id", &stored_light_id);
    nvs_get_registered_flag("light_registered", &light_registered);
    
    if (nvs_result_light == ESP_OK && stored_light_id > 0 && light_registered) {
        ESP_LOGI(TAG, "Configuración de luz encontrada en NVS (ID: %ld)", stored_light_id);
        g_sensor_light_config.id_sensor = stored_light_id;
        g_sensor_light_config.config_loaded = true;
    } else {
        ESP_LOGI(TAG, "No hay configuración de luz en NVS, obteniendo del servidor...");
        fetch_sensor_config(DEVICE_SERIAL_LIGHT, &g_sensor_light_config, "LUZ", "light");
    }

    // Mostrar configuración final de ambos sensores
    ESP_LOGI(TAG, "=== CONFIGURACIÓN FINAL SENSORES ===");
    
    ESP_LOGI(TAG, "Sensor Humedad:");
    ESP_LOGI(TAG, "  ID: %d", g_sensor_humidity_config.id_sensor);
    ESP_LOGI(TAG, "  Descripción: %s", g_sensor_humidity_config.description);
    ESP_LOGI(TAG, "  Intervalo: %d segundos", g_sensor_humidity_config.interval_s);
    ESP_LOGI(TAG, "  Estado: %s", g_sensor_humidity_config.state ? "activo" : "inactivo");
    ESP_LOGI(TAG, "  Cargado: %s", g_sensor_humidity_config.config_loaded ? "servidor" : "por defecto");
    
    ESP_LOGI(TAG, "Sensor Luz:");
    ESP_LOGI(TAG, "  ID: %d", g_sensor_light_config.id_sensor);
    ESP_LOGI(TAG, "  Descripción: %s", g_sensor_light_config.description);
    ESP_LOGI(TAG, "  Intervalo: %d segundos", g_sensor_light_config.interval_s);
    ESP_LOGI(TAG, "  Estado: %s", g_sensor_light_config.state ? "activo" : "inactivo");
    ESP_LOGI(TAG, "  Cargado: %s", g_sensor_light_config.config_loaded ? "servidor" : "por defecto");

    // Notificar al supervisor que la configuración está completa
    ESP_LOGI(TAG, "✅ Configuración de sensores completada");
    task_send_status(TASK_TYPE_SENSOR_CONFIG, "Configuración completa");
    xSemaphoreGive(sensor_config_semaphore);

    // Esta tarea termina aquí
    vTaskDelete(NULL);
}

// Public function to refresh sensor configs on-demand. Returns ESP_OK when sensors loaded.
esp_err_t sensor_config_refresh(void)
{
    ESP_LOGI(TAG, "=== REFRESCANDO CONFIGURACIÓN DE SENSORES ===");

    esp_err_t result_humidity = fetch_sensor_config(DEVICE_SERIAL_HUMIDITY, &g_sensor_humidity_config, "HUMEDAD", "humidity");
    esp_err_t result_light = fetch_sensor_config(DEVICE_SERIAL_LIGHT, &g_sensor_light_config, "LUZ", "light");

    bool humidity_ok = (result_humidity == ESP_OK && g_sensor_humidity_config.config_loaded);
    bool light_ok = (result_light == ESP_OK && g_sensor_light_config.config_loaded);

    if (humidity_ok || light_ok) {
        ESP_LOGI(TAG, "✅ Configuración de sensores refrescada exitosamente");
        if (humidity_ok) ESP_LOGI(TAG, "  ✓ Humedad: OK");
        if (light_ok) ESP_LOGI(TAG, "  ✓ Luz: OK");
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "⚠ Error refrescando configuración de sensores");
        return ESP_FAIL;
    }
}
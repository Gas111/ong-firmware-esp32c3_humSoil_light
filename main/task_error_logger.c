#include "task_error_logger.h"
#include "task_main.h"
#include "task_sensor_config.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "ERROR_LOGGER";

#define ERROR_LOGGER_QUEUE_SIZE 50
#define ERROR_SEND_INTERVAL_MS 10000  // Intentar enviar cada 10 segundos
#define ERROR_LOG_ENDPOINT "/error-logs"
#define MAX_ERROR_TYPES 20  // Máximo de tipos de error únicos a trackear

static QueueHandle_t error_queue = NULL;
static SemaphoreHandle_t retry_semaphore = NULL; // Para forzar reintentos
static char response_buffer[1024];
static int response_len = 0;

// Estructura para trackear errores ya enviados (deduplicación)
typedef struct {
    char error_code[50];
    error_source_type_t source_type;
    int source_id;  // id_sensor, id_controller, o id_actuator
    uint32_t last_sent_time;
    uint32_t occurrence_count;
} error_dedup_entry_t;

static error_dedup_entry_t error_dedup_table[MAX_ERROR_TYPES];
static int error_dedup_count = 0;

// Callback para manejar respuesta HTTP
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            if (evt->data_len > 0 && response_len + evt->data_len < sizeof(response_buffer) - 1) {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
                response_buffer[response_len] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// Leer id_sensor desde NVS
static int32_t read_id_sensor_from_nvs(const char *device_serial)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("sensor_cfg", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir NVS para leer id_sensor: %s", esp_err_to_name(err));
        return -1;
    }
    
    // Determinar el prefijo según el device_serial
    const char *prefix = NULL;
    if (strcmp(device_serial, DEVICE_SERIAL_HUMIDITY) == 0) {
        prefix = "hum_";
    } else if (strcmp(device_serial, DEVICE_SERIAL_LIGHT) == 0) {
        prefix = "light_";  // Cambiar de "luz_" a "light_"
    } else {
        nvs_close(handle);
        ESP_LOGW(TAG, "device_serial desconocido: %s", device_serial);
        return -1;
    }
    
    // Leer id_sensor
    char key[32];
    snprintf(key, sizeof(key), "%sid", prefix);
    int32_t id_sensor = -1;
    err = nvs_get_i32(handle, key, &id_sensor);
    
    nvs_close(handle);
    
    if (err == ESP_OK && id_sensor > 0) {
        ESP_LOGI(TAG, "✓ ID sensor leído de NVS: %ld (serial: %s, key: %s)", (long)id_sensor, device_serial, key);
        return id_sensor;
    } else {
        ESP_LOGW(TAG, "⚠ No se encontró id_sensor en NVS para serial: %s (key: %s, err: %s)", 
                 device_serial, key, esp_err_to_name(err));
        return -1;
    }
}

// Verificar si un error es duplicado (ya fue enviado recientemente)
static bool is_duplicate_error(const error_log_entry_t *error, uint32_t *occurrence_count)
{
    int source_id = -1;
    if (error->source_type == ERROR_SOURCE_SENSOR) {
        source_id = error->id_sensor;
    } else if (error->source_type == ERROR_SOURCE_CONTROLLER) {
        source_id = error->id_controller_station;
    } else if (error->source_type == ERROR_SOURCE_ACTUATOR) {
        source_id = error->id_actuator;
    }
    
    // Buscar en tabla de deduplicación
    for (int i = 0; i < error_dedup_count; i++) {
        if (strcmp(error_dedup_table[i].error_code, error->error_code) == 0 &&
            error_dedup_table[i].source_type == error->source_type &&
            error_dedup_table[i].source_id == source_id) {
            
            // Encontrado - incrementar contador de ocurrencias
            error_dedup_table[i].occurrence_count++;
            *occurrence_count = error_dedup_table[i].occurrence_count;
            
            ESP_LOGD(TAG, "🔁 Error duplicado: [%s] (ocurrencias: %lu)", 
                     error->error_code, (unsigned long)*occurrence_count);
            return true;
        }
    }
    
    return false;
}

// Marcar error como enviado (agregar a tabla de deduplicación)
static void mark_error_as_sent(const error_log_entry_t *error)
{
    int source_id = -1;
    if (error->source_type == ERROR_SOURCE_SENSOR) {
        source_id = error->id_sensor;
    } else if (error->source_type == ERROR_SOURCE_CONTROLLER) {
        source_id = error->id_controller_station;
    } else if (error->source_type == ERROR_SOURCE_ACTUATOR) {
        source_id = error->id_actuator;
    }
    
    // Buscar si ya existe en la tabla
    for (int i = 0; i < error_dedup_count; i++) {
        if (strcmp(error_dedup_table[i].error_code, error->error_code) == 0 &&
            error_dedup_table[i].source_type == error->source_type &&
            error_dedup_table[i].source_id == source_id) {
            
            // Ya existe, actualizar timestamp
            error_dedup_table[i].last_sent_time = xTaskGetTickCount();
            return;
        }
    }
    
    // No existe, agregar nuevo (si hay espacio)
    if (error_dedup_count < MAX_ERROR_TYPES) {
        strncpy(error_dedup_table[error_dedup_count].error_code, error->error_code, sizeof(error_dedup_table[0].error_code) - 1);
        error_dedup_table[error_dedup_count].source_type = error->source_type;
        error_dedup_table[error_dedup_count].source_id = source_id;
        error_dedup_table[error_dedup_count].last_sent_time = xTaskGetTickCount();
        error_dedup_table[error_dedup_count].occurrence_count = 1;
        error_dedup_count++;
        
        ESP_LOGI(TAG, "➕ Nuevo tipo de error registrado: [%s] (total: %d)", error->error_code, error_dedup_count);
    } else {
        // Tabla llena, reemplazar entrada más antigua
        int oldest_idx = 0;
        uint32_t oldest_time = error_dedup_table[0].last_sent_time;
        
        for (int i = 1; i < MAX_ERROR_TYPES; i++) {
            if (error_dedup_table[i].last_sent_time < oldest_time) {
                oldest_time = error_dedup_table[i].last_sent_time;
                oldest_idx = i;
            }
        }
        
        strncpy(error_dedup_table[oldest_idx].error_code, error->error_code, sizeof(error_dedup_table[0].error_code) - 1);
        error_dedup_table[oldest_idx].source_type = error->source_type;
        error_dedup_table[oldest_idx].source_id = source_id;
        error_dedup_table[oldest_idx].last_sent_time = xTaskGetTickCount();
        error_dedup_table[oldest_idx].occurrence_count = 1;
    }
}

// Limpiar tabla de deduplicación (errores antiguos pueden enviarse de nuevo)
static void clear_old_dedup_entries(void)
{
    uint32_t current_time = xTaskGetTickCount();
    uint32_t timeout = pdMS_TO_TICKS(600000); // 10 minutos
    
    int i = 0;
    while (i < error_dedup_count) {
        if ((current_time - error_dedup_table[i].last_sent_time) > timeout) {
            // Remover entrada antigua
            ESP_LOGD(TAG, "🗑️ Removiendo entrada antigua: [%s]", error_dedup_table[i].error_code);
            
            // Mover última entrada a esta posición
            if (i < error_dedup_count - 1) {
                error_dedup_table[i] = error_dedup_table[error_dedup_count - 1];
            }
            error_dedup_count--;
        } else {
            i++;
        }
    }
}

// Inicializar sistema de logging
esp_err_t error_logger_init(void)
{
    error_queue = xQueueCreate(ERROR_LOGGER_QUEUE_SIZE, sizeof(error_log_entry_t));
    if (error_queue == NULL) {
        ESP_LOGE(TAG, "❌ Error creando cola de errores");
        return ESP_FAIL;
    }
    
    retry_semaphore = xSemaphoreCreateBinary();
    if (retry_semaphore == NULL) {
        ESP_LOGE(TAG, "❌ Error creando semáforo de reintentos");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Sistema de logging de errores inicializado");
    ESP_LOGI(TAG, "   - Cola: %d entradas", ERROR_LOGGER_QUEUE_SIZE);
    return ESP_OK;
}

// Registrar error en la cola
esp_err_t error_logger_log(const error_log_entry_t *error)
{
    if (error_queue == NULL) {
        ESP_LOGE(TAG, "❌ Cola de errores no inicializada");
        return ESP_FAIL;
    }
    
    error_log_entry_t entry = *error;
    entry.pending = true;
    entry.timestamp = xTaskGetTickCount();
    
    if (xQueueSend(error_queue, &entry, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ Cola de errores llena, descartando error: %s", error->message);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "📝 Error registrado: [%s] %s", error->error_code, error->message);
    return ESP_OK;
}

// Helper para log de sensor
esp_err_t error_logger_log_sensor(
    int id_sensor,
    const char *error_code,
    error_severity_t severity,
    const char *message,
    const char *details_json,
    const char *device_serial
)
{
    error_log_entry_t error = {
        .source_type = ERROR_SOURCE_SENSOR,
        .id_sensor = id_sensor,
        .id_controller_station = -1,
        .id_actuator = -1,
        .severity = severity,
        .timestamp = xTaskGetTickCount(),
        .pending = true
    };
    
    strncpy(error.error_code, error_code, sizeof(error.error_code) - 1);
    strncpy(error.message, message, sizeof(error.message) - 1);
    
    if (details_json != NULL) {
        strncpy(error.details_json, details_json, sizeof(error.details_json) - 1);
    } else {
        error.details_json[0] = '\0';
    }
    
    if (device_serial != NULL) {
        strncpy(error.device_serial, device_serial, sizeof(error.device_serial) - 1);
    } else {
        error.device_serial[0] = '\0';
    }
    
    // Obtener IP address
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(error.ip_address, sizeof(error.ip_address), IPSTR, IP2STR(&ip_info.ip));
        } else {
            strcpy(error.ip_address, "0.0.0.0");
        }
    } else {
        strcpy(error.ip_address, "0.0.0.0");
    }
    
    return error_logger_log(&error);
}

// Helper para log de sistema
// Genera 2 errores (uno por cada sensor asociado al ESP32)
esp_err_t error_logger_log_system(
    const char *error_code,
    error_severity_t severity,
    const char *message,
    const char *details_json
)
{
    // Acceder a configuraciones de sensores
    extern sensor_config_t g_sensor_humidity_config;
    extern sensor_config_t g_sensor_light_config;
    
    // Obtener IP address una sola vez
    char ip_address[16];
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&ip_info.ip));
        } else {
            strcpy(ip_address, "0.0.0.0");
        }
    } else {
        strcpy(ip_address, "0.0.0.0");
    }
    
    // Leer id_sensor desde NVS (fallback si config global no está lista)
    int32_t humidity_id = g_sensor_humidity_config.id_sensor;
    int32_t light_id = g_sensor_light_config.id_sensor;
    
    ESP_LOGI(TAG, "🔍 Config global - Humedad id=%ld, Luz id=%ld", (long)humidity_id, (long)light_id);
    
    // Si id_sensor no está configurado (<=0), leer desde NVS
    if (humidity_id <= 0) {
        humidity_id = read_id_sensor_from_nvs(DEVICE_SERIAL_HUMIDITY);
        ESP_LOGI(TAG, "📖 ID humedad leído desde NVS: %ld", (long)humidity_id);
    }
    if (light_id <= 0) {
        light_id = read_id_sensor_from_nvs(DEVICE_SERIAL_LIGHT);
        ESP_LOGI(TAG, "📖 ID luz leído desde NVS: %ld", (long)light_id);
    }
    
    // Validar que tengamos IDs válidos
    if (humidity_id <= 0) {
        ESP_LOGW(TAG, "⚠️  ID humedad inválido (%ld) - no se enviará error para humedad", (long)humidity_id);
    }
    if (light_id <= 0) {
        ESP_LOGW(TAG, "⚠️  ID luz inválido (%ld) - no se enviará error para luz", (long)light_id);
    }
    
    // ERROR 1: Para sensor de HUMEDAD
    error_log_entry_t error_humidity = {
        .source_type = ERROR_SOURCE_SENSOR,
        .id_sensor = humidity_id,
        .id_controller_station = -1,
        .id_actuator = -1,
        .severity = severity,
        .timestamp = xTaskGetTickCount(),
        .pending = true
    };
    
    ESP_LOGI(TAG, "🔍 DEBUG - Humedad: id_sensor=%ld, state=%d, device_serial=%s", 
             (long)error_humidity.id_sensor, g_sensor_humidity_config.state, DEVICE_SERIAL_HUMIDITY);
    
    strncpy(error_humidity.error_code, error_code, sizeof(error_humidity.error_code) - 1);
    strncpy(error_humidity.message, message, sizeof(error_humidity.message) - 1);
    
    if (details_json != NULL) {
        strncpy(error_humidity.details_json, details_json, sizeof(error_humidity.details_json) - 1);
    } else {
        error_humidity.details_json[0] = '\0';
    }
    
    strncpy(error_humidity.device_serial, DEVICE_SERIAL_HUMIDITY, sizeof(error_humidity.device_serial) - 1);
    strncpy(error_humidity.ip_address, ip_address, sizeof(error_humidity.ip_address) - 1);
    
    // ERROR 2: Para sensor de LUZ
    error_log_entry_t error_light = {
        .source_type = ERROR_SOURCE_SENSOR,
        .id_sensor = light_id,
        .id_controller_station = -1,
        .id_actuator = -1,
        .severity = severity,
        .timestamp = xTaskGetTickCount(),
        .pending = true
    };
    
    ESP_LOGI(TAG, "🔍 DEBUG - Luz: id_sensor=%ld, state=%d, device_serial=%s", 
             (long)error_light.id_sensor, g_sensor_light_config.state, DEVICE_SERIAL_LIGHT);
    
    strncpy(error_light.error_code, error_code, sizeof(error_light.error_code) - 1);
    strncpy(error_light.message, message, sizeof(error_light.message) - 1);
    
    if (details_json != NULL) {
        strncpy(error_light.details_json, details_json, sizeof(error_light.details_json) - 1);
    } else {
        error_light.details_json[0] = '\0';
    }
    
    strncpy(error_light.device_serial, DEVICE_SERIAL_LIGHT, sizeof(error_light.device_serial) - 1);
    strncpy(error_light.ip_address, ip_address, sizeof(error_light.ip_address) - 1);
    
    // Enviar ambos errores (backend acepta id_sensor null si no está configurado aún)
    esp_err_t result1 = error_logger_log(&error_humidity);
    if (result1 == ESP_OK) {
        ESP_LOGI(TAG, "✅ Error humedad encolado (id_sensor=%ld)", (long)humidity_id);
    }
    
    esp_err_t result2 = error_logger_log(&error_light);
    if (result2 == ESP_OK) {
        ESP_LOGI(TAG, "✅ Error luz encolado (id_sensor=%ld)", (long)light_id);
    }
    
    // Retornar OK si al menos uno se encoló correctamente
    return (result1 == ESP_OK || result2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

// Obtener cantidad de errores pendientes
int error_logger_get_pending_count(void)
{
    if (error_queue == NULL) return 0;
    return uxQueueMessagesWaiting(error_queue);
}

// Forzar reintento inmediato (llamar cuando WiFi reconecta)
void error_logger_trigger_retry(void)
{
    if (retry_semaphore != NULL) {
        xSemaphoreGive(retry_semaphore);
        ESP_LOGI(TAG, "🔔 Reintento de errores forzado por reconexión");
    }
}

// Convertir severidad a string
static const char* severity_to_string(error_severity_t severity)
{
    switch (severity) {
        case ERROR_SEVERITY_INFO: return "INFO";
        case ERROR_SEVERITY_WARNING: return "WARNING";
        case ERROR_SEVERITY_ERROR: return "ERROR";
        case ERROR_SEVERITY_CRITICAL: return "CRITICAL";
        default: return "ERROR";
    }
}

// Convertir source type a string
static const char* source_type_to_string(error_source_type_t type)
{
    switch (type) {
        case ERROR_SOURCE_SENSOR: return "SENSOR";
        case ERROR_SOURCE_CONTROLLER: return "CONTROLLER";
        case ERROR_SOURCE_ACTUATOR: return "ACTUATOR";
        case ERROR_SOURCE_SYSTEM: return "SYSTEM";
        default: return "SYSTEM";
    }
}

// Enviar error al backend
static esp_err_t send_error_to_backend(const error_log_entry_t *error, uint32_t occurrence_count)
{
    // Construir URL completa
    char url[256];
    snprintf(url, sizeof(url), "%s%s", HTTP_SERVER_BASE_URL, ERROR_LOG_ENDPOINT);
    
    // Crear JSON
    cJSON *root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "source_type", source_type_to_string(error->source_type));
    
    ESP_LOGI(TAG, "🔍 Construyendo JSON - source_type: %s, id_sensor: %ld", 
             source_type_to_string(error->source_type), (long)error->id_sensor);
    
    // Agregar IDs según el tipo de fuente
    if (error->source_type == ERROR_SOURCE_SENSOR) {
        // Siempre agregar id_sensor (backend lo convierte a null si es <= 0)
        cJSON_AddNumberToObject(root, "id_sensor", error->id_sensor);
        if (error->id_sensor > 0) {
            ESP_LOGI(TAG, "✅ id_sensor válido agregado al JSON: %ld", (long)error->id_sensor);
        } else {
            ESP_LOGW(TAG, "⚠️  id_sensor inválido (%ld) - backend lo recibirá como null", (long)error->id_sensor);
        }
    } else if (error->source_type == ERROR_SOURCE_CONTROLLER && error->id_controller_station > 0) {
        cJSON_AddNumberToObject(root, "id_controller_station", error->id_controller_station);
    } else if (error->source_type == ERROR_SOURCE_ACTUATOR && error->id_actuator > 0) {
        cJSON_AddNumberToObject(root, "id_actuator", error->id_actuator);
    }
    
    cJSON_AddStringToObject(root, "error_code", error->error_code);
    cJSON_AddStringToObject(root, "severity", severity_to_string(error->severity));
    cJSON_AddStringToObject(root, "message", error->message);
    
    // Agregar details como objeto JSON si existe, o crear uno nuevo
    cJSON *details = NULL;
    if (strlen(error->details_json) > 0) {
        details = cJSON_Parse(error->details_json);
    }
    
    if (details == NULL) {
        details = cJSON_CreateObject();
    }
    
    // Agregar contador de ocurrencias si es mayor a 1
    if (occurrence_count > 1) {
        cJSON_AddNumberToObject(details, "occurrence_count", occurrence_count);
    }
    
    cJSON_AddItemToObject(root, "details", details);
    
    if (strlen(error->ip_address) > 0) {
        cJSON_AddStringToObject(root, "ip_address", error->ip_address);
    }
    
    if (strlen(error->device_serial) > 0) {
        cJSON_AddStringToObject(root, "device_serial", error->device_serial);
    }
    
    char *json_string = cJSON_Print(root);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "❌ Error creando JSON");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    if (occurrence_count > 1) {
        ESP_LOGI(TAG, "🚀 Enviando error (x%lu): %s", (unsigned long)occurrence_count, json_string);
    } else {
        ESP_LOGI(TAG, "🚀 Enviando error: %s", json_string);
    }
    
    // Limpiar buffer de respuesta
    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));
    
    // Configurar cliente HTTP
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "❌ Error inicializando cliente HTTP");
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
    
    // Limpiar
    esp_http_client_cleanup(client);
    free(json_string);
    cJSON_Delete(root);
    
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        ESP_LOGI(TAG, "✅ Error enviado exitosamente al backend (HTTP %d)", status_code);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "⚠️ Error enviando al backend: %s (HTTP %d)", esp_err_to_name(err), status_code);
        return ESP_FAIL;
    }
}

// Tarea principal
void task_error_logger(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA ERROR LOGGER ===");
    
    if (error_queue == NULL) {
        ESP_LOGE(TAG, "❌ Cola no inicializada, abortando tarea");
        vTaskDelete(NULL);
        return;
    }
    
    error_log_entry_t error;
    uint32_t sent_count = 0;
    uint32_t failed_count = 0;
    uint32_t duplicate_count = 0;
    
    // Cola temporal para reintentar errores fallidos
    QueueHandle_t retry_queue = xQueueCreate(ERROR_LOGGER_QUEUE_SIZE, sizeof(error_log_entry_t));
    
    uint32_t last_cleanup = xTaskGetTickCount();
    
    while (1) {
        // Esperar por nuevo error O señal de reintento forzado
        bool force_retry = false;
        if (xSemaphoreTake(retry_semaphore, 0) == pdTRUE) {
            force_retry = true;
            ESP_LOGI(TAG, "⚡ Reintento forzado activado");
        }
        
        // Intentar recibir error de la cola principal (timeout corto si hay reintento forzado)
        TickType_t timeout = force_retry ? 0 : pdMS_TO_TICKS(ERROR_SEND_INTERVAL_MS);
        if (xQueueReceive(error_queue, &error, timeout) == pdTRUE) {
            
            ESP_LOGI(TAG, "📤 Procesando error: [%s] %s", error.error_code, error.message);
            
            // Verificar si es duplicado
            uint32_t occurrence_count = 1;
            if (is_duplicate_error(&error, &occurrence_count)) {
                duplicate_count++;
                ESP_LOGD(TAG, "⏭️ Error duplicado ignorado (total: %lu)", (unsigned long)duplicate_count);
                continue; // No enviar, continuar con el siguiente
            }
            
            // Verificar conectividad ANTES de intentar enviar
            EventBits_t bits = xEventGroupWaitBits(
                g_connectivity_event_group,
                CONNECTIVITY_WIFI_CONNECTED_BIT,
                pdFALSE,  // No clear bits
                pdFALSE,  // Wait for any bit
                portMAX_DELAY  // Esperar indefinidamente por conectividad
            );
            
            if (!(bits & CONNECTIVITY_WIFI_CONNECTED_BIT)) {
                ESP_LOGW(TAG, "⏸️ Sin conectividad, devolviendo error a cola");
                xQueueSend(retry_queue, &error, 0);
                continue;
            }
            
            // Intentar enviar al backend
            esp_err_t result = send_error_to_backend(&error, occurrence_count);
            
            if (result == ESP_OK) {
                sent_count++;
                mark_error_as_sent(&error);  // Marcar como enviado para deduplicación
                ESP_LOGI(TAG, "✅ Error enviado correctamente (total: %lu)", (unsigned long)sent_count);
            } else {
                failed_count++;
                ESP_LOGW(TAG, "⚠️ Error al enviar, reintentando más tarde (total fallos: %lu)", (unsigned long)failed_count);
                
                // Agregar a cola de reintentos (no bloquear)
                if (xQueueSend(retry_queue, &error, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "⚠️ Cola de reintentos llena, error perdido");
                }
            }
        }
        
        // Procesar cola de reintentos (más agresivo si hay reintento forzado)
        int max_retries = force_retry ? 20 : 5; // Procesar más errores si es reintento forzado
        int retry_count = 0;
        
        while (retry_count < max_retries && xQueueReceive(retry_queue, &error, 0) == pdTRUE) {
            ESP_LOGI(TAG, "🔄 Reintentando envío de error: [%s]", error.error_code);
            
            // Verificar conectividad antes de cada reintento
            EventBits_t bits = xEventGroupGetBits(g_connectivity_event_group);
            if (!(bits & CONNECTIVITY_WIFI_CONNECTED_BIT)) {
                ESP_LOGD(TAG, "⏸️ Sin conectividad durante reintento, devolviendo a cola");
                xQueueSend(retry_queue, &error, 0);
                break;  // Salir del loop de reintentos
            }
            
            uint32_t occurrence_count = 1;
            esp_err_t result = send_error_to_backend(&error, occurrence_count);
            
            if (result == ESP_OK) {
                sent_count++;
                mark_error_as_sent(&error);
                ESP_LOGI(TAG, "✅ Reintento exitoso");
            } else {
                // Volver a agregar a la cola de reintentos (al final)
                xQueueSend(retry_queue, &error, 0);
                
                // Si no es reintento forzado, salir del loop tras primer fallo
                if (!force_retry) {
                    break;
                }
            }
            retry_count++;
        }
        
        // Limpieza periódica de tabla de deduplicación (cada 10 minutos)
        uint32_t current_time = xTaskGetTickCount();
        if ((current_time - last_cleanup) > pdMS_TO_TICKS(600000)) {
            clear_old_dedup_entries();
            last_cleanup = current_time;
        }
        
        // Estadísticas cada 5 minutos
        static uint32_t last_stats = 0;
        if ((current_time - last_stats) > pdMS_TO_TICKS(300000)) {
            int pending = error_logger_get_pending_count();
            int retries = uxQueueMessagesWaiting(retry_queue);
            
            ESP_LOGI(TAG, "📊 Estadísticas - Enviados: %lu, Fallidos: %lu, Duplicados: %lu, Pendientes: %d, Reintentos: %d",
                     (unsigned long)sent_count, (unsigned long)failed_count, (unsigned long)duplicate_count, pending, retries);
            
            last_stats = current_time;
        }
    }
}

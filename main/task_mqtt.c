#include "task_mqtt.h"
#include "config.h"
#include "task_sensor_config.h"
#include "task_main.h"
#include "task_sensor.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "MQTT";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

/**
 * @brief Procesa el mensaje JSON de configuración recibido
 * 
 * @param serial Número de serie del sensor
 * @param json_data Datos JSON recibidos
 */
static void process_sensor_config_message(const char *serial, const char *json_data)
{
    ESP_LOGI(TAG, "Procesando configuración para sensor %s", serial);
    ESP_LOGI(TAG, "JSON recibido: %s", json_data);
    
    cJSON *root = cJSON_Parse(json_data);
    if (root == NULL) {
        ESP_LOGE(TAG, "Error al parsear JSON");
        return;
    }
    
    // Determinar qué sensor actualizar basándose en el serial
    sensor_config_t *config = NULL;
    if (strcmp(serial, DEVICE_SERIAL_HUMIDITY) == 0) {
        config = &g_sensor_humidity_config;
        ESP_LOGI(TAG, "Actualizando configuración del sensor de HUMEDAD");
    } else if (strcmp(serial, DEVICE_SERIAL_LIGHT) == 0) {
        config = &g_sensor_light_config;
        ESP_LOGI(TAG, "Actualizando configuración del sensor de LUZ");
    } else {
        ESP_LOGE(TAG, "Serial desconocido: %s", serial);
        cJSON_Delete(root);
        return;
    }
    
    // Parsear campos del JSON
    cJSON *id_sensor = cJSON_GetObjectItem(root, "id_sensor");
    if (cJSON_IsNumber(id_sensor)) {
        config->id_sensor = id_sensor->valueint;
        ESP_LOGI(TAG, "  id_sensor: %d", config->id_sensor);
    }
    
    cJSON *interval = cJSON_GetObjectItem(root, "interval_seconds");
    if (cJSON_IsNumber(interval)) {
        config->interval_s = interval->valueint;
        ESP_LOGI(TAG, "  interval_seconds: %d", config->interval_s);
    }
    
    cJSON *state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state)) {
        config->state = (strcmp(state->valuestring, "active") == 0);
        ESP_LOGI(TAG, "  state: %s", config->state ? "active" : "inactive");
    }
    
    // Campos opcionales - max_value
    cJSON *max_value = cJSON_GetObjectItem(root, "max_value");
    if (cJSON_IsNumber(max_value)) {
        config->max_value = (float)max_value->valuedouble;
        config->has_max_value = true;
        ESP_LOGI(TAG, "  max_value: %.2f", config->max_value);
    } else if (cJSON_IsNull(max_value)) {
        config->has_max_value = false;
        ESP_LOGI(TAG, "  max_value: null");
    }
    
    // Campos opcionales - min_value
    cJSON *min_value = cJSON_GetObjectItem(root, "min_value");
    if (cJSON_IsNumber(min_value)) {
        config->min_value = (float)min_value->valuedouble;
        config->has_min_value = true;
        ESP_LOGI(TAG, "  min_value: %.2f", config->min_value);
    } else if (cJSON_IsNull(min_value)) {
        config->has_min_value = false;
        ESP_LOGI(TAG, "  min_value: null");
    }
    
    // Campos de auditoría
    cJSON *id_user_created = cJSON_GetObjectItem(root, "id_user_created");
    if (cJSON_IsNumber(id_user_created)) {
        config->id_user_created = id_user_created->valueint;
        ESP_LOGI(TAG, "  id_user_created: %d", config->id_user_created);
    }
    
    cJSON *id_user_modified = cJSON_GetObjectItem(root, "id_user_modified");
    if (cJSON_IsNumber(id_user_modified)) {
        config->id_user_modified = id_user_modified->valueint;
        ESP_LOGI(TAG, "  id_user_modified: %d", config->id_user_modified);
    } else if (cJSON_IsNull(id_user_modified)) {
        config->id_user_modified = 0; // NULL se representa como 0
    }
    
    cJSON *created_at = cJSON_GetObjectItem(root, "created_at");
    if (cJSON_IsString(created_at)) {
        strncpy(config->created_at, created_at->valuestring, sizeof(config->created_at) - 1);
        config->created_at[sizeof(config->created_at) - 1] = '\0';
        ESP_LOGI(TAG, "  created_at: %s", config->created_at);
    }
    
    cJSON *modified_at = cJSON_GetObjectItem(root, "modified_at");
    if (cJSON_IsString(modified_at)) {
        strncpy(config->modified_at, modified_at->valuestring, sizeof(config->modified_at) - 1);
        config->modified_at[sizeof(config->modified_at) - 1] = '\0';
        ESP_LOGI(TAG, "  modified_at: %s", config->modified_at);
    }
    
    config->config_loaded = true;
    ESP_LOGI(TAG, "✓ Configuración actualizada exitosamente para sensor %s", serial);
    
    // ===== ENVIAR MENSAJE A COLA PARA ACTUALIZACIÓN EN TIEMPO REAL =====
    config_update_message_t update_msg = {
        .new_interval_s = config->interval_s,
        .update_interval = true
    };
    
    QueueHandle_t target_queue = NULL;
    if (strcmp(serial, DEVICE_SERIAL_HUMIDITY) == 0) {
        update_msg.type = SENSOR_TYPE_SOIL_HUMIDITY;
        target_queue = get_humidity_config_queue();
        ESP_LOGI(TAG, "📨 Enviando actualización de intervalo (%d seg) a tarea de HUMEDAD", update_msg.new_interval_s);
    } else if (strcmp(serial, DEVICE_SERIAL_LIGHT) == 0) {
        update_msg.type = SENSOR_TYPE_LIGHT;
        target_queue = get_light_config_queue();
        ESP_LOGI(TAG, "📨 Enviando actualización de intervalo (%d seg) a tarea de LUZ", update_msg.new_interval_s);
    }
    
    if (target_queue != NULL) {
        if (xQueueSend(target_queue, &update_msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "✓ Mensaje de actualización enviado correctamente");
        } else {
            ESP_LOGW(TAG, "⚠ No se pudo enviar mensaje de actualización (cola llena)");
        }
    }
    
    cJSON_Delete(root);
}

/**
 * @brief Manejador de eventos MQTT
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✓ Conectado al broker MQTT");
            mqtt_connected = true;
            
            // Suscribirse a los topics de configuración
            int msg_id;
            msg_id = esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_CONFIG_HUMIDITY, MQTT_QOS);
            ESP_LOGI(TAG, "Suscrito a %s, msg_id=%d", MQTT_TOPIC_CONFIG_HUMIDITY, msg_id);
            
            msg_id = esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC_CONFIG_LIGHT, MQTT_QOS);
            ESP_LOGI(TAG, "Suscrito a %s, msg_id=%d", MQTT_TOPIC_CONFIG_LIGHT, msg_id);
            
            // Publicar estado online
            mqtt_publish_status("online");
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Desconectado del broker MQTT");
            mqtt_connected = false;
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Suscripción exitosa, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "Cancelada suscripción, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "Mensaje publicado, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Mensaje MQTT recibido:");
            ESP_LOGI(TAG, "  TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "  DATA=%.*s", event->data_len, event->data);
            
            // Extraer serial del topic (formato: ong/sensor/{serial}/config)
            char topic[MQTT_MAX_TOPIC_LEN];
            snprintf(topic, sizeof(topic), "%.*s", event->topic_len, event->topic);
            
            char *serial_start = strstr(topic, "ong/sensor/");
            if (serial_start != NULL) {
                serial_start += strlen("ong/sensor/");
                char *serial_end = strchr(serial_start, '/');
                if (serial_end != NULL) {
                    char serial[16];
                    int serial_len = serial_end - serial_start;
                    if (serial_len < sizeof(serial)) {
                        strncpy(serial, serial_start, serial_len);
                        serial[serial_len] = '\0';
                        
                        // Crear string null-terminated del payload
                        char payload[MQTT_MAX_PAYLOAD_LEN];
                        snprintf(payload, sizeof(payload), "%.*s", event->data_len, event->data);
                        
                        // Procesar configuración
                        process_sensor_config_message(serial, payload);
                    }
                }
            }
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Error MQTT");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Error de transporte TCP reportado");
            }
            break;
            
        default:
            ESP_LOGD(TAG, "Evento MQTT no manejado: %d", event_id);
            break;
    }
}

esp_err_t mqtt_client_init(void)
{
    ESP_LOGI(TAG, "Inicializando cliente MQTT...");
    
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .broker.address.port = MQTT_BROKER_PORT,
        .credentials.client_id = MQTT_CLIENT_ID,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.keepalive = MQTT_KEEPALIVE,
        .network.reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
        .network.disable_auto_reconnect = false,
        // Configuración TLS para mqtts:// usando bundle de certificados
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Error al inicializar cliente MQTT");
        return ESP_FAIL;
    }
    
    esp_err_t ret = esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al registrar manejador de eventos MQTT");
        return ret;
    }
    
    ret = esp_mqtt_client_start(mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al iniciar cliente MQTT");
        return ret;
    }
    
    ESP_LOGI(TAG, "Cliente MQTT iniciado correctamente");
    return ESP_OK;
}

esp_err_t mqtt_publish_status(const char *status)
{
    if (!mqtt_connected) {
        ESP_LOGW(TAG, "No se puede publicar estado: no conectado");
        return ESP_ERR_INVALID_STATE;
    }
    
    char payload[256];
    snprintf(payload, sizeof(payload), 
             "{\"client_id\":\"%s\",\"status\":\"%s\",\"humidity_serial\":\"%s\",\"light_serial\":\"%s\"}",
             MQTT_CLIENT_ID, status, DEVICE_SERIAL_HUMIDITY, DEVICE_SERIAL_LIGHT);
    
    int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, payload, 0, MQTT_QOS, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Error al publicar estado");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Estado publicado: %s", status);
    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    return mqtt_connected;
}

void task_mqtt_client(void *pvParameters)
{
    ESP_LOGI(TAG, "Iniciando tarea MQTT...");
    
    // Esperar a que WiFi esté conectado (máximo 30 segundos)
    int wait_count = 0;
    while (wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait_count++;
        // Asumimos que si pasaron 5 segundos, WiFi ya debe estar conectado
        if (wait_count >= 5) {
            break;
        }
    }
    
    esp_err_t ret = mqtt_client_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error al inicializar MQTT, abortando tarea");
        vTaskDelete(NULL);
        return;
    }
    
    // Loop de heartbeat
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Cada 60 segundos
        
        if (mqtt_connected) {
            mqtt_publish_status("online");
        } else {
            ESP_LOGW(TAG, "MQTT desconectado, esperando reconexión automática...");
        }
    }
}

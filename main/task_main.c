#include "task_main.h"
#include "esp_log.h"
#include "led_strip.h"
#include "config.h"
#include "task_initial_config.h"
#include "task_wifi.h"
#include "task_sensor_config.h"
#include "task_sensors_unified.h"
#include "task_http.h"
#include "task_led_status.h"
#include "task_nvs.h"
#include "task_mqtt.h"
#include <string.h>

static const char *TAG = "TASK_MAIN";

uint32_t sensor_sampling_interval_ms = SENSOR_READING_INTERVAL_MS;
uint32_t sensor_post_interval_ms = SENSOR_POST_INTERVAL_MS_DEFAULT;

// Variables globales para límites de sensores ADC
float sensor_adc_min = 0.0f;     // Valor por defecto
float sensor_adc_max = 3300.0f;  // Valor por defecto (3.3V en mV)

// LED Neopixel global compartido entre tareas
led_strip_handle_t g_led_strip = NULL;

// Función para inicializar LED Neopixel
static void init_status_led(void)
{
    ESP_LOGI(TAG, "Inicializando LED de estado (WS2812)...");
    
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_RGB_GPIO,
        .max_leds = LED_NUMBERS,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando LED strip: %s", esp_err_to_name(ret));
        return;
    }

    // Limpiar el LED
    led_strip_clear(g_led_strip);
    ESP_LOGI(TAG, "✓ LED de estado inicializado correctamente");
}

// Semáforos para sincronización de inicialización secuencial
SemaphoreHandle_t init_config_semaphore = NULL;
SemaphoreHandle_t wifi_init_semaphore = NULL;
SemaphoreHandle_t sensor_config_semaphore = NULL;
SemaphoreHandle_t system_ready_semaphore = NULL;

// Handles de las tareas para poder controlarlas
static TaskHandle_t task_handles[TASK_TYPE_MAX] = {NULL};
// Track which tasks we suspended via pause_all_tasks_for_ms so we can resume them
static bool task_suspended[TASK_TYPE_MAX] = {false};
// HTTP backoff stage: 0 == initial (5m), 1 == 10m, 2 == 30m, capped at max
static int http_backoff_stage = 0;

static uint32_t backoff_ms_for_stage(int stage)
{
    switch (stage) {
        case 0: return 5 * 60 * 1000;   // 5 minutos
        case 1: return 10 * 60 * 1000;  // 10 minutos
        case 2: return 30 * 60 * 1000;  // 30 minutos
        default: return 30 * 60 * 1000; // Cap at 30 minutos
    }
}

// Pause tasks using progressive backoff. Each call increments the backoff stage
// up to a cap. The function suspends tasks, waits the interval for the stage,
// then resumes tasks.
void pause_all_tasks_with_backoff(void)
{
    uint32_t backoff_time = backoff_ms_for_stage(http_backoff_stage);
    
    ESP_LOGW(TAG, "Pausando tareas por %lu ms (backoff stage %d)", backoff_time, http_backoff_stage);
    
    // Suspend tasks (except this supervisor)
    for (int i = 0; i < TASK_TYPE_MAX; i++) {
        if (task_handles[i] != NULL) {
            vTaskSuspend(task_handles[i]);
            task_suspended[i] = true;
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(backoff_time));
    
    // Resume tasks
    for (int i = 0; i < TASK_TYPE_MAX; i++) {
        if (task_suspended[i] && task_handles[i] != NULL) {
            vTaskResume(task_handles[i]);
            task_suspended[i] = false;
        }
    }
    
    // Increment backoff stage
    if (http_backoff_stage < 2) {
        http_backoff_stage++;
    }
}

// Reset backoff stage to initial (called on successful HTTP communication)
void reset_http_backoff(void)
{
    if (http_backoff_stage > 0) {
        ESP_LOGI(TAG, "Reseteando HTTP backoff a nivel inicial");
        http_backoff_stage = 0;
    }
}

// Cola para recibir mensajes del supervisor (errores, heartbeats, status)
QueueHandle_t supervisor_queue_global = NULL;

// Colas de comunicación entre tareas (compartidas)
static QueueHandle_t shared_sensor_queue = NULL;
static QueueHandle_t shared_error_queue = NULL;

// Colas para actualización de configuración de sensores en tiempo real
static QueueHandle_t humidity_config_queue = NULL;
static QueueHandle_t light_config_queue = NULL;

// Funciones para acceder a las colas de configuración desde otras tareas
QueueHandle_t get_humidity_config_queue(void) {
    return humidity_config_queue;
}

QueueHandle_t get_light_config_queue(void) {
    return light_config_queue;
}

// Función para enviar heartbeat al supervisor
void task_send_heartbeat(task_type_t task_type, const char *message)
{
    if (supervisor_queue_global != NULL) {
        supervisor_message_t msg = {
            .type = SUPERVISOR_MSG_HEARTBEAT,
            .task_type = task_type,
            .error_code = TASK_ERROR_NONE,
            .timestamp = xTaskGetTickCount(),
            .task_free_stack = uxTaskGetStackHighWaterMark(NULL),
            .heap_free = esp_get_free_heap_size()
        };
        strncpy(msg.message, message ? message : "Heartbeat", sizeof(msg.message) - 1);
        msg.message[sizeof(msg.message) - 1] = '\0';
        
        xQueueSend(supervisor_queue_global, &msg, 0);
    }
}

// Función para reportar error al supervisor
void task_report_error(task_type_t task_type, task_error_t error_code, const char *message)
{
    if (supervisor_queue_global != NULL) {
        supervisor_message_t msg = {
            .type = SUPERVISOR_MSG_ERROR_REPORT,
            .task_type = task_type,
            .error_code = error_code,
            .timestamp = xTaskGetTickCount(),
            .task_free_stack = uxTaskGetStackHighWaterMark(NULL),
            .heap_free = esp_get_free_heap_size()
        };
        strncpy(msg.message, message ? message : "Error", sizeof(msg.message) - 1);
        msg.message[sizeof(msg.message) - 1] = '\0';
        
        xQueueSend(supervisor_queue_global, &msg, 0);
    }
}

// Función para enviar status al supervisor
void task_send_status(task_type_t task_type, const char *message)
{
    if (supervisor_queue_global != NULL) {
        supervisor_message_t msg = {
            .type = SUPERVISOR_MSG_STATUS_UPDATE,
            .task_type = task_type,
            .error_code = TASK_ERROR_NONE,
            .timestamp = xTaskGetTickCount(),
            .task_free_stack = uxTaskGetStackHighWaterMark(NULL),
            .heap_free = esp_get_free_heap_size()
        };
        strncpy(msg.message, message ? message : "Status", sizeof(msg.message) - 1);
        msg.message[sizeof(msg.message) - 1] = '\0';
        
        xQueueSend(supervisor_queue_global, &msg, 0);
    }
}

// Tarea principal del supervisor
void task_main_supervisor(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO SUPERVISOR PRINCIPAL ===");
    
    // Recibir las colas pasadas desde main
    struct {
        QueueHandle_t sensor_q;
        QueueHandle_t error_q;
    } *queues = (typeof(queues))pvParameters;
    
    shared_sensor_queue = queues->sensor_q;
    shared_error_queue = queues->error_q;
    
    // Crear cola del supervisor
    supervisor_queue_global = xQueueCreate(10, sizeof(supervisor_message_t));
    if (supervisor_queue_global == NULL) {
        ESP_LOGE(TAG, "Error creando cola del supervisor");
        esp_restart();
    }
    
    // Crear colas de actualización de configuración de sensores (tamaño 1)
    humidity_config_queue = xQueueCreate(1, sizeof(config_update_message_t));
    if (humidity_config_queue == NULL) {
        ESP_LOGE(TAG, "Error creando cola de config de humedad");
        esp_restart();
    }
    
    light_config_queue = xQueueCreate(1, sizeof(config_update_message_t));
    if (light_config_queue == NULL) {
        ESP_LOGE(TAG, "Error creando cola de config de luz");
        esp_restart();
    }
    
    ESP_LOGI(TAG, "✓ Colas de configuración creadas (tamaño: 1)");
    
    // Inicializar LED de estado
    init_status_led();
    
    // Inicializar queue del LED de estado
    init_led_status_queue();
    
    // Crear semáforos de sincronización
    init_config_semaphore = xSemaphoreCreateBinary();
    wifi_init_semaphore = xSemaphoreCreateBinary();
    sensor_config_semaphore = xSemaphoreCreateBinary();
    system_ready_semaphore = xSemaphoreCreateBinary();
    
    // Estado inicial
    send_led_status(SYSTEM_STATE_INIT, "Iniciando sistema");
    
    // Crear tarea de LED de estado (alta prioridad)
    ESP_LOGI(TAG, "Creando tarea LED de estado...");
    BaseType_t result = xTaskCreate(task_led_status, "led_status",
                                   2048, NULL, 4, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea LED de estado");
        esp_restart();
    }
    
    // Crear tarea de configuración inicial
    ESP_LOGI(TAG, "Creando tarea de configuración inicial...");
    result = xTaskCreate(task_initial_config, "initial_config",
                        4096, NULL, 3, &task_handles[TASK_TYPE_INITIAL_CONFIG]);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea de configuración inicial");
        esp_restart();
    }
    
    // Esperar que termine la configuración inicial
    ESP_LOGI(TAG, "Esperando finalización de configuración inicial...");
    if (xSemaphoreTake(init_config_semaphore, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout esperando configuración inicial");
        send_led_status(SYSTEM_STATE_ERROR, "Config timeout");
        esp_restart();
    }
    ESP_LOGI(TAG, "✓ Configuración inicial completada");
    
    // Crear tarea WiFi
    ESP_LOGI(TAG, "Creando tarea WiFi...");
    send_led_status(SYSTEM_STATE_WIFI, "Conectando WiFi");
    result = xTaskCreate(task_wifi_connection, "wifi_task",
                        4096, NULL, 3, &task_handles[TASK_TYPE_WIFI]);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea WiFi");
        esp_restart();
    }
    
    // Esperar conexión WiFi
    ESP_LOGI(TAG, "Esperando conexión WiFi...");
    if (xSemaphoreTake(wifi_init_semaphore, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout conectando WiFi");
        send_led_status(SYSTEM_STATE_ERROR, "WiFi timeout");
        // No reiniciar, seguir intentando
    } else {
        ESP_LOGI(TAG, "✓ WiFi conectado exitosamente");
    }
    
    // Crear tarea de configuración de sensores
    ESP_LOGI(TAG, "Creando tarea de configuración de sensores...");
    send_led_status(SYSTEM_STATE_CONFIG, "Config sensores");
    result = xTaskCreate(task_sensor_config_init, "sensor_config",
                        4096, NULL, 3, &task_handles[TASK_TYPE_SENSOR_CONFIG]);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea de configuración de sensores");
        esp_restart();
    }
    
    // Esperar configuración de sensores
    ESP_LOGI(TAG, "Esperando configuración de sensores...");
    if (xSemaphoreTake(sensor_config_semaphore, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timeout configurando sensores, usando valores por defecto");
    } else {
        ESP_LOGI(TAG, "✓ Sensores configurados exitosamente");
    }
    
    // Crear tarea unificada de sensores
    ESP_LOGI(TAG, "Creando tarea unificada de sensores...");
    ESP_LOGI(TAG, "  - Lectura cada %d ms", SENSOR_READING_INTERVAL_MS);
    ESP_LOGI(TAG, "  - Envío HTTP según interval_seconds (configurado por MQTT)");
    
    result = xTaskCreate(task_sensors_unified_reading, "sensors_unified",
                        4096, shared_sensor_queue, 3, &task_handles[TASK_TYPE_SENSOR]);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea unificada de sensores");
        esp_restart();
    }
    ESP_LOGI(TAG, "✓ Tarea unificada de sensores creada");
    
    // Crear tarea HTTP
    ESP_LOGI(TAG, "Creando tarea HTTP...");
    result = xTaskCreate(task_http_client, "http_task",
                        6144, shared_sensor_queue, 2, &task_handles[TASK_TYPE_HTTP]);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea HTTP");
        esp_restart();
    }
    
    // Crear tarea MQTT
    ESP_LOGI(TAG, "Creando tarea MQTT...");
    result = xTaskCreate(task_mqtt_client, "mqtt_task",
                        4096, NULL, 2, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea MQTT");
        // No reiniciar, MQTT es complementario
    } else {
        ESP_LOGI(TAG, "✓ Tarea MQTT creada exitosamente");
    }
    
    // Crear tarea NVS
    ESP_LOGI(TAG, "Creando tarea NVS...");
    result = xTaskCreate(task_nvs_config, "nvs_task",
                        3072, NULL, 1, &task_handles[TASK_TYPE_NVS]);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea NVS");
        esp_restart();
    }
    
    // Sistema listo
    ESP_LOGI(TAG, "✓ Sistema completamente inicializado");
    send_led_status(SYSTEM_STATE_READY, "Sistema listo");
    xSemaphoreGive(system_ready_semaphore);
    
    // Loop principal del supervisor
    supervisor_message_t received_msg;
    uint32_t last_heartbeat_check = xTaskGetTickCount();
    
    while (1) {
        // Procesar mensajes con timeout
        if (xQueueReceive(supervisor_queue_global, &received_msg, pdMS_TO_TICKS(5000)) == pdTRUE) {
            // Procesar mensaje recibido
            switch (received_msg.type) {
                case SUPERVISOR_MSG_ERROR_REPORT:
                    ESP_LOGE(TAG, "Error reportado por tarea %d: %s", 
                            received_msg.task_type, received_msg.message);
                    send_led_status(SYSTEM_STATE_ERROR, "Error detectado");
                    break;
                    
                case SUPERVISOR_MSG_HEARTBEAT:
                    ESP_LOGD(TAG, "Heartbeat de tarea %d: %s", 
                            received_msg.task_type, received_msg.message);
                    
                    // Cambiar LED según el tipo de tarea
                    if (received_msg.task_type == TASK_TYPE_SENSOR) {
                        send_led_status(SYSTEM_STATE_SENSOR_READ, "Leyendo sensores");
                    } else if (received_msg.task_type == TASK_TYPE_HTTP) {
                        send_led_status(SYSTEM_STATE_HTTP_SEND, "Enviando datos");
                    }
                    break;
                    
                case SUPERVISOR_MSG_STATUS_UPDATE:
                    ESP_LOGI(TAG, "Status de tarea %d: %s", 
                            received_msg.task_type, received_msg.message);
                    break;
            }
        }
        
        // Watchdog periódico cada 30 segundos
        uint32_t current_time = xTaskGetTickCount();
        if ((current_time - last_heartbeat_check) > pdMS_TO_TICKS(30000)) {
            ESP_LOGI(TAG, "Supervisor activo - Heap libre: %lu bytes", 
                    (unsigned long)esp_get_free_heap_size());
            last_heartbeat_check = current_time;
            
            // Enviar estado Ready si no hay errores (volver a verde después de actividad)
            send_led_status(SYSTEM_STATE_READY, "Sistema OK");
        }
        
        // Yield para otras tareas
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
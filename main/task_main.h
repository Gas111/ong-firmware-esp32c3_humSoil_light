#ifndef TASK_MAIN_H
#define TASK_MAIN_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "led_strip.h"

// LED Neopixel global compartido entre tareas
extern led_strip_handle_t g_led_strip;

// Estados del sistema para el LED de estado
typedef enum
{
    SYSTEM_STATE_INIT,           // Inicialización
    SYSTEM_STATE_WIFI,           // Conectando WiFi
    SYSTEM_STATE_CONFIG,         // Configurando sensores
    SYSTEM_STATE_READY,          // Sistema listo
    SYSTEM_STATE_SENSOR_READ,    // Leyendo sensor
    SYSTEM_STATE_HTTP_SEND,      // Enviando datos HTTP
    SYSTEM_STATE_ERROR,          // Error crítico
    SYSTEM_STATE_ESPERANDO_WIFI, // Esperando WiFi
    SYSTEM_STATE_PROVISIONING,   // Provisioning Bluetooth
    SYSTEM_STATE_WARNING,        // Advertencia no crítica
    SYSTEM_STATE_MAX
} system_state_t;

// Estructura para mensajes de estado del LED
typedef struct
{
    system_state_t state;
    uint32_t timestamp;
    char message[32];
} led_status_message_t;

// Funciones para el LED de estado
void init_led_status_queue(void);
void send_led_status(system_state_t state, const char *message);
QueueHandle_t get_led_status_queue(void);
void task_led_status(void *pvParameters);

// Semáforos para sincronización de inicialización secuencial
extern SemaphoreHandle_t init_config_semaphore;
extern SemaphoreHandle_t wifi_init_semaphore;
extern SemaphoreHandle_t sensor_config_semaphore;
extern SemaphoreHandle_t system_ready_semaphore;

// Tipos de tareas del sistema
typedef enum
{
    TASK_TYPE_INITIAL_CONFIG,
    TASK_TYPE_WIFI,
    TASK_TYPE_SENSOR_CONFIG,
    TASK_TYPE_SENSOR,
    TASK_TYPE_HTTP,
    TASK_TYPE_NVS,
    TASK_TYPE_MAX
} task_type_t;

// Tipos de mensajes que pueden enviarse al supervisor
typedef enum
{
    SUPERVISOR_MSG_ERROR_REPORT,
    SUPERVISOR_MSG_HEARTBEAT,
    SUPERVISOR_MSG_STATUS_UPDATE
} supervisor_msg_type_t;

// Tipos de errores que pueden reportar las tareas
typedef enum
{
    TASK_ERROR_NONE,
    TASK_ERROR_ADC_INIT_FAILED,
    TASK_ERROR_SENSOR_NOT_FOUND,
    TASK_ERROR_WIFI_CONNECTION_FAILED,
    TASK_ERROR_MEMORY_ALLOCATION_FAILED,
    TASK_ERROR_QUEUE_FULL,
    TASK_ERROR_TIMEOUT,
    TASK_ERROR_TASK_CRASHED,
    TASK_ERROR_UNEXPECTED_TERMINATION,
    TASK_ERROR_HARDWARE,
    TASK_ERROR_SENSOR_READ,
    TASK_ERROR_UNKNOWN
} task_error_t;

// Estructura para mensajes al supervisor
typedef struct
{
    supervisor_msg_type_t type;
    task_type_t task_type;
    task_error_t error_code;
    uint32_t timestamp;
    char message[64];
    uint32_t task_free_stack;
    uint32_t heap_free;
} supervisor_message_t;

// Variables globales para configuración dinámica
extern uint32_t sensor_sampling_interval_ms;
extern uint32_t sensor_post_interval_ms;
extern float sensor_adc_min;
extern float sensor_adc_max;

// Función principal del supervisor
void task_main_supervisor(void *pvParameters);

// Funciones de utilidad para las tareas
void task_send_heartbeat(task_type_t task_type, const char *message);
void task_report_error(task_type_t task_type, task_error_t error_code, const char *message);
void task_send_status(task_type_t task_type, const char *message);

// Funciones de control de backoff HTTP
void pause_all_tasks_with_backoff(void);
void reset_http_backoff(void);

// Cola global del supervisor
extern QueueHandle_t supervisor_queue_global;

// Funciones para acceder a colas de configuración de sensores
QueueHandle_t get_humidity_config_queue(void);
QueueHandle_t get_light_config_queue(void);

#endif // TASK_MAIN_H
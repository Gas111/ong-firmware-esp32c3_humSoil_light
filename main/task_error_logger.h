#ifndef TASK_ERROR_LOGGER_H
#define TASK_ERROR_LOGGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

// Tipos de fuente de error (según backend)
typedef enum {
    ERROR_SOURCE_SENSOR = 0,
    ERROR_SOURCE_CONTROLLER = 1,
    ERROR_SOURCE_ACTUATOR = 2,
    ERROR_SOURCE_SYSTEM = 3
} error_source_type_t;

// Severidad del error (según backend)
typedef enum {
    ERROR_SEVERITY_INFO = 0,
    ERROR_SEVERITY_WARNING = 1,
    ERROR_SEVERITY_ERROR = 2,
    ERROR_SEVERITY_CRITICAL = 3
} error_severity_t;

// Estructura de error para la cola
typedef struct {
    error_source_type_t source_type;
    int id_sensor;                  // NULL si no es sensor (-1)
    int id_controller_station;      // NULL si no es controller (-1)
    int id_actuator;                // NULL si no es actuator (-1)
    char error_code[50];
    error_severity_t severity;
    char message[256];
    char details_json[512];         // JSON string con detalles adicionales
    char ip_address[46];            // IPv6 max length
    char device_serial[32];
    uint32_t timestamp;             // Timestamp cuando ocurrió el error
    bool pending;                   // true si aún no se ha enviado
} error_log_entry_t;

/**
 * @brief Inicializar el sistema de logging de errores
 * 
 * @return ESP_OK si todo está bien
 */
esp_err_t error_logger_init(void);

/**
 * @brief Registrar un error en la cola
 * 
 * @param error Estructura con los datos del error
 * @return ESP_OK si se agregó correctamente a la cola
 */
esp_err_t error_logger_log(const error_log_entry_t *error);

/**
 * @brief Helper para crear log de error de sensor
 */
esp_err_t error_logger_log_sensor(
    int id_sensor,
    const char *error_code,
    error_severity_t severity,
    const char *message,
    const char *details_json,
    const char *device_serial
);

/**
 * @brief Helper para crear log de error de sistema
 */
esp_err_t error_logger_log_system(
    const char *error_code,
    error_severity_t severity,
    const char *message,
    const char *details_json
);

/**
 * @brief Obtener el número de errores pendientes en la cola
 */
int error_logger_get_pending_count(void);

/**
 * @brief Forzar reintento inmediato de errores pendientes (llamar cuando WiFi reconecta)
 */
void error_logger_trigger_retry(void);

/**
 * @brief Tarea que procesa y envía errores al backend
 */
void task_error_logger(void *pvParameters);

#endif // TASK_ERROR_LOGGER_H

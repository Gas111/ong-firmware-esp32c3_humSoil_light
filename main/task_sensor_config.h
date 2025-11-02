#ifndef TASK_SENSOR_CONFIG_H
#define TASK_SENSOR_CONFIG_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

// Estructura para almacenar la configuración de un sensor individual
typedef struct {
    int id_sensor;           // ID del sensor
    char description[64];    // Descripción del sensor
    int interval_s;          // Intervalo de lectura en segundos
    bool state;              // Si el sensor está habilitado
    bool config_loaded;      // Si la configuración fue cargada exitosamente
} sensor_config_t;

// Configuraciones para cada sensor
extern sensor_config_t g_sensor_humidity_config;  // Configuración sensor humedad
extern sensor_config_t g_sensor_light_config;     // Configuración sensor luz

/**
 * @brief Tarea de configuración de sensores
 * 
 * Esta tarea se ejecuta una vez al inicio para:
 * - Obtener configuración del sensor desde el servidor
 * - Validar y almacenar la configuración
 * - Notificar al supervisor cuando termine
 * 
 * @param pvParameters No utilizado
 */
void task_sensor_config_init(void *pvParameters);

// Refresh sensor configuration from backend on-demand.
// Returns ESP_OK if sensor config was loaded successfully.
esp_err_t sensor_config_refresh(void);

#endif // TASK_SENSOR_CONFIG_H
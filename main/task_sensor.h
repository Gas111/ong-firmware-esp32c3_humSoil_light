#ifndef TASK_SENSOR_H
#define TASK_SENSOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// Tipo de sensor
typedef enum {
    SENSOR_TYPE_SOIL_HUMIDITY = 0,
    SENSOR_TYPE_LIGHT = 1,
    SENSOR_TYPE_UNKNOWN = 255
} sensor_type_t;

// Estructura para datos de sensores ADC
typedef struct {
    sensor_type_t type;     // Tipo de sensor
    float adc_voltage;      // Voltaje leído del ADC en mV
    int raw_value;          // Valor crudo del ADC (0-4095)
    float converted_value;  // Valor convertido (HS% para humedad, lux para luz)
    uint32_t timestamp;     // Timestamp de la lectura
    bool valid;             // Si la lectura es válida
} sensor_data_t;

// Estructura para actualización de configuración en tiempo real vía MQTT
typedef struct {
    sensor_type_t type;     // Tipo de sensor afectado
    int new_interval_s;     // Nuevo intervalo de lectura en segundos
    bool update_interval;   // true = actualizar intervalo de lectura
} config_update_message_t;

/**
 * @brief Tarea de lectura de sensores ADC
 * 
 * Esta tarea ejecuta continuamente y se encarga de:
 * - Leer datos del ADC oneshot en GPIO0
 * - Convertir valores crudos a voltaje
 * - Validar los datos recibidos
 * - Enviar los datos a través de una cola
 * - Mantener estadísticas de lectura
 * 
 * @param pvParameters Puntero a la cola donde enviar los datos (QueueHandle_t)
 */
void task_sensor_reading(void *pvParameters);

#endif // TASK_SENSOR_H
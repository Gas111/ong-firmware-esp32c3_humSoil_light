#ifndef TASK_SENSOR_READER_H
#define TASK_SENSOR_READER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/**
 * @brief Tarea unificada de lectura de sensores
 * 
 * Esta tarea lee TODOS los sensores cada SENSOR_READING_INTERVAL_MS (5 segundos)
 * y decide cuándo enviar datos al servidor según el interval_seconds de cada sensor
 * 
 * @param pvParameters Estructura con colas de configuración
 */
void task_sensor_reader(void *pvParameters);

#endif // TASK_SENSOR_READER_H

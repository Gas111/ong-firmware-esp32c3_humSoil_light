#ifndef TASK_SENSORS_UNIFIED_H
#define TASK_SENSORS_UNIFIED_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/**
 * @brief Tarea unificada que lee TODOS los sensores cada SENSOR_READING_INTERVAL_MS
 * 
 * Lee ambos sensores (humedad y luz) cada 5 segundos y envía los datos
 * a una cola compartida. La tarea HTTP decide cuándo enviar según interval_seconds.
 * 
 * @param pvParameters Puntero a la cola de datos de sensores (QueueHandle_t)
 */
void task_sensors_unified_reading(void *pvParameters);

#endif // TASK_SENSORS_UNIFIED_H

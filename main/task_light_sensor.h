#ifndef TASK_LIGHT_SENSOR_H
#define TASK_LIGHT_SENSOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "task_sensor.h"

// Función de la tarea
void task_light_sensor_reading(void *pvParameters);

#endif // TASK_LIGHT_SENSOR_H

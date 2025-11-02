#ifndef TASK_SOIL_HUMIDITY_H
#define TASK_SOIL_HUMIDITY_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "task_sensor.h"

// Función de la tarea
void task_soil_humidity_reading(void *pvParameters);

#endif // TASK_SOIL_HUMIDITY_H

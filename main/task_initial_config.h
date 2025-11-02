#ifndef TASK_INITIAL_CONFIG_H
#define TASK_INITIAL_CONFIG_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief Tarea de configuración inicial del sistema
 * 
 * Esta tarea se ejecuta una sola vez al inicio del sistema y luego se elimina.
 * Se encarga de:
 * - Inicializar NVS Flash
 * - Configurar el ADC para ESP32-C3
 * - Realizar verificaciones básicas del sistema
 * 
 * @param pvParameters Parámetros de la tarea (no utilizados)
 */
void task_initial_config(void *pvParameters);

#endif // TASK_INITIAL_CONFIG_H
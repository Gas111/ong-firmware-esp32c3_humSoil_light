#ifndef TASK_WIFI_H
#define TASK_WIFI_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/**
 * @brief Tarea de conexión WiFi
 * 
 * Esta tarea se ejecuta una vez para establecer la conexión WiFi y luego se elimina.
 * Se encarga de:
 * - Inicializar el stack WiFi
 * - Configurar los parámetros de conexión
 * - Intentar conectar a la red especificada
 * - Reportar el resultado de la conexión
 * 
 * @param pvParameters Parámetros de la tarea (no utilizados)
 */
void task_wifi_connection(void *pvParameters);

#endif // TASK_WIFI_H
#ifndef TASK_HTTP_H
#define TASK_HTTP_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "config.h"

// Función principal de la tarea HTTP
void task_http_client(void *pvParameters);

#endif // TASK_HTTP_H
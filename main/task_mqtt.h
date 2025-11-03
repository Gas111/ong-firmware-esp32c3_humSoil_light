#ifndef TASK_MQTT_H
#define TASK_MQTT_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Inicializa el cliente MQTT
 * 
 * Configura el cliente MQTT con el broker, credenciales y topics
 * Se suscribe a los topics de configuración de sensores
 * 
 * @return ESP_OK si se inicializó correctamente
 */
esp_err_t mqtt_client_init(void);

/**
 * @brief Publica el estado del dispositivo al servidor
 * 
 * Envía un mensaje de heartbeat con información del dispositivo
 * 
 * @param status Estado del dispositivo (ej: "online", "offline")
 * @return ESP_OK si se publicó correctamente
 */
esp_err_t mqtt_publish_status(const char *status);

/**
 * @brief Verifica si el cliente MQTT está conectado
 * 
 * @return true si está conectado, false en caso contrario
 */
bool mqtt_is_connected(void);

/**
 * @brief Tarea principal del cliente MQTT
 * 
 * Gestiona la conexión MQTT, reintentos automáticos y reconexiones
 * Publica heartbeats periódicos al servidor
 * 
 * @param pvParameters No utilizado
 */
void task_mqtt_client(void *pvParameters);

#endif // TASK_MQTT_H

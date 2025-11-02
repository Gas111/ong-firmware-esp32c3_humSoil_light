#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "config.h"
#include "task_main.h"
#include "task_sensor.h"
#include "task_soil_humidity.h"
#include "task_light_sensor.h"

static const char *TAG = "MAIN";

// Colas de comunicación entre tareas - UNA SOLA COLA PARA AMBOS SENSORES
static QueueHandle_t sensor_queue;
static QueueHandle_t error_queue;

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "=== ONG SENSOR APPLICATION v1.0 ===");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-C3 - Dual sensor monitoring");
    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned int)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "========================================");

    // Crear colas de comunicación entre tareas
    ESP_LOGI(TAG, "Creando colas de comunicación...");
    sensor_queue = xQueueCreate(SENSOR_QUEUE_SIZE, sizeof(sensor_data_t));
    error_queue = xQueueCreate(ERROR_QUEUE_SIZE, sizeof(supervisor_message_t));

    if (sensor_queue == NULL || error_queue == NULL) {
        ESP_LOGE(TAG, "Error creando colas de comunicación");
        esp_restart();
    }
    ESP_LOGI(TAG, "✓ Colas creadas correctamente");

    // Crear estructura para pasar las colas al supervisor
    struct {
        QueueHandle_t sensor_q;
        QueueHandle_t error_q;
    } queues = {
        .sensor_q = sensor_queue,
        .error_q = error_queue
    };

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "=== INICIANDO SUPERVISOR DEL SISTEMA ===");
    ESP_LOGI(TAG, "========================================");

    // Crear la tarea principal del supervisor (prioridad máxima)
    ESP_LOGI(TAG, "Creando tarea supervisor...");
    BaseType_t result = xTaskCreate(task_main_supervisor, "main_supervisor",
                                   4096, &queues, 5, NULL);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Error creando tarea supervisor");
        esp_restart();
    }

    ESP_LOGI(TAG, "✓ Tarea supervisor creada correctamente");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "✓ SISTEMA SUPERVISADO EN FUNCIONAMIENTO");
    ESP_LOGI(TAG, "========================================");

    // La tarea main termina aquí, el supervisor toma el control
    vTaskDelete(NULL);
}

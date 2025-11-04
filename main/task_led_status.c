#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "led_strip.h"
#include "task_main.h"
#include "config.h"

static const char *TAG = "TASK_LED_STATUS";

// Duración en ms que el LED se fuerza a INIT (amarillo) al arrancar
#define INIT_HOLD_MS 4000
// Queue para recibir estados del sistema
static QueueHandle_t led_status_queue = NULL;

// Forzar INIT al arranque hasta forced_init_until (ms desde boot)
static uint32_t forced_init_until = 0;
// Colores asignados a cada estado del sistema (formato RGB: {R,G,B})
// Nota: los índices deben coincidir con el enum 'system_state_t' en task_main.h
static const uint8_t state_colors[SYSTEM_STATE_MAX][3] = {

    [SYSTEM_STATE_INIT] = {255, 255, 0},         // Amarillo - Inicialización
    [SYSTEM_STATE_WIFI] = {0, 0, 255},           // Azul - Conectando WiFi
    [SYSTEM_STATE_CONFIG] = {0, 0, 255},         // Azul - Configurando sensores
    [SYSTEM_STATE_READY] = {0, 255, 0},          // Verde - Sistema listo
    [SYSTEM_STATE_SENSOR_READ] = {0, 255, 0},    // Verde - Leyendo sensor
    [SYSTEM_STATE_HTTP_SEND] = {0, 255, 0},      // Verde - Enviando HTTP
    [SYSTEM_STATE_ERROR] = {255, 0, 0},          // Rojo - Error crítico
    [SYSTEM_STATE_ESPERANDO_WIFI] = {0, 0, 255}, // Azul - Esperando WiFi
    [SYSTEM_STATE_PROVISIONING] = {0, 255, 0},   // Verde - Provisioning
    [SYSTEM_STATE_WARNING] = {200, 100, 0},      // Naranja - Advertencia
};
// Color order used by the LED strip. WS2812 typically uses GRB ordering.
// If your strip expects RGB, set this to 0. If GRB, set to 1.
// Para ESP32-C3 con WS2812, generalmente es GRB
#define LED_ORDER_GRB 0

// Helper to set pixel with configurable color order
static void set_strip_color(uint8_t r, uint8_t g, uint8_t b)
{
#if LED_ORDER_GRB
    // GRB order for WS2812
    led_strip_set_pixel(g_led_strip, 0, g, r, b);
#else
    // RGB order for other strips
    led_strip_set_pixel(g_led_strip, 0, r, g, b);
#endif
    led_strip_refresh(g_led_strip);
}

// Función para calcular el brillo basado en un latido sinusoidal
// Devuelve un valor en rango 0..255 para facilitar el mapeo directo a colores RGB.
static uint8_t calculate_heartbeat_brightness(uint32_t time_ms)
{
    // Configuración del latido
    const float heartbeat_freq_hz = 1.0f; // 1 Hz = 60 BPM
    const float min_brightness = 0.1f;    // 10% brillo mínimo
    const float max_brightness = 1.0f;    // 100% brillo máximo

    // Calcular fase del seno basada en el tiempo
    float phase = (time_ms / 1000.0f) * heartbeat_freq_hz * 2.0f * M_PI;

    // Función seno que va de -1 a +1
    float sine_wave = sinf(phase);

    // Convertir a rango de brillo (min_brightness a max_brightness)
    float brightness = min_brightness + (max_brightness - min_brightness) * (sine_wave + 1.0f) / 2.0f;

    // Convertir a rango 0-255
    uint8_t brightness_8bit = (uint8_t)(brightness * 255.0f);

    return brightness_8bit;
}

// Función para aplicar el efecto de latido al LED
static void apply_heartbeat_effect(system_state_t current_state)
{
    if (g_led_strip == NULL)
    {
        return; // LED no disponible
    }

    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Si estamos en período de inicialización forzada
    if (forced_init_until > 0 && current_time < forced_init_until)
    {
        current_state = SYSTEM_STATE_INIT;
    }

    // Validar estado
    if (current_state >= SYSTEM_STATE_MAX)
    {
        ESP_LOGW(TAG, "Estado LED inválido: %d", current_state);
        current_state = SYSTEM_STATE_ERROR;
    }

    // Obtener color base para el estado actual
    uint8_t base_r = state_colors[current_state][0];
    uint8_t base_g = state_colors[current_state][1];
    uint8_t base_b = state_colors[current_state][2];

    // Calcular brillo con efecto heartbeat
    uint8_t brightness = calculate_heartbeat_brightness(current_time);

    // Aplicar brillo al color base
    uint8_t final_r = (base_r * brightness) / 255;
    uint8_t final_g = (base_g * brightness) / 255;
    uint8_t final_b = (base_b * brightness) / 255;

    // Aplicar color al LED
    set_strip_color(final_r, final_g, final_b);

    // Log periódico para debug (cada 30 segundos)
    static uint32_t last_debug_log = 0;
    if ((current_time - last_debug_log) > 30000)
    {
        ESP_LOGD(TAG, "LED Estado: %d, RGB: (%d,%d,%d), Brillo: %d",
                 current_state, final_r, final_g, final_b, brightness);
        last_debug_log = current_time;
    }
}

// Tarea principal del LED de estado
void task_led_status(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA LED DE ESTADO ===");

    // Verificar que el LED esté inicializado
    if (g_led_strip == NULL)
    {
        ESP_LOGE(TAG, "LED strip no inicializado");
        vTaskDelete(NULL);
        return;
    }

    // Configurar período de inicialización forzada
    forced_init_until = (xTaskGetTickCount() * portTICK_PERIOD_MS) + INIT_HOLD_MS;

    led_status_message_t received_msg;
    system_state_t current_state = SYSTEM_STATE_INIT;

    ESP_LOGI(TAG, "✓ LED de estado listo - GPIO%d (D2)", LED_RGB_GPIO);

    while (1)
    {
        // Verificar si hay mensajes nuevos (no bloqueante)
        if (led_status_queue != NULL &&
            xQueueReceive(led_status_queue, &received_msg, pdMS_TO_TICKS(50)) == pdTRUE)
        {

            // Actualizar estado actual
            current_state = received_msg.state;

            ESP_LOGD(TAG, "Nuevo estado LED: %d - %s", current_state, received_msg.message);
        }

        // Aplicar efecto heartbeat al estado actual
        apply_heartbeat_effect(current_state);

        // Controlar velocidad de actualización del efecto
        vTaskDelay(pdMS_TO_TICKS(50)); // 20 FPS para suavidad del efecto
    }
}

// Función para inicializar la queue del LED de estado
void init_led_status_queue(void)
{
    if (led_status_queue == NULL)
    {
        led_status_queue = xQueueCreate(5, sizeof(led_status_message_t));
        if (led_status_queue == NULL)
        {
            ESP_LOGE(TAG, "Error creando queue del LED de estado");
        }
        else
        {
            ESP_LOGI(TAG, "✓ Queue del LED de estado creada");
        }
    }
}

// Función para enviar un estado al LED
void send_led_status(system_state_t state, const char *message)
{
    if (led_status_queue != NULL)
    {
        led_status_message_t msg = {
            .state = state,
            .timestamp = xTaskGetTickCount()};

        // Copiar mensaje si se proporciona
        if (message != NULL)
        {
            strncpy(msg.message, message, sizeof(msg.message) - 1);
            msg.message[sizeof(msg.message) - 1] = '\0';
        }
        else
        {
            msg.message[0] = '\0';
        }

        // Enviar mensaje (no bloqueante)
        if (xQueueSend(led_status_queue, &msg, 0) != pdTRUE)
        {
            // Queue llena, no es crítico
            ESP_LOGD(TAG, "Queue LED llena, mensaje descartado");
        }
    }
    else
    {
        ESP_LOGW(TAG, "Queue LED no inicializada");
    }
}

// Función para obtener la queue del LED de estado (para otras tareas)
QueueHandle_t get_led_status_queue(void)
{
    return led_status_queue;
}
#ifndef TASK_LED_STATUS_H
#define TASK_LED_STATUS_H

#include "task_main.h"

// Funciones para el LED de estado
void init_led_status_queue(void);
void send_led_status(system_state_t state, const char *message);
QueueHandle_t get_led_status_queue(void);
void task_led_status(void *pvParameters);

#endif // TASK_LED_STATUS_H
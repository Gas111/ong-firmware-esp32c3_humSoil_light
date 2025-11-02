#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define DEFAULT_WIFI_SSID "TeleCentro-5950"
#define DEFAULT_WIFI_PASS "12345678"
#define WIFI_MAXIMUM_RETRY 5

#define HTTP_SERVER_URL "https://ong-controller.vercel.app/api/v1/process-data"
#define HTTP_CONFIG_URL "https://ong-controller.vercel.app/api/v1/sensors/serial/"
#define HTTP_TIMEOUT_MS 20000
#define DEVICE_SERIAL_HUMIDITY "0x001C"
#define DEVICE_SERIAL_LIGHT "0x001D"

// ============= CONFIGURACIÓN ADC - XIAO ESP32-C3 =============
// Sensor de Humedad de Suelo - GPIO2 (D0)
#define SOIL_HUMIDITY_ADC_CHANNEL   ADC_CHANNEL_2    // GPIO2 - D0
#define SOIL_HUMIDITY_GPIO          2
#define SOIL_HUMIDITY_DRY_VALUE     2800    // Valor ADC cuando está seco (0% humedad)
#define SOIL_HUMIDITY_WET_VALUE     1200    // Valor ADC cuando está húmedo (100% humedad)

// Sensor de Luz - GPIO3 (D1)
#define LIGHT_SENSOR_ADC_CHANNEL    ADC_CHANNEL_3    // GPIO3 - D1
#define LIGHT_SENSOR_GPIO           3
#define LIGHT_SENSOR_DARK_VALUE     200     // Valor ADC en oscuridad (0 lux)
#define LIGHT_SENSOR_BRIGHT_VALUE   3800    // Valor ADC con luz brillante (máximo lux)
#define LIGHT_SENSOR_MAX_LUX        10000   // Lúmenes máximos del sensor

// Configuración general ADC
#define ADC_UNIT ADC_UNIT_1
#define ADC_ATTEN ADC_ATTEN_DB_12
#define ADC_BITWIDTH ADC_BITWIDTH_12

#define LED_RGB_GPIO 4
#define LED_NUMBERS 1
#define LED_BRIGHTNESS 50

#define SENSOR_READING_INTERVAL_MS 5000
#define SENSOR_POST_INTERVAL_MS_DEFAULT 60000
#define WATCHDOG_TIMEOUT_MS 30000

// Colas - UNA SOLA COLA COMPARTIDA PARA AMBOS SENSORES
#define SENSOR_QUEUE_SIZE 5          // Cola compartida para ambos sensores
#define ERROR_QUEUE_SIZE 5

#endif
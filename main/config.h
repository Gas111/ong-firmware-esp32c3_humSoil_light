#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define DEFAULT_WIFI_SSID "TeleCentro-5950"
#define DEFAULT_WIFI_PASS "12345678"
#define WIFI_MAXIMUM_RETRY 5

#define HTTP_SERVER_URL "https://ong-controller.vercel.app/api/v1/process-data"
#define HTTP_SERVER_BASE_URL "https://ong-controller.vercel.app/api/v1"
#define HTTP_CONFIG_URL "https://ong-controller.vercel.app/api/v1/sensors/serial/"
#define HTTP_TIMEOUT_MS 20000
#define DEVICE_SERIAL_HUMIDITY "0x001C"
#define DEVICE_SERIAL_LIGHT "0x001D"

// ============= CONFIGURACIÓN MQTT =============
// Broker HiveMQ Cloud con TLS (puerto 8883)
#define MQTT_BROKER_URL "mqtts://7b0acd8e8fb242379eb6c4983fc30401.s1.eu.hivemq.cloud"
#define MQTT_BROKER_PORT 8883
    #define MQTT_CLIENT_ID "7b0acd8e8fb242379eb6c4983fc30401"
#define MQTT_USERNAME "hivemq.webclient.1762137558937"
#define MQTT_PASSWORD "0Ql>XrtWH*6wo8k9D$:M"
#define MQTT_KEEPALIVE 120
#define MQTT_QOS 1
#define MQTT_RECONNECT_TIMEOUT_MS 5000

// Tópicos MQTT (los placeholders {serial} se reemplazan en tiempo de ejecución)
#define MQTT_TOPIC_CONFIG_HUMIDITY "ong/sensor/0x001C/config"
#define MQTT_TOPIC_CONFIG_LIGHT "ong/sensor/0x001D/config"
#define MQTT_TOPIC_STATUS "ong/sensor/status"

#define MQTT_MAX_TOPIC_LEN 128
#define MQTT_MAX_PAYLOAD_LEN 1024

// ============= CONFIGURACIÓN ADC - XIAO ESP32-C3 =============
// Sensor de Humedad de Suelo - GPIO2 (D0)
#define SOIL_HUMIDITY_ADC_CHANNEL ADC_CHANNEL_2 // GPIO2 - D0
#define SOIL_HUMIDITY_GPIO 2
#define SOIL_HUMIDITY_DRY_VALUE 2800 // Valor ADC cuando está seco (0% humedad)
#define SOIL_HUMIDITY_WET_VALUE 1200 // Valor ADC cuando está húmedo (100% humedad)

// Sensor de Luz - GPIO3 (D1)
#define LIGHT_SENSOR_ADC_CHANNEL ADC_CHANNEL_3 // GPIO3 - D1
#define LIGHT_SENSOR_GPIO 3
#define LIGHT_SENSOR_DARK_VALUE 200    // Valor ADC en oscuridad (0 lux)
#define LIGHT_SENSOR_BRIGHT_VALUE 3800 // Valor ADC con luz brillante (máximo lux)
#define LIGHT_SENSOR_MAX_LUX 10000     // Lúmenes máximos del sensor

// Configuración general ADC
#define ADC_UNIT ADC_UNIT_1
#define ADC_ATTEN ADC_ATTEN_DB_12
#define ADC_BITWIDTH ADC_BITWIDTH_12

#define LED_RGB_GPIO 4
#define LED_NUMBERS 1
#define LED_BRIGHTNESS 40

#define SENSOR_READING_INTERVAL_MS 5000
#define SENSOR_POST_INTERVAL_MS_DEFAULT 5000
#define WATCHDOG_TIMEOUT_MS 30000

// Colas - UNA SOLA COLA COMPARTIDA PARA AMBOS SENSORES
#define SENSOR_QUEUE_SIZE 5 // Cola compartida para ambos sensores
#define ERROR_QUEUE_SIZE 20 // Cola para supervisor de errores

#endif
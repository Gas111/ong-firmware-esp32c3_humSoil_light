#ifndef ADC_SHARED_H
#define ADC_SHARED_H

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_err.h"

// Handle global del ADC compartido
extern adc_oneshot_unit_handle_t g_adc_handle;
extern adc_cali_handle_t g_adc_cali_handle;
extern bool g_adc_calibration_enabled;

/**
 * @brief Inicializar ADC compartido para todos los sensores
 *
 * Esta función debe ser llamada una sola vez al inicio del sistema
 * antes de crear las tareas de sensores.
 *
 * @return ESP_OK si la inicialización fue exitosa
 */
esp_err_t init_shared_adc(void);

/**
 * @brief Configurar canal ADC para un sensor específico
 *
 * @param channel Canal ADC a configurar
 * @param atten Atenuación del ADC
 * @return ESP_OK si la configuración fue exitosa
 */
esp_err_t configure_adc_channel(adc_channel_t channel, adc_atten_t atten);

/**
 * @brief Leer valor ADC de un canal específico
 *
 * @param channel Canal ADC a leer
 * @param out_raw Puntero donde almacenar el valor crudo
 * @return ESP_OK si la lectura fue exitosa
 */
esp_err_t read_adc_channel(adc_channel_t channel, int *out_raw);

/**
 * @brief Convertir valor crudo ADC a voltaje usando calibración
 *
 * @param raw_value Valor crudo ADC
 * @param out_voltage_mv Puntero donde almacenar el voltaje en mV
 * @return ESP_OK si la conversión fue exitosa
 */
esp_err_t convert_adc_to_voltage(int raw_value, int *out_voltage_mv);

#endif // ADC_SHARED_H
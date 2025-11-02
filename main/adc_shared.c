#include "adc_shared.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "ADC_SHARED";

// Variables globales del ADC compartido
adc_oneshot_unit_handle_t g_adc_handle = NULL;
adc_cali_handle_t g_adc_cali_handle = NULL;
bool g_adc_calibration_enabled = false;

esp_err_t init_shared_adc(void)
{
    ESP_LOGI(TAG, "=== INICIALIZANDO ADC COMPARTIDO ===");

    // Configuración del ADC unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&init_config, &g_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando ADC unit: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✓ ADC unit inicializado correctamente");

    // Inicializar calibración
    ESP_LOGI(TAG, "Inicializando calibración ADC...");

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &g_adc_cali_handle);
    if (ret == ESP_OK) {
        g_adc_calibration_enabled = true;
        ESP_LOGI(TAG, "✓ Calibración ADC habilitada");
    } else {
        ESP_LOGW(TAG, "Calibración ADC no disponible, usando valores crudos: %s", esp_err_to_name(ret));
        g_adc_calibration_enabled = false;
    }

    ESP_LOGI(TAG, "✓ ADC compartido inicializado correctamente");
    return ESP_OK;
}

esp_err_t configure_adc_channel(adc_channel_t channel, adc_atten_t atten)
{
    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = atten,
    };

    esp_err_t ret = adc_oneshot_config_channel(g_adc_handle, channel, &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configurando canal ADC %d: %s", channel, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✓ Canal ADC %d configurado correctamente", channel);
    return ESP_OK;
}

esp_err_t read_adc_channel(adc_channel_t channel, int *out_raw)
{
    if (g_adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC handle no inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    if (out_raw == NULL) {
        ESP_LOGE(TAG, "Puntero out_raw es NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = adc_oneshot_read(g_adc_handle, channel, out_raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error leyendo canal ADC %d: %s", channel, esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t convert_adc_to_voltage(int raw_value, int *out_voltage_mv)
{
    if (!g_adc_calibration_enabled) {
        // Sin calibración, devolver valor crudo como voltaje aproximado
        *out_voltage_mv = raw_value;
        return ESP_OK;
    }

    if (g_adc_cali_handle == NULL) {
        ESP_LOGE(TAG, "Calibration handle no inicializado");
        return ESP_ERR_INVALID_STATE;
    }

    if (out_voltage_mv == NULL) {
        ESP_LOGE(TAG, "Puntero out_voltage_mv es NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = adc_cali_raw_to_voltage(g_adc_cali_handle, raw_value, out_voltage_mv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error convirtiendo ADC a voltaje: %s", esp_err_to_name(ret));
        *out_voltage_mv = raw_value; // Fallback
        return ret;
    }

    return ESP_OK;
}
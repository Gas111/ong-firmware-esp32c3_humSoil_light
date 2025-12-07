#include "task_bluetooth.h"
#include "task_main.h"
#include "task_led_status.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_bt_device.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "config.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "BLUETOOTH";

// Variables globales
static bluetooth_state_t current_state = BT_STATE_DISABLED;
static wifi_config_from_bt_t wifi_config = {0};
static bool config_received = false;

// BLE Configuration
#define DEVICE_NAME "ESP32_ONG_CONFIG"
#define BLE_ADV_DURATION 0  // Advertise indefinitely
#define BLE_ADV_INTERVAL_MIN 0x20
#define BLE_ADV_INTERVAL_MAX 0x40

// GATT Service and Characteristic UUIDs - CORREGIDOS
static uint8_t service_uuid[16] = {
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0
};

static uint8_t char_uuid[16] = {
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf1,
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf2  // CAMBIADO: último byte diferente
};

// Configuración de seguridad BLE
static esp_ble_auth_req_t auth_req = ESP_LE_AUTH_NO_BOND;     // Sin bonding para simplificar
static esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;              // Sin I/O capability
static uint8_t key_size = 16;
static uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
static uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

// GATT server variables
static uint16_t gatts_if = ESP_GATT_IF_NONE;
static uint16_t conn_id = 0;
static uint16_t service_handle = 0;
static uint16_t char_handle = 0;

// Advertising data - MEJORADO
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,        // Para mejor debugging
    .min_interval = BLE_ADV_INTERVAL_MIN,
    .max_interval = BLE_ADV_INTERVAL_MAX,
    .appearance = 0x0080,          // Generic Computer
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 16,
    .p_service_uuid = service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = BLE_ADV_INTERVAL_MIN,
    .adv_int_max = BLE_ADV_INTERVAL_MAX,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// Forward declarations
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
static void process_wifi_config(const char* data, size_t len);

// Funciones públicas
bluetooth_state_t bluetooth_get_state(void)
{
    return current_state;
}

bool bluetooth_has_wifi_config(void)
{
    return config_received;
}

esp_err_t bluetooth_get_wifi_config(char *ssid, char *password)
{
    if (!config_received) {
        return ESP_ERR_NOT_FOUND;
    }
    
    strncpy(ssid, wifi_config.ssid, 32);
    strncpy(password, wifi_config.password, 64);
    return ESP_OK;
}

void bluetooth_clear_wifi_config(void)
{
    memset(&wifi_config, 0, sizeof(wifi_config));
    config_received = false;
}

// Procesar datos de configuración WiFi recibidos
static void process_wifi_config(const char* data, size_t len)
{
    ESP_LOGI(TAG, "📱 Datos recibidos (%d bytes): %.*s", (int)len, (int)len, data);
    
    // Parse JSON: {"ssid":"MiWiFi","password":"mipassword"}
    cJSON *json = cJSON_ParseWithLength(data, len);
    if (json == NULL) {
        ESP_LOGE(TAG, "❌ Error parseando JSON");
        return;
    }
    
    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(json, "password");
    
    if (cJSON_IsString(ssid_item) && cJSON_IsString(pass_item)) {
        strncpy(wifi_config.ssid, cJSON_GetStringValue(ssid_item), sizeof(wifi_config.ssid) - 1);
        strncpy(wifi_config.password, cJSON_GetStringValue(pass_item), sizeof(wifi_config.password) - 1);
        wifi_config.received = true;
        config_received = true;
        
        ESP_LOGI(TAG, "✅ Configuración WiFi recibida:");
        ESP_LOGI(TAG, "   SSID: %s", wifi_config.ssid);
        ESP_LOGI(TAG, "   Password: %s", strlen(wifi_config.password) > 0 ? "***" : "(vacío)");
        
    // Enviar respuesta de confirmación
    const char* response = "{\"status\":\"OK\",\"message\":\"WiFi config received\"}";
    esp_ble_gatts_send_indicate(gatts_if, conn_id, char_handle, 
                   strlen(response), (uint8_t*)response, false);

    // Notificar a la tarea WiFi que la configuración está lista
    xEventGroupSetBits(g_connectivity_event_group, CONNECTIVITY_WIFI_CONFIGURED_BIT);

    current_state = BT_STATE_CONFIGURING;
    send_led_status(SYSTEM_STATE_CONFIG, "Config WiFi OK");
        
    } else {
        ESP_LOGE(TAG, "❌ JSON inválido - faltan campos ssid/password");
        const char* response = "{\"status\":\"ERROR\",\"message\":\"Invalid JSON format\"}";
        esp_ble_gatts_send_indicate(gatts_if, conn_id, char_handle, 
                                   strlen(response), (uint8_t*)response, false);
    }
    
    cJSON_Delete(json);
}

// GATT Server Event Handler
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if_param, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT:
            ESP_LOGI(TAG, "🔧 GATT Server registrado, interface: %d", gatts_if_param);
            gatts_if = gatts_if_param;
            
            // Set device name
            esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(DEVICE_NAME);
            if (set_dev_name_ret) {
                ESP_LOGE(TAG, "❌ Error setting device name: %s", esp_err_to_name(set_dev_name_ret));
            }
            
            // Configure advertising data
            esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
            if (ret) {
                ESP_LOGE(TAG, "❌ Error config adv data: %s", esp_err_to_name(ret));
            }
            
            // Create service
            esp_gatt_srvc_id_t service_id = {
                .is_primary = true,
                .id.inst_id = 0x00,
                .id.uuid.len = 16,
            };
            memcpy(service_id.id.uuid.uuid.uuid128, service_uuid, 16);
            
            esp_ble_gatts_create_service(gatts_if, &service_id, 4);
            break;
            
        case ESP_GATTS_CREATE_EVT:
            ESP_LOGI(TAG, "🔧 Servicio creado, handle: %d", param->create.service_handle);
            service_handle = param->create.service_handle;
            esp_ble_gatts_start_service(service_handle);
            
            // Add characteristic
            esp_bt_uuid_t char_uuid_bt = {
                .len = 16,
            };
            memcpy(char_uuid_bt.uuid.uuid128, char_uuid, 16);
            
            esp_ble_gatts_add_char(service_handle, &char_uuid_bt, 
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                   NULL, NULL);
            break;
            
        case ESP_GATTS_ADD_CHAR_EVT:
            ESP_LOGI(TAG, "🔧 Característica agregada, handle: %d", param->add_char.attr_handle);
            char_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "✅ Servidor GATT completamente configurado y listo");
            ESP_LOGI(TAG, "🔍 Service UUID: 12345678-9ABC-DEF0-1234-56789ABCDEF0");
            ESP_LOGI(TAG, "🔍 Char UUID: 12345678-9ABC-DEF1-1234-56789ABCDEF2");
            break;
            
        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "📱 Cliente BLE conectado, conn_id: %d", param->connect.conn_id);
            conn_id = param->connect.conn_id;
            current_state = BT_STATE_CONNECTED;
            send_led_status(SYSTEM_STATE_CONFIG, "BLE Connected");
            break;
            
        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "📱 Cliente BLE desconectado");
            current_state = BT_STATE_DISCOVERABLE;
            send_led_status(SYSTEM_STATE_CONFIG, "BLE Waiting");
            
            // Restart advertising
            esp_ble_gap_start_advertising(&adv_params);
            break;
            
        case ESP_GATTS_WRITE_EVT:
            ESP_LOGI(TAG, "📝 Datos escritos, handle: %d, len: %d", 
                     param->write.handle, param->write.len);
            
            if (param->write.handle == char_handle) {
                // Process WiFi configuration data
                process_wifi_config((char*)param->write.value, param->write.len);
            }
            break;
            
        default:
            break;
    }
}

// GAP Event Handler
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "🔧 Advertising data configurado");
            esp_ble_gap_start_advertising(&adv_params);
            break;
            
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "📡 Advertising iniciado exitosamente");
                ESP_LOGI(TAG, "🔍 Dispositivo visible como: %s", DEVICE_NAME);
                ESP_LOGI(TAG, "📱 Usa nRF Connect para conectar (NO la búsqueda Bluetooth del sistema)");
                current_state = BT_STATE_DISCOVERABLE;
                send_led_status(SYSTEM_STATE_CONFIG, "BLE Config Mode");
            } else {
                ESP_LOGE(TAG, "❌ Error iniciando advertising: %d", param->adv_start_cmpl.status);
            }
            break;
            
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            ESP_LOGI(TAG, "📡 Advertising detenido");
            break;
            
        default:
            break;
    }
}

// Inicializar Bluetooth
esp_err_t bluetooth_init(void)
{
    ESP_LOGI(TAG, "🔧 Inicializando Bluetooth...");
    
    // Initialize Bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "❌ Error init BT controller: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "❌ Error enable BT controller: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "❌ Error init bluedroid: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "❌ Error enable bluedroid: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configurar MTU BLE para permitir mensajes largos (por ejemplo, 128 bytes)
    ret = esp_ble_gatt_set_local_mtu(128);
    if (ret) {
        ESP_LOGE(TAG, "❌ Error configurando MTU BLE: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register callbacks
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "❌ Error register GAP callback: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "❌ Error register GATTS callback: %s", esp_err_to_name(ret));
        return ret;
    }
    
    current_state = BT_STATE_ENABLED;
    ESP_LOGI(TAG, "✅ Bluetooth inicializado exitosamente");
    return ESP_OK;
}

// Iniciar modo de configuración
esp_err_t bluetooth_start_config_mode(void)
{
    ESP_LOGI(TAG, "🔧 Iniciando modo de configuración...");
    
    if (current_state == BT_STATE_DISABLED) {
        esp_err_t ret = bluetooth_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    // Register GATT application
    esp_err_t ret = esp_ble_gatts_app_register(0);
    if (ret) {
        ESP_LOGE(TAG, "❌ Error register GATT app: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "📡 Modo configuración BLE activo");
    ESP_LOGI(TAG, "   Nombre: %s", DEVICE_NAME);
    ESP_LOGI(TAG, "   Esperando conexión de app móvil...");
    
    return ESP_OK;
}

// Detener modo de configuración
esp_err_t bluetooth_stop_config_mode(void)
{
    ESP_LOGI(TAG, "🔧 Deteniendo modo de configuración...");
    
    if (current_state >= BT_STATE_DISCOVERABLE) {
        esp_ble_gap_stop_advertising();
    }
    
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    
    current_state = BT_STATE_DISABLED;
    ESP_LOGI(TAG, "✅ Bluetooth deshabilitado");
    
    return ESP_OK;
}

// Tarea principal del Bluetooth
void task_bluetooth(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO TAREA BLUETOOTH ===");
    
    // Wait for initial system setup
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    while (1) {
        switch (current_state) {
            case BT_STATE_DISABLED:
                // Bluetooth disabled, wait for activation signal
                vTaskDelay(pdMS_TO_TICKS(5000));
                break;
                
            case BT_STATE_ENABLED:
            case BT_STATE_DISCOVERABLE:
            case BT_STATE_CONNECTED:
                // Bluetooth active, wait for configuration or timeout
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
                
            case BT_STATE_CONFIGURING:
                // Configuration completed, wait a bit then signal completion
                vTaskDelay(pdMS_TO_TICKS(2000));
                ESP_LOGI(TAG, "🎯 Configuración completada, deteniendo Bluetooth");
                bluetooth_stop_config_mode();
                break;
        }
        
        // Send periodic heartbeat
        if (current_state != BT_STATE_DISABLED) {
            const char* state_str = (current_state == BT_STATE_DISCOVERABLE) ? "Config Mode" :
                                  (current_state == BT_STATE_CONNECTED) ? "Connected" :
                                  (current_state == BT_STATE_CONFIGURING) ? "Configuring" : "Enabled";
            task_send_heartbeat(TASK_TYPE_BLUETOOTH, state_str);
        }
    }
}
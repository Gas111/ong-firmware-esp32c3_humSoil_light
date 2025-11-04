#include "task_wifi.h"
#include "task_main.h"
#include "task_error_logger.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "config.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_TASK";

// Credenciales WiFi (leídas de NVS o defaults)
static char wifi_ssid[32] = DEFAULT_WIFI_SSID;
static char wifi_pass[64] = DEFAULT_WIFI_PASS;

// Función para leer credenciales WiFi de NVS
static esp_err_t read_wifi_credentials_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir NVS para WiFi, usando defaults");
        return err;
    }

    size_t ssid_len = sizeof(wifi_ssid);
    err = nvs_get_str(nvs_handle, "ssid", wifi_ssid, &ssid_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SSID cargado de NVS: %s", wifi_ssid);
    } else {
        ESP_LOGW(TAG, "SSID no encontrado en NVS, usando default");
    }

    size_t pass_len = sizeof(wifi_pass);
    err = nvs_get_str(nvs_handle, "pass", wifi_pass, &pass_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Password cargado de NVS");
    } else {
        ESP_LOGW(TAG, "Password no encontrado en NVS, usando default");
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

// Función para guardar credenciales WiFi en NVS
static esp_err_t save_wifi_credentials_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS para escritura WiFi");
        return err;
    }

    err = nvs_set_str(nvs_handle, "ssid", ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando SSID en NVS");
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, "pass", pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error guardando password en NVS");
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return err;
}

// Event Groups
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static esp_netif_t *sta_netif = NULL; // Guardar referencia al netif

// Manejador de eventos WiFi
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi iniciado, intentando conectar...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Reintentando conexión WiFi (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
            send_led_status(SYSTEM_STATE_ESPERANDO_WIFI, "Reintentando");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Conexión WiFi falló después de %d intentos", WIFI_MAXIMUM_RETRY);
            send_led_status(SYSTEM_STATE_ERROR, "WiFi falló");
            
            // Notificar pérdida de conectividad
            xEventGroupClearBits(g_connectivity_event_group, CONNECTIVITY_WIFI_CONNECTED_BIT);
            ESP_LOGW(TAG, "⚠️ Conectividad perdida - Tareas pausadas");
            
            // NO llamar error_logger aquí - causa stack overflow en sys_evt task
            // El loop de monitoreo detectará y loggeará el error
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Dirección IP obtenida:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        send_led_status(SYSTEM_STATE_WIFI, "WiFi conectado");
        
        // Notificar conectividad global
        xEventGroupSetBits(g_connectivity_event_group, CONNECTIVITY_WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "✅ Conectividad establecida - Tareas reanudadas");
        
        // NO LLAMAR error_logger aquí - causa stack overflow en sys_evt
        // El log se enviará desde el loop principal de la tarea WiFi
    }
}

// Inicialización WiFi
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // Leer credenciales de NVS
    read_wifi_credentials_from_nvs();

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // Copiar SSID y password
    strncpy((char*)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi inicializado. Conectando a %s...", wifi_ssid);
}

// Tarea de conexión y monitoreo WiFi (se mantiene ejecutándose)
void task_wifi_connection(void *pvParameters)
{
    ESP_LOGI(TAG, "=== INICIANDO CONEXIÓN WIFI ===");
    
    // Inicializar WiFi
    wifi_init_sta();
    
    // Esperar conexión o fallo
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✓ Conectado exitosamente a WiFi SSID: %s", wifi_ssid);
        
        // Obtener información de la conexión
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "  - RSSI: %d dBm", ap_info.rssi);
            ESP_LOGI(TAG, "  - Canal: %d", ap_info.primary);
        }
        
        // Obtener IP
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            // Usar un solo log para reducir uso de stack
            ESP_LOGI(TAG, "  - IP: " IPSTR " | Mask: " IPSTR " | GW: " IPSTR,
                     IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
            
            // Enviar log INFO de primera conexión exitosa (AQUÍ SÍ HAY STACK SUFICIENTE)
            static bool first_connection = true;
            if (first_connection) {
                first_connection = false;
                char details[128];
                snprintf(details, sizeof(details), 
                         "{\"ip\":\""IPSTR"\",\"ssid\":\"%s\"}", 
                         IP2STR(&ip_info.ip), wifi_ssid);
                error_logger_log_system(
                    "WIFI_CONNECTED",
                    ERROR_SEVERITY_INFO,
                    "Conexión WiFi establecida exitosamente",
                    details
                );
            }
        }
        
        // Notificar al supervisor
        task_send_status(TASK_TYPE_WIFI, "WiFi conectado exitosamente");
        xSemaphoreGive(wifi_init_semaphore);
        
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "✗ Fallo conectando a WiFi SSID: %s", wifi_ssid);
        task_report_error(TASK_TYPE_WIFI, TASK_ERROR_WIFI_CONNECTION_FAILED, "WiFi connection failed");
        
        // Log de fallo de conexión inicial (aquí sí tenemos stack suficiente)
        char details[128];
        snprintf(details, sizeof(details), 
                 "{\"attempts\": %d, \"ssid\": \"%s\"}", 
                 WIFI_MAXIMUM_RETRY, wifi_ssid);
        error_logger_log_system(
            "WIFI_CONNECTION_FAILED",
            ERROR_SEVERITY_CRITICAL,
            "WiFi desconectado tras múltiples intentos",
            details
        );
        
        // NO bloqueamos aquí - continuamos al loop de monitoreo que reintentará periódicamente
        ESP_LOGW(TAG, "⏭️ Continuando al loop de monitoreo - reintentará cada 60s");
        
        // No damos el semáforo en caso de fallo inicial
    } else {
        ESP_LOGE(TAG, "✗ Evento WiFi inesperado");
    }
    
    // Loop de monitoreo WiFi - maneja reconexiones periódicas
    uint32_t last_reconnect_attempt = xTaskGetTickCount();
    uint32_t last_status_check = xTaskGetTickCount();
    bool was_connected = (bits & WIFI_CONNECTED_BIT) != 0;
    
    while (1) {
        // Verificar estado cada 30 segundos
        if ((xTaskGetTickCount() - last_status_check) > pdMS_TO_TICKS(30000)) {
            wifi_ap_record_t ap_info;
            esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
            
            if (ret == ESP_OK) {
                // Conectado
                if (!was_connected) {
                    ESP_LOGI(TAG, "✅ WiFi reconectado exitosamente");
                    send_led_status(SYSTEM_STATE_WIFI, "WiFi OK");
                    s_retry_num = 0; // Resetear contador de reintentos
                    was_connected = true;
                    
                    // Notificar reconexión
                    xEventGroupSetBits(g_connectivity_event_group, CONNECTIVITY_WIFI_CONNECTED_BIT);
                    ESP_LOGI(TAG, "▶️ Conectividad restablecida - Tareas reanudadas");
                    
                    // Forzar reintento de errores pendientes
                    error_logger_trigger_retry();
                    
                    // Registrar recuperación de conexión
                    char details[128];
                    snprintf(details, sizeof(details), "{\"ssid\": \"%s\", \"rssi\": %d}", 
                             wifi_ssid, ap_info.rssi);
                    error_logger_log_system(
                        "WIFI_RECONNECTED",
                        ERROR_SEVERITY_INFO,
                        "WiFi reconectado exitosamente",
                        details
                    );
                }
                
                // Enviar heartbeat con información de señal
                char heartbeat_msg[32];
                snprintf(heartbeat_msg, sizeof(heartbeat_msg), "WiFi RSSI %d dBm", ap_info.rssi);
                task_send_heartbeat(TASK_TYPE_WIFI, heartbeat_msg);
                
            } else {
                // Desconectado - reintentar cada 60 segundos
                if (was_connected) {
                    // Primera vez que detectamos desconexión
                    ESP_LOGW(TAG, "⚠ WiFi desconectado, comenzando reintentos periódicos...");
                    send_led_status(SYSTEM_STATE_ESPERANDO_WIFI, "Reconectando");
                    was_connected = false;
                    
                    // Notificar pérdida de conectividad
                    xEventGroupClearBits(g_connectivity_event_group, CONNECTIVITY_WIFI_CONNECTED_BIT);
                    ESP_LOGW(TAG, "⏸️ Conectividad perdida - Tareas pausadas");
                    
                    // Log de desconexión WiFi durante monitoreo
                    char details[128];
                    snprintf(details, sizeof(details), "{\"ssid\": \"%s\"}", wifi_ssid);
                    error_logger_log_system(
                        "WIFI_DISCONNECTED",
                        ERROR_SEVERITY_WARNING,
                        "WiFi desconectado durante operación normal",
                        details
                    );
                    
                    // Primer intento de reconexión
                    s_retry_num = 0;
                    esp_wifi_connect();
                    last_reconnect_attempt = xTaskGetTickCount();
                } else {
                    // Ya sabíamos que estaba desconectado - reintentar cada 60s
                    uint32_t time_since_last_attempt = xTaskGetTickCount() - last_reconnect_attempt;
                    if (time_since_last_attempt > pdMS_TO_TICKS(60000)) {
                        ESP_LOGI(TAG, "🔄 Reintentando conexión WiFi periódica...");
                        s_retry_num = 0;
                        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
                        esp_wifi_connect();
                        last_reconnect_attempt = xTaskGetTickCount();
                    }
                }
            }
            
            last_status_check = xTaskGetTickCount();
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000)); // Verificar cada 5 segundos
    }
}
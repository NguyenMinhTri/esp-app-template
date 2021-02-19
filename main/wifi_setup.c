#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>
#include <wifi_reconnect.h>

#define APP_WIFI_PROV_TIMEOUT_S CONFIG_APP_WIFI_PROV_TIMEOUT_S

static const char TAG[] = "wifi_setup";

static esp_timer_handle_t provisioning_timer = NULL;
static wifi_config_t startup_wifi_config = {};

static void provisioning_timer_delete()
{
    if (provisioning_timer)
    {
        esp_timer_stop(provisioning_timer);
        esp_timer_delete(provisioning_timer);
        provisioning_timer = NULL;
    }
}

static void provisioning_timer_handler(__unused void *arg)
{
    ESP_LOGI(TAG, "provisioning timeout");
    wifi_prov_mgr_stop_provisioning();
    // Note: everything is done in WIFI_PROV_END handler
}

static void wifi_prov_event_handler(__unused void *arg, __unused esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "provisioning started");
        break;
    case WIFI_PROV_CRED_RECV: {
        wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
        ESP_LOGI(TAG, "provisioning received ssid '%s'", (const char *)wifi_sta_cfg->ssid);
        break;
    }
    case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
        ESP_LOGE(TAG, "provisioning failed: %s", (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "wifi STA authentication failed" : "wifi AP not found");
        // Note: Let the timeout kill provisioning, even if it won't connect anyway
        break;
    }
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "provisioning successful");
        break;
    case WIFI_PROV_END: {
        ESP_LOGI(TAG, "provisioning end");
        provisioning_timer_delete();
        wifi_prov_mgr_deinit();

        // When successful, config should be correctly set
        // On timeout, it needs to be reset manually (probably bug in wifi config stack)
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
        wifi_config_t current_cfg = {};
        ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &current_cfg));

        if (current_cfg.sta.ssid[0] == '\0')
        {
            ESP_LOGI(TAG, "wifi credentials not found after provisioning, trying startup wifi config");
            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &startup_wifi_config));
        }
        else if (startup_wifi_config.sta.ssid[0] == '\0')
        {
            // Nothing we can do, no internet connectivity
            ESP_LOGI(TAG, "no wifi credentials found");
        }

        wifi_reconnect_resume();
        break;
    }
    default:
        break;
    }
}

void setup_wifi(bool reconfigure)
{
    // Get app info
    esp_app_desc_t app_info = {};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ota_get_partition_description(esp_ota_get_running_partition(), &app_info));

    // Service device_name
    uint64_t mac = 0;
    ESP_ERROR_CHECK(esp_efuse_mac_get_default((uint8_t *)&mac));

    char device_name[33] = {}; // max 32 characters
    snprintf(device_name, sizeof(device_name), "%.25s-%06llx", app_info.project_name, mac);
    ESP_LOGI(TAG, "device name '%s'", device_name);

    // Initialize WiFi
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, device_name));
    ESP_ERROR_CHECK(wifi_reconnect_start()); // NOTE this must be called before connect, otherwise it might miss connected event

    // Store original STA config, so it can be used on provisioning timeout
    // Note: This is needed, since wifi stack is unable to re-read correct config from NVS after provisioning for some reason
    ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &startup_wifi_config));

    // Initialize provisioning
    wifi_prov_mgr_config_t wifi_prof_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler, NULL));
    ESP_ERROR_CHECK(wifi_prov_mgr_init(wifi_prof_cfg));

    // Provisioning mode
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned || reconfigure)
    {
        // Provisioning mode
        ESP_LOGI(TAG, "provisioning starting, timeout %d s", APP_WIFI_PROV_TIMEOUT_S);

        char service_name[sizeof(device_name) + 5] = {};
        snprintf(service_name, sizeof(service_name), "PROV_%s", device_name);

        //ESP_ERROR_CHECK(wifi_prov_mgr_endpoint_create("custom-data")); // TODO
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, NULL, service_name, NULL));

        esp_timer_create_args_t args = {
            .callback = provisioning_timer_handler,
            .name = "wifi_prov_timer",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &provisioning_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(provisioning_timer, APP_WIFI_PROV_TIMEOUT_S * 1000000));
    }
    else
    {
        // Deallocate wifi provisioning
        wifi_prov_mgr_deinit();

        // Connect to existing wifi
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_reconnect_resume();
    }
}
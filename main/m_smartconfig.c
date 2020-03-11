#include "m_smartconfig.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_wifi.h"

static EventGroupHandle_t s_wifi_event_group;
static esp_err_t Airkiss_CONNECT;

static const int CONNECTED_BIT = BIT0;
static const int Airkiss_DONE_BIT = BIT1;

static const char *TAG = "sc";
static uint16_t Connect_Times = 0;

void smartconfig_task(void *parm);

static void sc_callback(smartconfig_status_t status, void *pdata) {
    switch (status) {
        case SC_STATUS_WAIT:
            ESP_LOGI(TAG, " [ sc ] SC_STATUS_WAIT");
            break;
        case SC_STATUS_FIND_CHANNEL:
            ESP_LOGI(TAG, " [ sc ] SC_STATUS_FINDING_CHANNEL");
            Led_Display(DISPLAY_PATTERN_WIFI_SETTING);
            break;
        case SC_STATUS_GETTING_SSID_PSWD:
            ESP_LOGI(TAG, " [ sc ] SC_STATUS_GETTING_SSID_PSWD");
            break;
        case SC_STATUS_LINK:
            ESP_LOGI(TAG, " [ sc ] SC_STATUS_LINK");
            Led_Display(DISPLAY_PATTERN_WIFI_CONNECTTING);
            wifi_config_t *wifi_config = pdata;
            ESP_LOGI(TAG, " [ sc ] SSID:%s", wifi_config->sta.ssid);
            ESP_LOGI(TAG, " [ sc ] PASSWORD:%s", wifi_config->sta.password);
            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, wifi_config));
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;
        case SC_STATUS_LINK_OVER:
            ESP_LOGI(TAG, " [ sc ] SC_STATUS_LINK_OVER");
            Led_Display(DISPLAY_PATTERN_WIFI_CONNECTED);
            if (pdata != NULL) {
                uint8_t phone_ip[4] = {0};
                memcpy(phone_ip, (uint8_t *)pdata, 4);
                ESP_LOGI(TAG, " [ sc ] Phone ip: %d.%d.%d.%d\n", phone_ip[0],
                         phone_ip[1], phone_ip[2], phone_ip[3]);
            }
            xEventGroupSetBits(s_wifi_event_group, Airkiss_DONE_BIT);
            break;
        default:
            break;
    }
}

void smartconfig_task(void *parm) {
    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS));
    ESP_ERROR_CHECK(esp_smartconfig_start(sc_callback));
    while (1) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group,
                                     CONNECTED_BIT | Airkiss_DONE_BIT, true,
                                     false, portMAX_DELAY);
        if (uxBits & CONNECTED_BIT) {
            ESP_LOGI(TAG, " [ sc ] WiFi Connected to ap");
        }
        if (uxBits & Airkiss_DONE_BIT) {
            ESP_LOGI(TAG, " [ sc ] smartconfig over");
            Led_Display(DISPLAY_PATTERN_WIFI_SETTING_FINISHED);
            Airkiss_CONNECT = ESP_OK;
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}

esp_err_t Wait_Airkiss_Connect(void) {
    while (Airkiss_CONNECT != ESP_OK) {
        vTaskDelay(10 / portTICK_RATE_MS);
    }
    return Airkiss_CONNECT;
}

static esp_err_t event_handler(void *ctx, system_event_t *event) {
    switch (event->event_id) {
        case SYSTEM_EVENT_WIFI_READY:
            ESP_LOGI(TAG, "WIFI_EVENT_WIFI_READY.");
            break;
        case SYSTEM_EVENT_STA_START:
            switch (esp_wifi_connect()) {
                case ESP_OK:
                    ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START.");
                    Led_Display(DISPLAY_PATTERN_WIFI_CONNECTTING);
                    break;
                case ESP_ERR_WIFI_NOT_INIT:
                    ESP_LOGI(TAG, "ESP_ERR_WIFI_NOT_INIT.");
                    break;
                case ESP_ERR_WIFI_NOT_STARTED:
                    ESP_LOGI(TAG, "ESP_ERR_WIFI_NOT_STARTED.");
                    break;
                case ESP_ERR_WIFI_CONN:
                    ESP_LOGI(TAG, "ESP_ERR_WIFI_CONN.");
                    break;
                case ESP_ERR_WIFI_SSID:
                    xTaskCreate(smartconfig_task, "smartconfig_task", 4096,
                                NULL, 3, NULL);
                    ESP_LOGI(TAG, "ESP_ERR_WIFI_SSID.");
                    break;
            }
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            Airkiss_CONNECT = ESP_OK;
            ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP.");
            break;
        case SYSTEM_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED.");
            Led_Display(DISPLAY_PATTERN_WIFI_CONNECTED);
            Connect_Times = 0;
            // xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "SYSTEM_EVENT_STA_DISCONNECTED.");
            Led_Display(DISPLAY_PATTERN_WIFI_DISCONNECTED);
            Connect_Times++;
            if (Connect_Times >= 10) {
                xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3,
                            NULL);
                Connect_Times = 0;
            } else
                esp_wifi_connect();
            break;

        default:
            break;
    }
    return ESP_OK;
}

// Check the wifi connection status if there is no wifi then enter smartconfig
// mode
void Wifi_Init_Airkiss(void) {
    Airkiss_CONNECT = ESP_FAIL;
    esp_log_level_set(TAG, ESP_LOG_INFO);

    tcpip_adapter_init();
    ESP_LOGI(TAG, " [ 1 ] Start Wifi Airkiss...");
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    Wait_Airkiss_Connect();
}

void Break_Wifi_Connect(void) {
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_restore());
    esp_restart();
}

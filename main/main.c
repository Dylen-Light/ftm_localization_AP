#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "nvs_flash.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_console.h"
#include "esp_now.h"

#define CONFIG_LESS_INTERFERENCE_CHANNEL 11
#define CONFIG_SEND_FREQUENCY 1
//static const uint8_t CONFIG_CSI_SEND_MAC[] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x00};

static bool s_reconnect = true;
static const char *TAG_ANCHOR = "ftm-anchor";

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;

// 带宽和信道
const wifi_bandwidth_t CURRENT_BW = WIFI_BW_HT40;
const uint8_t CURRENT_CHANNEL = 11;

static void wifi_connected_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;

    ESP_LOGI(TAG_ANCHOR, "Connected to %s (BSSID: " MACSTR ", Channel: %d)", event->ssid,
             MAC2STR(event->bssid), event->channel);

    xEventGroupClearBits(wifi_event_group, DISCONNECTED_BIT);
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT); // event设置为连接状态
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (s_reconnect)
    {
        ESP_LOGI(TAG_ANCHOR, "sta disconnect, s_reconnect...");
        esp_wifi_connect();
    }
    else
    {
        ESP_LOGI(TAG_ANCHOR, "sta disconnect");
    }
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    xEventGroupSetBits(wifi_event_group, DISCONNECTED_BIT); // event设置为非连接状态
}

// 默认初始化函数
void initialise_wifi(void)
{
    static bool initialized = false;

    if (initialized)
    {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        WIFI_EVENT_STA_CONNECTED,
                                                        &wifi_connected_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        WIFI_EVENT_STA_DISCONNECTED,
                                                        &disconnect_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(ESP_IF_WIFI_AP, CURRENT_BW));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(ESP_IF_WIFI_STA, CURRENT_BW));

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_config_espnow_rate(ESP_IF_WIFI_STA, WIFI_PHY_RATE_MCS0_SGI));
    //ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_LESS_INTERFERENCE_CHANNEL, WIFI_SECOND_CHAN_BELOW));
    //ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, CONFIG_CSI_SEND_MAC));
    initialized = true;
}

static bool start_wifi_ap(const char *ssid, const char *pass)
{
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .max_connection = 4,
            .password = "",
            .authmode = WIFI_AUTH_WPA2_PSK,
            .channel = CURRENT_CHANNEL,
            .ftm_responder = true},
    };

    s_reconnect = false;
    strlcpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password));

    if (strlen(pass) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    //ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    return true;
}

void app_main(void)
{
    uint8_t mac[6];
    char mac_add[17];

    wifi_bandwidth_t bw;

    // 重置一下内存
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    initialise_wifi();

        // 记录mac地址至mac_add数组
    ESP_ERROR_CHECK(esp_base_mac_addr_get(&mac[0]));
    sprintf(&mac_add[0], "ftm_%02X%02X%02X%02X%02X%02X", (unsigned char)mac[0], (unsigned char)mac[1], (unsigned char)mac[2], (unsigned char)mac[3], (unsigned char)mac[4], (unsigned char)mac[5]);
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, mac));

    start_wifi_ap(mac_add, "ftmftmftmftm"); // mac地址（STA接口地址）作为ssid，后面是密码

    ESP_ERROR_CHECK(esp_wifi_get_bandwidth(ESP_IF_WIFI_AP, &bw));

    if (bw == WIFI_BW_HT20)
    {
        printf("BW = 20Mhz\n");
    }
    else
    {
        printf("BW = 40Mhz\n");
    }

    printf("Started SoftAP with FTM Responder support, SSID - %s\n", mac_add);

    /**
     * @breif Initialize ESP-NOW
     *        ESP-NOW protocol see: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html
     */
    ESP_ERROR_CHECK(esp_now_init());
    //ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)"pmk1234567890123"));
    esp_now_peer_info_t peer = {
        .channel   = CURRENT_CHANNEL,//同AP信道
        .ifidx     = WIFI_IF_STA,    
        .encrypt   = false,   
        .peer_addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},//广播
    };
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_LOGI(TAG_ANCHOR, "================ CSI SEND ================");
    ESP_LOGI(TAG_ANCHOR, "wifi_channel: %d, send_frequency: %d, mac: " MACSTR,
             CONFIG_LESS_INTERFERENCE_CHANNEL, CONFIG_SEND_FREQUENCY, MAC2STR(mac));

    for (uint8_t count = 0; ;++count) {
        esp_err_t ret = esp_now_send(peer.peer_addr, &count, sizeof(uint8_t));

        if(ret != ESP_OK) {
            ESP_LOGW(TAG_ANCHOR, "<%s> ESP-NOW send error", esp_err_to_name(ret));
        }else{
            ESP_LOGI(TAG_ANCHOR,"send csi success.");
        }

        usleep(1000 * 1000 / CONFIG_SEND_FREQUENCY);
    }



}

#include "network.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif
#include "lwip/err.h"
#include "lwip/sys.h"
#include "sdkconfig.h"

/*─── Configuration macro aliases ─────────────────────────────────────────────*/

#if CONFIG_NETWORK_WIFI_STA
  #define ESP_WIFI_STA_ENABLE   CONFIG_NETWORK_WIFI_STA
  #define ESP_WIFI_STA_SSID     CONFIG_NETWORK_WIFI_STA_SSID
  #define ESP_WIFI_STA_PASSWD   CONFIG_NETWORK_WIFI_STA_PASSWORD
  #define ESP_MAXIMUM_RETRY     CONFIG_NETWORK_WIFI_STA_RETRY
#endif

#if CONFIG_NETWORK_WIFI_AP
  #define ESP_WIFI_AP_ENABLE    CONFIG_NETWORK_WIFI_AP
  #define ESP_WIFI_AP_SSID      CONFIG_NETWORK_WIFI_AP_SSID
  #define ESP_WIFI_AP_PASSWD    CONFIG_NETWORK_WIFI_AP_PASSWORD
  #define ESP_WIFI_CHANNEL      CONFIG_NETWORK_WIFI_AP_CHANNEL
  #define MAX_STA_CONN          CONFIG_NETWORK_WIFI_AP_MAX_CONNECTIONS
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define DHCPS_OFFER_DNS    0x02

static const char *TAG_AP  = "WiFi SoftAP";
static const char *TAG_STA = "WiFi Sta";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/*─── Event handler ──────────────────────────────────────────────────────────*/

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
  #if CONFIG_NETWORK_WIFI_AP
        wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
  #endif

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
  #if CONFIG_NETWORK_WIFI_AP
        wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d, reason:%d",
                 MAC2STR(event->mac), event->aid, event->reason);
  #endif

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
  #if CONFIG_NETWORK_WIFI_STA
        esp_wifi_connect();
        ESP_LOGI(TAG_STA, "Station started");
  #endif

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
  #if CONFIG_NETWORK_WIFI_STA
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  #endif
    }
}

/*─── SoftAP init ─────────────────────────────────────────────────────────────*/

#if CONFIG_NETWORK_WIFI_AP
esp_netif_t *wifi_init_softap(void)
{
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid        = ESP_WIFI_AP_SSID,
            .ssid_len    = strlen(ESP_WIFI_AP_SSID),
            .channel     = ESP_WIFI_CHANNEL,
            .password    = ESP_WIFI_AP_PASSWD,
            .max_connection = MAX_STA_CONN,
            .authmode    = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg     = { .required = false, },
        },
    };

    if (strlen(ESP_WIFI_AP_PASSWD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_LOGI(TAG_AP, "SoftAP ready. SSID:%s PW:%s CH:%d",
             ESP_WIFI_AP_SSID, ESP_WIFI_AP_PASSWD, ESP_WIFI_CHANNEL);

    return esp_netif_ap;
}
#endif  // CONFIG_NETWORK_WIFI_AP

/*─── STA init ────────────────────────────────────────────────────────────────*/

#if CONFIG_NETWORK_WIFI_STA
esp_netif_t *wifi_init_sta(void)
{
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid         = ESP_WIFI_STA_SSID,
            .password     = ESP_WIFI_STA_PASSWD,
            .scan_method  = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = ESP_MAXIMUM_RETRY,
            .sae_pwe_h2e  = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    ESP_LOGI(TAG_STA, "Station config done. SSID:%s", ESP_WIFI_STA_SSID);

    return esp_netif_sta;
}
#endif  // CONFIG_NETWORK_WIFI_STA

/*─── DNS relay for SoftAP ───────────────────────────────────────────────────*/

#if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
void softap_set_dns_addr(esp_netif_t *esp_netif_ap, esp_netif_t *esp_netif_sta)
{
    esp_netif_dns_info_t dns;
    esp_netif_get_dns_info(esp_netif_sta, ESP_NETIF_DNS_MAIN, &dns);

    uint8_t opt = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(esp_netif_ap));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(esp_netif_ap,
                        ESP_NETIF_OP_SET,
                        ESP_NETIF_DOMAIN_NAME_SERVER,
                        &opt, sizeof(opt)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(esp_netif_ap,
                        ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(esp_netif_ap));
}
#endif

/*─── init_wifi ───────────────────────────────────────────────────────────────*/

void init_wifi(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_init(& (wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT() ));

    /* Select mode based on Kconfig */
  #if CONFIG_NETWORK_WIFI_AP && CONFIG_NETWORK_WIFI_STA
    ESP_LOGI(TAG_STA, "Starting AP+STA mode");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  #elif CONFIG_NETWORK_WIFI_AP
    ESP_LOGI(TAG_AP, "Starting AP mode");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  #elif CONFIG_NETWORK_WIFI_STA
    ESP_LOGI(TAG_STA, "Starting STA mode");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  #else
    #error "CONFIG_NETWORK_WIFI_AP and CONFIG_NETWORK_WIFI_STA are both off!"
  #endif

    /* Bring up interfaces */
  #if CONFIG_NETWORK_WIFI_AP
    esp_netif_t *esp_netif_ap = wifi_init_softap();
  #endif

  #if CONFIG_NETWORK_WIFI_STA
    esp_netif_t *esp_netif_sta = wifi_init_sta();
  #endif

    ESP_ERROR_CHECK(esp_wifi_start());

    /* If STA, wait for IP */
  #if CONFIG_NETWORK_WIFI_STA
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_STA,
                 "Connected: SSID=%s", ESP_WIFI_STA_SSID);
      #if CONFIG_NETWORK_WIFI_AP
        softap_set_dns_addr(esp_netif_ap, esp_netif_sta);
      #endif
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG_STA,
                 "Failed to connect: SSID=%s", ESP_WIFI_STA_SSID);
    }
    esp_netif_set_default_netif(esp_netif_sta);
  #endif  // CONFIG_NETWORK_WIFI_STA

    /* Enable NAPT only if AP is up */
  #if CONFIG_NETWORK_WIFI_AP
    if (esp_netif_napt_enable(esp_netif_ap) != ESP_OK) {
        ESP_LOGE(TAG_AP, "Failed to enable NAPT");
    }
  #endif
}

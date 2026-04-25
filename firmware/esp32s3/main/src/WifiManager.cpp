// PHASE 2 — skeleton; first prototype runs on RP2040
#include "WifiManager.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace oxinode::esp32
{
    namespace
    {
        constexpr const char* kTag = "WifiManager";

        // Event group bits — mirrors the ESP-IDF station example.
        constexpr int kBitConnected = BIT0;
        constexpr int kBitFailed    = BIT1;

        std::atomic<bool>    g_connected{false};
        std::atomic<std::uint32_t> g_ipAddr{0};

        EventGroupHandle_t   g_evt = nullptr;
        int                  g_retryCount = 0;
        constexpr int        kMaxRetries  = 5;

        // Small NVS init that tolerates a corrupted partition by
        // erasing and retrying — IDF's recommended boilerplate.
        void initNvs()
        {
            esp_err_t ret = nvs_flash_init();
            if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
            {
                ESP_ERROR_CHECK(nvs_flash_erase());
                ret = nvs_flash_init();
            }
            ESP_ERROR_CHECK(ret);
        }

        void wifiEventHandler(void* /*arg*/, esp_event_base_t base,
                              int32_t id, void* data)
        {
            if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
            {
                ESP_LOGI(kTag, "STA start, connecting…");
                (void)esp_wifi_connect();
            }
            else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
            {
                g_connected.store(false, std::memory_order_relaxed);
                if (g_retryCount < kMaxRetries)
                {
                    ++g_retryCount;
                    ESP_LOGW(kTag, "STA disconnected, retry %d/%d", g_retryCount, kMaxRetries);
                    (void)esp_wifi_connect();
                }
                else if (g_evt != nullptr)
                {
                    xEventGroupSetBits(g_evt, kBitFailed);
                }
            }
            else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED)
            {
                auto* e = static_cast<wifi_event_ap_staconnected_t*>(data);
                ESP_LOGI(kTag, "AP: station " MACSTR " joined (AID=%d)",
                         MAC2STR(e->mac), e->aid);
            }
        }

        void ipEventHandler(void* /*arg*/, esp_event_base_t /*base*/,
                            int32_t id, void* data)
        {
            if (id == IP_EVENT_STA_GOT_IP)
            {
                auto* evt = static_cast<ip_event_got_ip_t*>(data);
                g_ipAddr.store(evt->ip_info.ip.addr, std::memory_order_relaxed);
                g_connected.store(true, std::memory_order_relaxed);
                g_retryCount = 0;
                ESP_LOGI(kTag, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
                if (g_evt != nullptr)
                {
                    xEventGroupSetBits(g_evt, kBitConnected);
                }
            }
        }

        // Build a 4-hex-char tag from the lower 16 bits of the MAC for
        // the soft-AP fallback SSID. e.g. MAC ...:9F:34 → "9F34".
        void macSuffix(char out[5])
        {
            std::uint8_t mac[6] = {0};
            (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
            std::snprintf(out, 5, "%02X%02X", mac[4], mac[5]);
        }
    }

    void WifiManager::startStation()
    {
        initNvs();
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        (void)esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        g_evt = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &ipEventHandler, nullptr, nullptr));

        wifi_config_t sta{};
        std::strncpy(reinterpret_cast<char*>(sta.sta.ssid),
                     CONFIG_OXINODE_WIFI_SSID, sizeof(sta.sta.ssid));
        std::strncpy(reinterpret_cast<char*>(sta.sta.password),
                     CONFIG_OXINODE_WIFI_PASSWORD, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(kTag, "joining \"%s\"…", CONFIG_OXINODE_WIFI_SSID);

        const EventBits_t bits = xEventGroupWaitBits(
            g_evt,
            kBitConnected | kBitFailed,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(kStaJoinTimeoutMs));

        if ((bits & kBitConnected) != 0)
        {
            ESP_LOGI(kTag, "STA up");
            return;
        }

        ESP_LOGW(kTag, "STA join timed out — falling back to AP mode");
        startSoftApFallback();
    }

    void WifiManager::startSoftApFallback()
    {
        // Tear down STA and bring up AP. We do NOT call esp_wifi_init
        // again because it was already called from startStation().
        (void)esp_wifi_stop();
        (void)esp_wifi_set_mode(WIFI_MODE_AP);

        // Need a netif for AP mode if startStation() ran first.
        // create_default_wifi_ap is idempotent across the netif key.
        (void)esp_netif_create_default_wifi_ap();

        char suffix[5] = {0};
        macSuffix(suffix);

        wifi_config_t ap{};
        const int n = std::snprintf(reinterpret_cast<char*>(ap.ap.ssid),
                                    sizeof(ap.ap.ssid), "%s-%s",
                                    CONFIG_OXINODE_WIFI_AP_SSID, suffix);
        ap.ap.ssid_len = (n > 0) ? static_cast<std::uint8_t>(n) : 0;
        std::strncpy(reinterpret_cast<char*>(ap.ap.password),
                     CONFIG_OXINODE_WIFI_AP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.channel        = 1;
        ap.ap.max_connection = 4;
        ap.ap.authmode = (std::strlen(CONFIG_OXINODE_WIFI_AP_PASSWORD) >= 8)
            ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ap.ap.pmf_cfg.required = false;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());

        // soft-AP IP is the IDF default 192.168.4.1
        g_ipAddr.store(0x0104A8C0u, std::memory_order_relaxed);
        g_connected.store(true, std::memory_order_relaxed);

        ESP_LOGI(kTag, "AP up: SSID=\"%s\" pass=\"%s\" -> http://192.168.4.1/",
                 reinterpret_cast<const char*>(ap.ap.ssid),
                 CONFIG_OXINODE_WIFI_AP_PASSWORD);
    }

    bool WifiManager::isConnected()
    {
        return g_connected.load(std::memory_order_relaxed);
    }

    std::uint32_t WifiManager::ipAddr()
    {
        return g_ipAddr.load(std::memory_order_relaxed);
    }
}

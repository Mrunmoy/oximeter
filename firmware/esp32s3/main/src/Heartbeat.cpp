// PHASE 2 — skeleton; first prototype runs on RP2040
#include "Heartbeat.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// led_strip is an IDF managed component. If you haven't added it via
// `idf.py add-dependency espressif/led_strip` yet the include below
// fails — the firmware still compiles because the include is gated on
// __has_include and we fall back to a logging-only heartbeat.
#if __has_include("led_strip.h")
#  include "led_strip.h"
#  define OXINODE_HAVE_LED_STRIP 1
#else
#  define OXINODE_HAVE_LED_STRIP 0
#endif

namespace oxinode::esp32
{
    namespace
    {
        constexpr const char* kTag = "Heartbeat";

#if OXINODE_HAVE_LED_STRIP
        led_strip_handle_t g_strip = nullptr;

        void setColor(std::uint8_t r, std::uint8_t g, std::uint8_t b)
        {
            if (g_strip == nullptr) { return; }
            (void)led_strip_set_pixel(g_strip, 0, r, g, b);
            (void)led_strip_refresh(g_strip);
        }
#endif

        void heartbeatTask(void* /*arg*/)
        {
#if OXINODE_HAVE_LED_STRIP
            led_strip_config_t stripCfg{};
            stripCfg.strip_gpio_num = Heartbeat::kStatusLedGpio;
            stripCfg.max_leds       = 1;
            stripCfg.led_model      = LED_MODEL_WS2812;
            stripCfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

            led_strip_rmt_config_t rmtCfg{};
            rmtCfg.clk_src           = RMT_CLK_SRC_DEFAULT;
            rmtCfg.resolution_hz     = 10 * 1000 * 1000;
            rmtCfg.mem_block_symbols = 64;
            rmtCfg.flags.with_dma    = false;

            esp_err_t err = led_strip_new_rmt_device(&stripCfg, &rmtCfg, &g_strip);
            if (err != ESP_OK)
            {
                ESP_LOGW(kTag, "led_strip init failed: %s — falling back to log",
                         esp_err_to_name(err));
                g_strip = nullptr;
            }
#endif
            bool on = false;
            while (true)
            {
                on = !on;
#if OXINODE_HAVE_LED_STRIP
                if (g_strip != nullptr)
                {
                    // dim green: easy on the eyes at night, doesn't
                    // wash out the user's sight if they look at the
                    // module while a finger is on the sensor.
                    if (on) { setColor(0, 8, 0); }
                    else    { setColor(0, 0, 0); }
                }
                else
                {
                    if (on) { ESP_LOGI(kTag, "alive"); }
                }
#else
                if (on) { ESP_LOGI(kTag, "alive"); }
#endif
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    } // namespace

    void Heartbeat::start()
    {
        // Low-priority cosmetic task. tskNO_AFFINITY lets the scheduler
        // park it on whichever core has slack.
        (void)xTaskCreatePinnedToCore(
            &heartbeatTask, "heartbeat",
            2048, nullptr,
            1, nullptr,
            tskNO_AFFINITY);
    }
}

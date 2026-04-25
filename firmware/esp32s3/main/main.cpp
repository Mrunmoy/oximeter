// PHASE 2 — skeleton; first prototype runs on RP2040
//
// app_main wires the four building blocks together:
//
//   ┌── core 0 ─────────┐    ┌── core 1 ────────────────┐
//   │  Wi-Fi event loop │    │  sensor task             │
//   │  HTTP / WS server │    │   - waits on INT sema    │
//   │  heartbeat task   │    │   - drains MAX30102 FIFO │
//   └───────────────────┘    │   - JSON-serializes      │
//                            │   - WebSocketServer::    │
//                            │     broadcast()          │
//                            └──────────────────────────┘
//
// Pin allocation is FIXED for the ESP32-S3-Zero:
//   SDA = GPIO8, SCL = GPIO9, INT = GPIO10 (open-drain, internal pull-up).
//
// The portable MAX30102 driver (lib/max3010x/) is sourced into this
// component via main/CMakeLists.txt's reach-across SRCS. If that
// driver isn't yet on disk the build fails at the link step — that's
// fine for skeleton work; the firmware/esp32s3/ side compiles
// against the IHal contract alone.

#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Esp32I2cHal.hpp"
#include "Esp32IntPin.hpp"
#include "Heartbeat.hpp"
#include "WebSocketServer.hpp"
#include "WifiManager.hpp"

#include "max3010x/Max30102.hpp"
#include "max3010x/SampleObserver.hpp"

namespace
{
    constexpr const char* kTag = "oxinode";

    // ── Pin allocation (FIXED) ───────────────────────────────────
    constexpr int           kPinSda = 8;
    constexpr int           kPinScl = 9;
    constexpr int           kPinInt = 10;
    constexpr int           kI2cPort = 0;
    constexpr std::uint32_t kI2cFreqHz = 100u * 1000u;
    constexpr std::uint8_t  kMax3010xAddr = 0x57;

    // ── Sensor task on core 1 ────────────────────────────────────
    //
    // The driver fans (tMs, ir, red) per FIFO entry through onSample,
    // and (tMs, hrBpm, spo2Pct) through onHrSpo2 once the HR detector
    // and SpO2 algorithm agree on a fresh estimate. We cache the latest
    // HR/SpO2 between samples so each WS broadcast carries a complete
    // record matching the JSON-Lines schema in docs/PROTOCOL.md.
    class WsBroadcastObserver : public oxinode::max3010x::ISampleObserver
    {
    public:
        void onSample(uint32_t tMs, uint32_t ir, uint32_t red) override
        {
            char buf[128];
            const int n = std::snprintf(
                buf, sizeof(buf),
                "{\"t\":%u,\"ir\":%u,\"red\":%u,\"hr\":%d,\"spo2\":%d}",
                static_cast<unsigned>(tMs),
                static_cast<unsigned>(ir),
                static_cast<unsigned>(red),
                static_cast<int>(m_lastHr),
                static_cast<int>(m_lastSpo2));
            if (n > 0)
            {
                oxinode::esp32::WebSocketServer::broadcast(buf);
            }
        }

        void onHrSpo2(uint32_t /*tMs*/, uint8_t hrBpm, uint8_t spo2Pct) override
        {
            m_lastHr   = hrBpm;
            m_lastSpo2 = spo2Pct;
        }

    private:
        uint8_t m_lastHr   = 0;
        uint8_t m_lastSpo2 = 0;
    };

    struct SensorCtx
    {
        oxinode::esp32::Esp32I2cHal* hal;
        oxinode::max3010x::Max30102* drv;
    };

    void sensorTask(void* arg)
    {
        auto* ctx = static_cast<SensorCtx*>(arg);

        // 1 s timeout: if no INT shows up for a second something is
        // wrong (sensor unplugged?) — log and keep waiting. Don't
        // hard-fail; the user might be re-seating the sensor.
        while (true)
        {
            const bool got = oxinode::esp32::Esp32IntPin::waitForInterrupt(1000);
            if (!got)
            {
                ESP_LOGW(kTag, "no INT in 1 s (edges=%u)",
                         static_cast<unsigned>(
                             oxinode::esp32::Esp32IntPin::edgeCount()));
                continue;
            }
            // Drain the FIFO out of ISR context — I²C from inside an
            // ISR is forbidden on the ESP32. The driver internally
            // calls IHal::i2cReadReg which blocks on the I²C master.
            const int rc = ctx->drv->handleInterrupt();
            if (rc < 0)
            {
                ESP_LOGW(kTag, "handleInterrupt rc=%d", rc);
            }
        }
    }
}

extern "C" void app_main()
{
    ESP_LOGI(kTag, "OxiNode (ESP32-S3) booting — phase 2 skeleton");

    // ── Wi-Fi: STA join, fall back to soft-AP after 30 s ─────────
    oxinode::esp32::WifiManager::startStation();

    // ── HTTP + WebSocket server ──────────────────────────────────
    oxinode::esp32::WebSocketServer::start();

    // ── Heartbeat (1 Hz status LED) ──────────────────────────────
    oxinode::esp32::Heartbeat::start();

    // ── I²C HAL ──────────────────────────────────────────────────
    // Aggregate-init the Config in field-declaration order. We avoid
    // C99-style designated initializers because they were only added
    // to standard C++ in C++20 — gcc -std=gnu++17 does accept them as
    // an extension, but staying portable C++17 means the same code
    // works under host-test builds with strict -std=c++17.
    static oxinode::esp32::Esp32I2cHal::Config s_halCfg{
        kI2cPort,
        kPinSda,
        kPinScl,
        kI2cFreqHz,
        kMax3010xAddr,
    };
    static oxinode::esp32::Esp32I2cHal s_hal(s_halCfg);
    if (s_hal.init() != 0)
    {
        ESP_LOGE(kTag, "I²C HAL init failed — halting sensor bring-up");
        // Wi-Fi UI still runs so the user can hit the page even with
        // no sensor wired. Don't hard-loop.
        return;
    }

    // ── Driver: SpO2 mode, default ratio-of-ratios ───────────────
    // Default Max30102::Config selects SpO2 mode, 100 Hz, AVG_4, 18-bit
    // pulse width, 4096 nA ADC range, ~7 mA per LED, FIFO almost-full
    // threshold 0x0F, rollover enabled. configure() also unmasks
    // A_FULL + PPG_RDY in INTR_ENABLE_1 so the INT pin will fire.
    static oxinode::max3010x::Max30102 s_drv(s_hal);
    static WsBroadcastObserver         s_obs;
    s_drv.addObserver(&s_obs);

    if (s_drv.probe() != 0)
    {
        ESP_LOGE(kTag, "MAX30102 probe() failed — sensor offline");
        return;
    }
    if (s_drv.configure(oxinode::max3010x::Max30102::Config{}) != 0)
    {
        ESP_LOGE(kTag, "MAX30102 configure() failed");
        return;
    }

    // ── INT pin (must arm AFTER the driver enables A_FULL etc.) ──
    if (oxinode::esp32::Esp32IntPin::init(kPinInt) != 0)
    {
        ESP_LOGE(kTag, "INT pin init failed");
        return;
    }

    // ── Sensor task pinned to core 1 ─────────────────────────────
    static SensorCtx s_ctx{ &s_hal, &s_drv };
    (void)xTaskCreatePinnedToCore(
        &sensorTask, "sensor",
        4096, &s_ctx,
        12, nullptr,
        1 /* core 1 */);

    ESP_LOGI(kTag, "all subsystems up — open the device IP in a browser");

    // ── app_main goes idle ───────────────────────────────────────
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

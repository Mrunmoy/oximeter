// PHASE 2 — skeleton; first prototype runs on RP2040
#include "Esp32IntPin.hpp"

#include <cerrno>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace oxinode::esp32
{
    namespace
    {
        constexpr const char* kTag = "Esp32IntPin";
    }

    std::atomic<std::uint32_t> Esp32IntPin::s_edges{0};
    int                        Esp32IntPin::s_gpio{-1};
    bool                       Esp32IntPin::s_inited{false};
    void*                      Esp32IntPin::s_sem{nullptr};

    int Esp32IntPin::init(int gpioNum)
    {
        if (s_inited)
        {
            return 0;
        }

        s_sem = xSemaphoreCreateBinary();
        if (s_sem == nullptr)
        {
            ESP_LOGE(kTag, "xSemaphoreCreateBinary failed");
            return -ENOMEM;
        }
        s_gpio = gpioNum;

        gpio_config_t io{};
        io.pin_bit_mask = (1ULL << gpioNum);
        io.mode         = GPIO_MODE_INPUT;
        io.pull_up_en   = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type    = GPIO_INTR_NEGEDGE;          // active-low INT

        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK)
        {
            ESP_LOGE(kTag, "gpio_config failed: %s", esp_err_to_name(err));
            return -EIO;
        }

        // gpio_install_isr_service is idempotent under ESP_ERR_INVALID_STATE
        // (already installed) so we tolerate that return code.
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(kTag, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
            return -EIO;
        }

        err = gpio_isr_handler_add(static_cast<gpio_num_t>(gpioNum),
                                   &Esp32IntPin::gpioIsrTrampoline,
                                   nullptr);
        if (err != ESP_OK)
        {
            ESP_LOGE(kTag, "gpio_isr_handler_add failed: %s", esp_err_to_name(err));
            return -EIO;
        }

        s_inited = true;
        ESP_LOGI(kTag, "INT armed on GPIO%d (FALLING)", gpioNum);
        return 0;
    }

    bool Esp32IntPin::waitForInterrupt(std::uint32_t timeoutMs)
    {
        if (!s_inited || s_sem == nullptr)
        {
            return false;
        }
        const TickType_t ticks = (timeoutMs == 0xFFFFFFFFu)
            ? portMAX_DELAY
            : pdMS_TO_TICKS(timeoutMs);
        return xSemaphoreTake(static_cast<SemaphoreHandle_t>(s_sem), ticks) == pdTRUE;
    }

    std::uint32_t Esp32IntPin::edgeCount()
    {
        return s_edges.load(std::memory_order_relaxed);
    }

    void IRAM_ATTR Esp32IntPin::gpioIsrTrampoline(void* /*arg*/)
    {
        s_edges.fetch_add(1, std::memory_order_relaxed);

        BaseType_t higherPrioWoken = pdFALSE;
        if (s_sem != nullptr)
        {
            xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(s_sem),
                                  &higherPrioWoken);
        }
        if (higherPrioWoken == pdTRUE)
        {
            portYIELD_FROM_ISR();
        }
    }
}

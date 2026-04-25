// PHASE 2 — skeleton; first prototype runs on RP2040
#pragma once

#include <cstdint>

namespace oxinode::esp32
{
    // ── Wi-Fi connectivity manager ────────────────────────────────
    //
    // Boots in STA mode using the credentials baked in via Kconfig
    // (CONFIG_OXINODE_WIFI_SSID / _PASSWORD). If the device fails to
    // associate inside kStaJoinTimeoutMs it falls back to soft-AP mode
    // with SSID "<prefix>-XXXX" derived from the lower 16 bits of the
    // station MAC. The fallback lets a freshly-flashed device with no
    // configured network still serve the UI.
    //
    // All methods are static — there is exactly one Wi-Fi controller
    // per chip and the IDF layer below is already a singleton.
    class WifiManager
    {
    public:
        WifiManager()                              = delete;
        ~WifiManager()                             = delete;
        WifiManager(const WifiManager&)            = delete;
        WifiManager& operator=(const WifiManager&) = delete;

        // Initialize NVS / netif / event loop and start STA. Blocking
        // call returns once we have an IP, OR after the timeout when
        // we transition into AP fallback.
        static void startStation();

        // Force-start soft-AP. Used by startStation() on STA timeout.
        static void startSoftApFallback();

        // True once we've been given an IPv4 lease (STA) or a STA has
        // associated with our soft-AP (AP fallback).
        [[nodiscard]] static bool isConnected();

        // Last seen IP address as host-order uint32. Zero when offline.
        [[nodiscard]] static std::uint32_t ipAddr();

        // Default STA join timeout before AP fallback triggers.
        static constexpr std::uint32_t kStaJoinTimeoutMs = 30u * 1000u;
    };
}

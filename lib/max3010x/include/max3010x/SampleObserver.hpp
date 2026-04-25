#pragma once

#include <cstdint>

// Observer interface for raw IR/RED samples and computed HR/SpO2 results.
// Mirrors HeartNode's SensorObserver pattern but specialised to the
// pulse-oximeter data model — splitting the two callbacks lets a
// platform-side logger choose which stream it cares about.

namespace oxinode::max3010x
{
    class ISampleObserver
    {
    public:
        virtual ~ISampleObserver() = default;

        // Called once per FIFO sample drained, before HR/SpO2 update.
        // `tMs` is the platform monotonic clock at drain time.
        virtual void onSample(uint32_t tMs, uint32_t ir, uint32_t red) = 0;

        // Called when the HR detector and SpO2 algo agree on a fresh
        // estimate. Either value may be 0 to signal "not yet valid"
        // — callers must sanity-check.
        virtual void onHrSpo2(uint32_t tMs, uint8_t hrBpm, uint8_t spo2Pct) = 0;
    };
}

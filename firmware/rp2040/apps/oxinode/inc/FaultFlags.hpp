#pragma once

#include <cstdint>

// Bitmap of "things that look wrong". Surfaced in every alive frame
// as `fault_flags` so the host can detect regressions without parsing
// individual counters. Each bit is *level-triggered* — set when the
// condition holds during the current alive-frame interval, cleared
// when the condition stops holding. The host (or `scripts/burn-in.sh`)
// is responsible for sticky-recording any bit it observes.
//
// New bits append to the end. Never repurpose an existing value.

namespace oxinode::rp2040
{
    enum FaultFlag : std::uint32_t
    {
        FAULT_NONE              = 0,
        // Stage A: driver-level events
        FAULT_BROWNOUT          = 1u << 0,   // PWR_RDY tripped this interval
        FAULT_ALC_DEGRADED      = 1u << 1,   // ALC_OVF tripped this interval
        FAULT_I2C_ERR           = 1u << 2,   // i2cErrTotal advanced this interval

        // Stage B: ring / consumer events
        FAULT_RING_DROPS        = 1u << 3,   // ring_drops advanced this interval
        FAULT_BACKPRESSURE      = 1u << 4,   // ring_hwm ≥ 75 % of capacity
        FAULT_DSP_OVERBUDGET    = 1u << 5,   // dsp_overbudget advanced this interval

        // Stage C: soft liveness — *should never trigger* in healthy ops.
        // Set if the producer/consumer index did not advance for ≥3 s.
        FAULT_STAGNANT_PRODUCER = 1u << 6,
        FAULT_STAGNANT_CONSUMER = 1u << 7,
    };
}

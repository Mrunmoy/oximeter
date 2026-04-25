#pragma once

#include <cstdarg>
#include <cstdio>

#include "UsbCdcLink.hpp"

namespace oxinode::rp2040
{
    // ── Lightweight structured logger ───────────────────────────
    //
    // Wraps two helpers around the global UsbCdcLink so the rest of
    // the firmware can emit `{"status":"...","reason":"..."}` JSON
    // (or STATUS frames in BIN mode) without sprinkling format
    // strings everywhere.
    //
    // The link reference is a free function instead of a singleton
    // so unit-testing on host can swap in a fake without inheritance.
    class StatusLog
    {
    public:
        StatusLog()                            = delete;
        ~StatusLog()                           = delete;
        StatusLog(const StatusLog&)            = delete;
        StatusLog& operator=(const StatusLog&) = delete;

        static void bind(UsbCdcLink* link) { s_link = link; }

        // Format-string variants. The reason buffer is bounded
        // (kReasonBufBytes) so we never trigger heap allocation.
        static void info(const char* status, const char* fmt, ...)
        {
            va_list ap;
            va_start(ap, fmt);
            emit(status, fmt, ap);
            va_end(ap);
        }

        static void warn(const char* status, const char* fmt, ...)
        {
            // Same wire format as info(); a future revision can
            // route warnings to a separate channel without touching
            // call sites.
            va_list ap;
            va_start(ap, fmt);
            emit(status, fmt, ap);
            va_end(ap);
        }

    private:
        static constexpr std::size_t kReasonBufBytes = 96;

        static void emit(const char* status, const char* fmt, va_list ap)
        {
            char buf[kReasonBufBytes];
            (void)std::vsnprintf(buf, sizeof(buf), fmt, ap);

            if (s_link != nullptr)
            {
                s_link->writeStatus(status, buf);
            }
            else
            {
                // Pre-init fallback: stdio is already up by the time
                // anyone has a reason to log, but be defensive.
                std::printf("{\"status\":\"%s\",\"reason\":\"%s\"}\n", status, buf);
            }
        }

        // Definition lives in the same TU so the header stays
        // header-only and no .cpp is needed (driven by the linker
        // because it's an inline static).
        inline static UsbCdcLink* s_link{nullptr};
    };
}

// PHASE 2 — skeleton; first prototype runs on RP2040
#pragma once

#include <cstddef>

namespace oxinode::esp32
{
    // ── HTTP + WebSocket server ───────────────────────────────────
    //
    // Serves the embedded `index.html` at GET / and exposes a single
    // WebSocket endpoint at /ws. Connected clients receive one JSON
    // message per sample, schema-compatible with the RP2040 JSON-Lines
    // wire format:
    //
    //     {"t":12345,"ir":123456,"red":98765,"hr":72,"spo2":98}
    //
    // We track up to kMaxClients sockets in a fixed-size array so the
    // hot path never allocates. Five was the upper bound on HeartNode;
    // four matches the soft-AP max_connection setting and is enough
    // for "the user opens the page in two browsers".
    class WebSocketServer
    {
    public:
        WebSocketServer()                                  = delete;
        ~WebSocketServer()                                 = delete;
        WebSocketServer(const WebSocketServer&)            = delete;
        WebSocketServer& operator=(const WebSocketServer&) = delete;

        // Start HTTP server + register URI handlers. Idempotent.
        static void start();

        // Send a UTF-8 text frame to every connected client. Safe to
        // call from any task context. Drops the message if the server
        // is not running or no clients are connected.
        static void broadcast(const char* message);

        // Same, but binary. Phase-2 placeholder — current UI uses text.
        static void broadcastBinary(const void* data, std::size_t len);

        // Maximum simultaneous WebSocket clients.
        static constexpr std::size_t kMaxClients = 4;
    };
}

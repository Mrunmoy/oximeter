// PHASE 2 — skeleton; first prototype runs on RP2040
#include "WebSocketServer.hpp"

#include <array>
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace oxinode::esp32
{
    namespace
    {
        constexpr const char* kTag = "WebSocketServer";

        // Symbols planted by EMBED_FILES "index.html" in the
        // component's CMakeLists. The IDF mangles the filename into
        // an asm symbol of the form "_binary_<name>_start".
        extern "C" const std::uint8_t kIndexHtmlStart[] asm("_binary_index_html_start");
        extern "C" const std::uint8_t kIndexHtmlEnd[]   asm("_binary_index_html_end");

        // ── Module-scoped state ──────────────────────────────────
        // Two distinct httpd instances: one binds to CONFIG_OXINODE_HTTP_PORT
        // and serves the embedded HTML page; the other binds to
        // CONFIG_OXINODE_WS_PORT and hosts the /ws endpoint. Splitting
        // them lets us follow the spec's HTTP/WS port separation while
        // still using a single esp_http_server backend for both.
        httpd_handle_t                           g_httpServer = nullptr;
        httpd_handle_t                           g_wsServer   = nullptr;
        std::array<int, WebSocketServer::kMaxClients> g_clients{};
        std::size_t                              g_clientCount = 0;
        SemaphoreHandle_t                        g_clientLock  = nullptr;

        // ── helpers ──────────────────────────────────────────────
        void lockClients()
        {
            if (g_clientLock != nullptr)
            {
                (void)xSemaphoreTake(g_clientLock, portMAX_DELAY);
            }
        }
        void unlockClients()
        {
            if (g_clientLock != nullptr)
            {
                (void)xSemaphoreGive(g_clientLock);
            }
        }

        bool addClient(int fd)
        {
            bool added = false;
            lockClients();
            // already known?
            for (std::size_t i = 0; i < g_clientCount; ++i)
            {
                if (g_clients[i] == fd)
                {
                    unlockClients();
                    return true;
                }
            }
            if (g_clientCount < WebSocketServer::kMaxClients)
            {
                g_clients[g_clientCount++] = fd;
                added = true;
            }
            unlockClients();
            return added;
        }

        void removeClient(int fd)
        {
            lockClients();
            for (std::size_t i = 0; i < g_clientCount; ++i)
            {
                if (g_clients[i] == fd)
                {
                    g_clients[i] = g_clients[g_clientCount - 1];
                    --g_clientCount;
                    break;
                }
            }
            unlockClients();
        }

        // ── HTTP handler: serve /index.html ──────────────────────
        esp_err_t rootHandler(httpd_req_t* req)
        {
            const std::size_t len = static_cast<std::size_t>(
                kIndexHtmlEnd - kIndexHtmlStart);
            httpd_resp_set_type(req, "text/html");
            return httpd_resp_send(
                req,
                reinterpret_cast<const char*>(kIndexHtmlStart),
                len);
        }

        // ── WebSocket handshake + frame drain ────────────────────
        esp_err_t wsHandler(httpd_req_t* req)
        {
            if (req->method == HTTP_GET)
            {
                const int fd = httpd_req_to_sockfd(req);
                if (addClient(fd))
                {
                    ESP_LOGI(kTag, "WS client connected fd=%d (count=%u)",
                             fd, static_cast<unsigned>(g_clientCount));
                }
                else
                {
                    ESP_LOGW(kTag, "WS client list full, refusing fd=%d", fd);
                }
                return ESP_OK;
            }

            // We accept inbound frames but currently ignore them. The
            // browser UI is purely a viewer in phase 2.
            httpd_ws_frame_t frame{};
            esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
            if (ret != ESP_OK)
            {
                return ret;
            }
            if (frame.len > 0)
            {
                std::uint8_t scratch[64];
                if (frame.len > sizeof(scratch))
                {
                    return ESP_OK;   // silently drop oversized control msgs
                }
                frame.payload = scratch;
                ret = httpd_ws_recv_frame(req, &frame, frame.len);
                if (ret != ESP_OK)
                {
                    return ret;
                }
            }
            return ESP_OK;
        }

        // ── Socket-close hook ────────────────────────────────────
        void onClose(httpd_handle_t /*hd*/, int sockfd)
        {
            removeClient(sockfd);
            ESP_LOGI(kTag, "WS client disconnected fd=%d (count=%u)",
                     sockfd, static_cast<unsigned>(g_clientCount));
        }
    } // namespace

    void WebSocketServer::start()
    {
        if (g_httpServer != nullptr || g_wsServer != nullptr)
        {
            return;
        }

        if (g_clientLock == nullptr)
        {
            g_clientLock = xSemaphoreCreateMutex();
        }

        // ── HTTP server (port 80 by default): serves index.html ──
        httpd_config_t httpCfg = HTTPD_DEFAULT_CONFIG();
        httpCfg.server_port      = CONFIG_OXINODE_HTTP_PORT;
        httpCfg.ctrl_port        = httpCfg.ctrl_port; // keep default
        httpCfg.uri_match_fn     = httpd_uri_match_wildcard;
        httpCfg.lru_purge_enable = true;

        ESP_ERROR_CHECK(httpd_start(&g_httpServer, &httpCfg));

        httpd_uri_t rootUri{};
        rootUri.uri          = "/";
        rootUri.method       = HTTP_GET;
        rootUri.handler      = rootHandler;
        rootUri.user_ctx     = nullptr;
        rootUri.is_websocket = false;
        ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpServer, &rootUri));

        // ── WS server (port 81 by default): /ws endpoint ─────────
        httpd_config_t wsCfg = HTTPD_DEFAULT_CONFIG();
        wsCfg.server_port      = CONFIG_OXINODE_WS_PORT;
        // Distinct ctrl_port so the two httpd instances don't clash
        // on their internal control sockets.
        wsCfg.ctrl_port        = wsCfg.ctrl_port + 1;
        wsCfg.uri_match_fn     = httpd_uri_match_wildcard;
        wsCfg.lru_purge_enable = true;
        wsCfg.close_fn         = onClose;

        ESP_ERROR_CHECK(httpd_start(&g_wsServer, &wsCfg));

        httpd_uri_t wsUri{};
        wsUri.uri                   = "/ws";
        wsUri.method                = HTTP_GET;
        wsUri.handler               = wsHandler;
        wsUri.user_ctx              = nullptr;
        wsUri.is_websocket          = true;
        wsUri.handle_ws_control_frames = true;
        ESP_ERROR_CHECK(httpd_register_uri_handler(g_wsServer, &wsUri));

        ESP_LOGI(kTag, "HTTP on :%d  WS on :%d/ws",
                 CONFIG_OXINODE_HTTP_PORT, CONFIG_OXINODE_WS_PORT);
    }

    void WebSocketServer::broadcast(const char* message)
    {
        if (g_wsServer == nullptr || message == nullptr)
        {
            return;
        }
        httpd_ws_frame_t pkt{};
        pkt.type    = HTTPD_WS_TYPE_TEXT;
        pkt.payload = const_cast<std::uint8_t*>(
            reinterpret_cast<const std::uint8_t*>(message));
        pkt.len     = std::strlen(message);
        pkt.final   = true;

        // Snapshot the client list under the lock so we don't hold it
        // across blocking send_frame_async calls.
        int snapshot[kMaxClients];
        std::size_t n = 0;
        lockClients();
        n = g_clientCount;
        for (std::size_t i = 0; i < n; ++i) { snapshot[i] = g_clients[i]; }
        unlockClients();

        for (std::size_t i = 0; i < n; ++i)
        {
            const esp_err_t r = httpd_ws_send_frame_async(g_wsServer, snapshot[i], &pkt);
            if (r != ESP_OK)
            {
                ESP_LOGW(kTag, "ws send fd=%d err=%d", snapshot[i], r);
            }
        }
    }

    void WebSocketServer::broadcastBinary(const void* data, std::size_t len)
    {
        if (g_wsServer == nullptr || data == nullptr || len == 0)
        {
            return;
        }
        httpd_ws_frame_t pkt{};
        pkt.type    = HTTPD_WS_TYPE_BINARY;
        pkt.payload = const_cast<std::uint8_t*>(
            static_cast<const std::uint8_t*>(data));
        pkt.len     = len;
        pkt.final   = true;

        int snapshot[kMaxClients];
        std::size_t n = 0;
        lockClients();
        n = g_clientCount;
        for (std::size_t i = 0; i < n; ++i) { snapshot[i] = g_clients[i]; }
        unlockClients();

        for (std::size_t i = 0; i < n; ++i)
        {
            const esp_err_t r = httpd_ws_send_frame_async(g_wsServer, snapshot[i], &pkt);
            if (r != ESP_OK)
            {
                ESP_LOGW(kTag, "ws bin send fd=%d err=%d", snapshot[i], r);
            }
        }
    }
}

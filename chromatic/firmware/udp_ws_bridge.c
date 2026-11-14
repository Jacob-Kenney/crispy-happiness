#include "udp_ws_bridge.h"
#include "bridge_gzip.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cwl.h"
#include "cwl_net.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

enum { WsContinuation = 0, WsBinary = 2, WsControl = 8 };
enum { TestWsPort = 18765, TestBridgePort = 18766, TestPeerPort = 18767 };

typedef struct {
    char uri[256];
    struct sockaddr_in peer;
    uint16_t local_port;
    bool loopback;
    atomic_bool running;
    atomic_bool stop;
    atomic_bool connected;
    atomic_uint sent;
    atomic_uint received;
    atomic_uint dropped;
    atomic_uint bad_gzip;
    int udp;
    size_t rx_size;
    bool rx_binary;
    cf_infl_t *inflate_state;
    uint8_t rx[BridgeMaxGzip];
    uint8_t datagram[BridgeMaxDatagram];
} Bridge;

static Bridge bridge;
static atomic_bool test_running;
static atomic_bool test_outbound_ok;
static const char target_ssid[] = "GameBoy";

typedef struct {
    char uri[sizeof(bridge.uri)];
    uint32_t peer_ip;
    uint16_t local_port;
    uint16_t peer_port;
} BridgeSettings;

static bool valid_settings(const BridgeSettings *settings)
{
    if (!memchr(settings->uri, '\0', sizeof(settings->uri))) return false;
    const char *url = settings->uri;
    bool valid_uri = (!strncmp(url, "ws://", 5) && url[5]) ||
                     (!strncmp(url, "wss://", 6) && url[6]);
    return valid_uri && settings->local_port && settings->peer_port;
}

static esp_err_t update_settings(const BridgeSettings *settings)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("udp_bridge", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    if (settings) {
        result = nvs_set_blob(handle, "settings_v1", settings, sizeof(*settings));
    } else {
        result = nvs_erase_key(handle, "settings_v1");
        if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

static bool saved_settings(BridgeSettings *settings)
{
    nvs_handle_t handle;
    if (nvs_open("udp_bridge", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(*settings);
    esp_err_t result = nvs_get_blob(handle, "settings_v1", settings, &size);
    nvs_close(handle);
    return result == ESP_OK && size == sizeof(*settings) && valid_settings(settings);
}

static bool on_target_network(void)
{
    char ssid[33];
    CWL_GetSsidText(ssid, sizeof(ssid));
    return !strcmp(ssid, target_ssid) && CWL_IsOnline();
}

bool bridge_keep_awake(void)
{
    return atomic_load(&test_running) || atomic_load(&bridge.running);
}

static void drop_message(Bridge *b)
{
    b->rx_binary = false;
    b->rx_size = 0;
    atomic_fetch_add(&b->dropped, 1);
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t event, void *raw)
{
    (void)base;
    Bridge *b = arg;
    const esp_websocket_event_data_t *frame = raw;
    if (event == WEBSOCKET_EVENT_ERROR && b->loopback && frame) {
        printf("bridge selftest WebSocket error: %.*s\n", frame->data_len,
               frame->data_ptr ? frame->data_ptr : "");
    }
    if (event == WEBSOCKET_EVENT_CONNECTED || event == WEBSOCKET_EVENT_DISCONNECTED) {
        atomic_store(&b->connected, event == WEBSOCKET_EVENT_CONNECTED);
        if (b->loopback) printf("bridge selftest WebSocket %s\n",
                                event == WEBSOCKET_EVENT_CONNECTED ? "connected" : "disconnected");
        b->rx_binary = false;
        b->rx_size = 0;
    }
    if (event != WEBSOCKET_EVENT_DATA || frame->op_code >= WsControl) return;

    if (frame->payload_offset == 0 && frame->op_code != WsContinuation) {
        b->rx_size = 0;
        b->rx_binary = frame->op_code == WsBinary;
    }
    if (!b->rx_binary) return;
    if (frame->payload_len < 0 || frame->payload_offset < 0 || frame->data_len < 0 ||
        frame->payload_offset > frame->payload_len ||
        frame->data_len > frame->payload_len - frame->payload_offset ||
        (size_t)frame->data_len > sizeof(b->rx) - b->rx_size) {
        drop_message(b);
        return;
    }

    if (frame->data_len) {
        memcpy(b->rx + b->rx_size, frame->data_ptr, frame->data_len);
        b->rx_size += frame->data_len;
    }
    if (!frame->fin || frame->payload_offset + frame->data_len != frame->payload_len) return;

    int length = bridge_gunzip(b->rx, b->rx_size, b->datagram, b->inflate_state);
    if (length < 0) atomic_fetch_add(&b->bad_gzip, 1);
    if (length >= 0 && (b->loopback || on_target_network()) &&
        send(b->udp, b->datagram, length, MSG_DONTWAIT) == length) {
        atomic_fetch_add(&b->received, 1);
        b->rx_binary = false;
        b->rx_size = 0;
    } else {
        drop_message(b);
    }
}

static int open_udp(uint16_t port, bool loopback, const struct sockaddr_in *peer)
{
    const struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY),
    };
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    const struct timeval timeout = {.tv_usec = 100000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        bind(fd, (const struct sockaddr *)&local, sizeof(local)) ||
        connect(fd, (const struct sockaddr *)peer, sizeof(*peer))) {
        close(fd);
        return -1;
    }
    return fd;
}

static void bridge_task(void *arg)
{
    Bridge *b = arg;
    esp_websocket_client_handle_t ws = NULL;
    b->udp = -1;
    uint8_t *buffers = malloc(BridgeMaxDatagram + 1 + BridgeMaxGzip);
    if (!buffers) goto done;
reconnect:
    b->udp = -1;
    if (!b->loopback && !on_target_network()) {
        puts("bridge: waiting for Wi-Fi GameBoy");
        while (!on_target_network() && !atomic_load(&b->stop)) {
            char ssid[33];
            CWL_GetSsidText(ssid, sizeof(ssid));
            if (strcmp(ssid, target_ssid) && CwlNet_IsSaved(target_ssid)) {
                (void)CwlNet_Join(target_ssid, NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (atomic_load(&b->stop)) goto done;
    b->udp = open_udp(b->local_port, b->loopback, &b->peer);
    if (b->udp < 0) {
        printf("bridge: unable to open UDP socket\n");
        goto retry;
    }

    const esp_websocket_client_config_t config = {
        .uri = b->uri,
        .crt_bundle_attach = strncmp(b->uri, "wss://", 6) == 0 ? esp_crt_bundle_attach : NULL,
        .buffer_size = 512,
        .task_stack = 4096,
        .network_timeout_ms = 3000,
        .reconnect_timeout_ms = 1500,
        .enable_close_reconnect = true,
    };
    ws = esp_websocket_client_init(&config);
    if (!ws || esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, websocket_event, b) != ESP_OK ||
        esp_websocket_client_start(ws) != ESP_OK) {
        printf("bridge: unable to start WebSocket client\n");
        goto retry;
    }

    while (!atomic_load(&b->stop)) {
        if (!b->loopback && !on_target_network()) {
            // Stop this client so its reconnect attempts cannot use another SSID.
            esp_websocket_client_destroy(ws);
            ws = NULL;
            close(b->udp);
            b->udp = -1;
            goto reconnect;
        }
        int length = recv(b->udp, buffers, BridgeMaxDatagram + 1, 0);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            printf("bridge: UDP receive error %d\n", errno);
            goto retry;
        }
        uint8_t *encoded = buffers + BridgeMaxDatagram + 1;
        const bool connected = esp_websocket_client_is_connected(ws);
        int size = connected ? bridge_gzip(buffers, length, encoded) : -1;
        int written = size > 0 ? esp_websocket_client_send_bin(ws, (const char *)encoded, size, pdMS_TO_TICKS(1000)) : -1;
        if (size > 0 && written == size) {
            atomic_fetch_add(&b->sent, 1);
        } else {
            if (b->loopback && connected) {
                printf("bridge selftest send failed: gzip=%d sent=%d heap=%u largest=%u\n", size, written,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            }
            atomic_fetch_add(&b->dropped, 1);
        }
    }
    goto done;

retry:
    if (!b->loopback && !atomic_load(&b->stop)) {
        if (ws) esp_websocket_client_destroy(ws);
        ws = NULL;
        if (b->udp >= 0) close(b->udp);
        atomic_store(&b->connected, false);
        vTaskDelay(pdMS_TO_TICKS(2000));
        goto reconnect;
    }
done:
    if (ws) esp_websocket_client_destroy(ws);
    if (b->udp >= 0) close(b->udp);
    free(buffers);
    free(b->inflate_state);
    b->inflate_state = NULL;
    CWL_SetBridgeActive(false);
    atomic_store(&b->running, false);
    vTaskDelete(NULL);
}

// Takes ownership of the optional decoder workspace, including on failure.
static bool start_bridge(const char *uri, uint16_t local_port, const struct sockaddr_in *peer, bool loopback,
                         cf_infl_t *workspace)
{
    if (atomic_load(&bridge.running) || strlen(uri) >= sizeof(bridge.uri)) {
        free(workspace);
        return false;
    }
    bridge.inflate_state = workspace ? workspace : malloc(sizeof(*workspace));
    if (!bridge.inflate_state) {
        puts("bridge: insufficient heap for gzip decoder");
        return false;
    }
    strcpy(bridge.uri, uri);
    bridge.local_port = local_port;
    bridge.peer = *peer;
    bridge.loopback = loopback;
    bridge.rx_size = 0;
    bridge.rx_binary = false;
    atomic_store(&bridge.sent, 0);
    atomic_store(&bridge.received, 0);
    atomic_store(&bridge.dropped, 0);
    atomic_store(&bridge.bad_gzip, 0);
    atomic_store(&bridge.stop, false);
    atomic_store(&bridge.connected, false);
    atomic_store(&bridge.running, true);
    CWL_SetBridgeActive(!loopback);
    if (xTaskCreate(bridge_task, "udp_ws", 3072, &bridge, 4, NULL) == pdPASS) return true;
    CWL_SetBridgeActive(false);
    free(bridge.inflate_state);
    bridge.inflate_state = NULL;
    atomic_store(&bridge.running, false);
    return false;
}

static const uint8_t to_websocket[] = "UDP to WebSocket\0AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
static const uint8_t to_udp[] = "WebSocket to UDP\0verified\n";
// Produced independently with gzip -n, including its CRC and size trailer.
static const uint8_t gzip_fixture[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0b, 0x4f,
    0x4d, 0x0a, 0xce, 0x4f, 0xce, 0x4e, 0x2d, 0x51, 0x28, 0xc9, 0x57, 0x08,
    0x75, 0x09, 0x60, 0x28, 0x4b, 0x2d, 0xca, 0x4c, 0xcb, 0x4c, 0x4d, 0xe1,
    0x02, 0x00, 0x34, 0xfd, 0xd0, 0x22, 0x1a, 0x00, 0x00, 0x00,
};

static esp_err_t test_websocket(httpd_req_t *request)
{
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    if (request->method == HTTP_GET) {
        if (getpeername(httpd_req_to_sockfd(request), (struct sockaddr *)&peer, &peer_size) ||
            peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return ESP_FAIL;
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {0};
    esp_err_t frame_status = httpd_ws_recv_frame(request, &frame, 0);
    if (frame_status != ESP_OK || frame.type != HTTPD_WS_TYPE_BINARY || frame.len > 128) {
        printf("bridge selftest server: invalid first frame, result=%d type=%d length=%u\n",
               frame_status, frame.type, (unsigned)frame.len);
        return ESP_FAIL;
    }
    uint8_t payload[128];
    frame.payload = payload;
    if (httpd_ws_recv_frame(request, &frame, sizeof(payload)) != ESP_OK) return ESP_FAIL;
    // Known gzip message checked with an independent decoder. Comparing it
    // here keeps a second inflate workspace out of the device's test heap.
    static const uint8_t expected[] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x0b, 0x75,
        0x09, 0x50, 0x28, 0xc9, 0x57, 0x08, 0x4f, 0x4d, 0x0a, 0xce, 0x4f, 0xce,
        0x4e, 0x2d, 0x61, 0x70, 0x24, 0x00, 0xb8, 0x00, 0x69, 0x45, 0xd5, 0xb3,
        0x32, 0x00, 0x00, 0x00,
    };
    if (frame.len != sizeof(expected) || memcmp(payload, expected, sizeof(expected))) {
        printf("bridge selftest server: gzip did not match reference payload\n");
        return ESP_FAIL;
    }
    if (atomic_exchange(&test_outbound_ok, true)) return ESP_OK;

    uint8_t corrupt[sizeof(gzip_fixture)];
    memcpy(corrupt, gzip_fixture, sizeof(corrupt));
    corrupt[sizeof(corrupt) - 8] ^= 1;
    frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_BINARY, .payload = corrupt, .len = sizeof(corrupt)};
    if (httpd_ws_send_frame(request, &frame) != ESP_OK) return ESP_FAIL;
    frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_BINARY, .fragmented = true,
                              .payload = (uint8_t *)gzip_fixture, .len = 13};
    if (httpd_ws_send_frame(request, &frame) != ESP_OK) return ESP_FAIL;
    frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_CONTINUE, .fragmented = true, .final = true,
                              .payload = (uint8_t *)gzip_fixture + 13, .len = sizeof(gzip_fixture) - 13};
    return httpd_ws_send_frame(request, &frame);
}

static void test_task(void *unused)
{
    (void)unused;
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    static unsigned next_test;
    const unsigned offset = (next_test++ % 100) * 4;
    config.server_port = TestWsPort + offset;
    config.ctrl_port = TestWsPort + offset + 3;
    config.max_open_sockets = 1;
    config.stack_size = 3072;
    const httpd_uri_t route = {.uri = "/test", .method = HTTP_GET, .handler = test_websocket, .is_websocket = true};
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons(TestBridgePort + offset)};
    int udp = -1;
    bool passed = false;
    cf_infl_t *workspace = NULL;
    bool held = false;
    atomic_store(&test_outbound_ok, false);
    if (CWL_CartHeld()) goto done;
    held = true;
    // Loopback uses no radio. Give its buffers back before starting two
    // WebSocket endpoints on the same ESP32.
    if (!CWL_CartHold(true, 2000)) goto done;
    workspace = malloc(sizeof(*workspace));
    if (!workspace) goto done;
    if (esp_netif_init() != ESP_OK) goto done;
    if (httpd_start(&server, &config) != ESP_OK || httpd_register_uri_handler(server, &route) != ESP_OK) goto done;
    udp = open_udp(TestPeerPort + offset, true, &peer);
    if (udp < 0) goto done;
    peer.sin_port = htons(TestPeerPort + offset);
    char uri[48];
    snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/test", TestWsPort + offset);
    bool started = start_bridge(uri, TestBridgePort + offset, &peer, true, workspace);
    workspace = NULL;
    if (!started) goto done;

    uint8_t reply[sizeof(to_udp) + 1];
    for (int i = 0; i < 150 && atomic_load(&bridge.running); i++) {
        if (i % 3 == 0 && atomic_load(&bridge.connected) && !atomic_load(&test_outbound_ok)) {
            (void)send(udp, to_websocket, sizeof(to_websocket) - 1, 0);
        }
        int size = recv(udp, reply, sizeof(reply), 0);
        if (size < 0) continue;
        passed = size == sizeof(to_udp) - 1 && !memcmp(reply, to_udp, size) &&
                 atomic_load(&test_outbound_ok) && atomic_load(&bridge.bad_gzip) >= 1;
        if (!passed) printf("bridge selftest: UDP reply bytes=%d expected=%u\n", size,
                            (unsigned)sizeof(to_udp) - 1);
        break;
    }

done:
    free(workspace);
    atomic_store(&bridge.stop, true);
    if (udp >= 0) close(udp);
    for (int i = 0; i < 400 && atomic_load(&bridge.running); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (server) httpd_stop(server);
    if (held) CWL_CartHold(false, 0);
    printf("bridge selftest %s: UDP->gzip->WS=%u, WS->gunzip->UDP=%u, bad gzip=%u, peer verified=%u\n",
           passed ? "PASS" : "FAIL", atomic_load(&bridge.sent), atomic_load(&bridge.received),
           atomic_load(&bridge.bad_gzip), atomic_load(&test_outbound_ok));
    atomic_store(&test_running, false);
    vTaskDelete(NULL);
}

static unsigned parse_port(const char *text)
{
    char *end;
    unsigned long port = strtoul(text, &end, 10);
    return end != text && !*end && port > 0 && port <= 65535 ? port : 0;
}

static bool start_saved_bridge(const BridgeSettings *settings)
{
    struct sockaddr_in peer = {
        .sin_family = AF_INET,
        .sin_port = htons(settings->peer_port),
        .sin_addr.s_addr = settings->peer_ip,
    };
    return start_bridge(settings->uri, settings->local_port, &peer, false, NULL);
}

void bridge_restore(void)
{
    BridgeSettings settings;
    if (saved_settings(&settings)) {
        puts(start_saved_bridge(&settings) ? "bridge: restored saved settings" :
                                           "bridge: could not start saved settings");
    }
}

static int bridge_command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "status")) {
        BridgeSettings settings;
        printf("bridge: %s, UDP->WS=%u WS->UDP=%u dropped=%u\n",
               atomic_load(&bridge.running) ? "running" : "stopped",
               atomic_load(&bridge.sent), atomic_load(&bridge.received), atomic_load(&bridge.dropped));
        printf("bridge: auto-start %s, Wi-Fi GameBoy %s, WebSocket %s\n",
               saved_settings(&settings) ? "configured" : "unconfigured",
               on_target_network() ? "connected" : "offline",
               atomic_load(&bridge.running) && atomic_load(&bridge.connected) ? "connected" : "offline");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "stop")) {
        esp_err_t result = update_settings(NULL);
        if (result != ESP_OK) {
            printf("bridge: cannot disable auto-start (%s)\n", esp_err_to_name(result));
            return 1;
        }
        atomic_store(&bridge.stop, true);
        puts("bridge: auto-start disabled");
        return 0;
    }
    if (atomic_load(&bridge.running) || atomic_load(&test_running)) {
        puts("bridge: busy");
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "selftest")) {
        atomic_store(&test_running, true);
        if (xTaskCreate(test_task, "bridge_test", 2560, NULL, 4, NULL) == pdPASS) return 0;
        atomic_store(&test_running, false);
        return 1;
    }
    if (argc == 6 && !strcmp(argv[1], "start")) {
        BridgeSettings settings = {
            .local_port = parse_port(argv[3]),
            .peer_port = parse_port(argv[5]),
        };
        struct in_addr addr;
        if (strlen(argv[2]) < sizeof(settings.uri) && inet_pton(AF_INET, argv[4], &addr) == 1) {
            strcpy(settings.uri, argv[2]);
            settings.peer_ip = addr.s_addr;
            if (valid_settings(&settings)) {
                esp_err_t result = update_settings(&settings);
                if (result != ESP_OK) {
                    printf("bridge: could not save settings (%s)\n", esp_err_to_name(result));
                    return 1;
                }
                puts("bridge: settings saved; auto-start enabled");
                return start_saved_bridge(&settings) ? 0 : 1;
            }
        }
    }
    puts("Usage: bridge start <ws(s)://url> <local-udp-port> <peer-ipv4> <peer-udp-port> | stop | status | selftest");
    return 1;
}

void bridge_register(void)
{
    const esp_console_cmd_t command = {
        .command = "bridge",
        .help = "Bridge one UDP peer and one WebSocket with gzip, or run the on-device self-test",
        .func = bridge_command,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&command));
}

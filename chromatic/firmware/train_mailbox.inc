/* Local network-window opcode for The Last Train. */
#include <stdbool.h>
#include "train_protocol.h"

static int train_socket = -1;
static uint8_t train_request_id;
static uint8_t train_sequence[2], train_stop;
static bool train_pending;

static void train_close(void)
{
    if (train_socket >= 0) close(train_socket);
    train_socket = -1;
    train_pending = false;
}

static void train_start(nw_esp_t *e, const uint8_t *body, uint16_t len)
{
    if (len != TRAIN_REQUEST_SIZE || body[0] != 'L' || body[1] != 'T' || body[2] != TRAIN_VERSION) {
        respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0);
        return;
    }
    if (train_socket < 0) {
        const struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = htons(TRAIN_PORT),
            .sin_addr.s_addr = htonl(0x0a2a0001u),
        };
        train_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (train_socket < 0 ||
            fcntl(train_socket, F_SETFL, O_NONBLOCK) < 0 ||
            connect(train_socket, (const struct sockaddr *)&peer, sizeof peer) < 0) {
            train_close();
            respond(e, NW_S_OFFLINE, 0, 0, NULL, 0);
            return;
        }
    }
    if (send(train_socket, body, len, 0) != len) {
        train_close();
        respond(e, NW_S_OFFLINE, 0, 0, NULL, 0);
        return;
    }
    train_request_id = e->id;
    train_sequence[0] = body[3];
    train_sequence[1] = body[4];
    train_stop = body[9];
    train_pending = true;
    e->state = NW_ESP_CALLING;
    e->call_started = e->now;
}

static void train_tick(nw_esp_t *e)
{
    if (!train_pending) return;
    if (e->state != NW_ESP_CALLING || e->id != train_request_id) {
        train_pending = false;
        return;
    }
    uint8_t *reply = e->resp + NW_RESP_HDR;
    for (uint8_t i = 0; i < 4; ++i) {
        const int len = recv(train_socket, reply, TRAIN_REPLY_SIZE + 1u, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            train_close();
            nw_esp_complete(e, e->id, NW_S_OFFLINE, 0, 0, NULL, 0);
            return;
        }
        if (len == TRAIN_REPLY_SIZE && reply[0] == 'T' && reply[1] == 'L' &&
            reply[2] == TRAIN_VERSION && reply[3] == train_sequence[0] &&
            reply[4] == train_sequence[1] && reply[5] == train_stop) {
            train_pending = false;
            nw_esp_complete(e, e->id, NW_S_OK, 0, len, reply, len);
            return;
        }
    }
}

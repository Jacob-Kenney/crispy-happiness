/* Included by nw_esp.c. The local opcode exchanges one Canyon Racer snapshot
 * with the Jetson on GameBoy and completes on a later network-window tick. */
#include <errno.h>
#include <fcntl.h>
#include "lwip/sockets.h"

#define CANYON_OP 0x30u
#define CANYON_REQUEST 90u
#define CANYON_REPLY 5u
#define CANYON_PORT 47821u

static int canyon_socket = -1;
static uint8_t canyon_sequence[2];
static uint8_t canyon_id;
static int canyon_pending;

static void canyon_close(void)
{
    if (canyon_socket >= 0) close(canyon_socket);
    canyon_socket = -1;
    canyon_pending = 0;
}

static void canyon_start(nw_esp_t *e, const uint8_t *body, uint16_t len)
{
    if (len != CANYON_REQUEST || body[0] != 'C' || body[1] != 'R' || body[2] != 1) {
        respond(e, NW_S_BAD_REQUEST, 0, 0, NULL, 0);
        return;
    }
    if (canyon_socket < 0) {
        struct sockaddr_in peer = {
            .sin_family = AF_INET,
            .sin_port = htons(CANYON_PORT),
            .sin_addr.s_addr = htonl(0x0a2a0001u), /* 10.42.0.1 */
        };
        canyon_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (canyon_socket < 0 ||
            fcntl(canyon_socket, F_SETFL, O_NONBLOCK) < 0 ||
            connect(canyon_socket, (const struct sockaddr *)&peer, sizeof peer) < 0) {
            canyon_close();
            respond(e, NW_S_OFFLINE, 0, 0, NULL, 0);
            return;
        }
    }
    if (send(canyon_socket, body, len, 0) != len) {
        canyon_close();
        respond(e, NW_S_OFFLINE, 0, 0, NULL, 0);
        return;
    }
    canyon_id = e->id;
    canyon_sequence[0] = body[4];
    canyon_sequence[1] = body[5];
    canyon_pending = 1;
    e->state = NW_ESP_CALLING;
    e->call_started = e->now;
}

static void canyon_tick(nw_esp_t *e)
{
    if (!canyon_pending) return;
    if (e->state != NW_ESP_CALLING || e->id != canyon_id) {
        canyon_pending = 0;
        return;
    }
    uint8_t reply[CANYON_REPLY + 1u];
    for (uint8_t i = 0; i < 4; ++i) {
        const int len = recv(canyon_socket, reply, sizeof reply, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            canyon_close();
            nw_esp_complete(e, e->id, NW_S_OFFLINE, 0, 0, NULL, 0);
            return;
        }
        if (len == CANYON_REPLY && reply[0] == 'C' && reply[1] == 'A' &&
            reply[2] == canyon_sequence[0] && reply[3] == canyon_sequence[1]) {
            canyon_pending = 0;
            nw_esp_complete(e, e->id, NW_S_OK, 0, CANYON_REPLY, reply, CANYON_REPLY);
            return;
        }
    }
}

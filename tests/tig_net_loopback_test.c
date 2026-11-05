/*
 * Headless loopback test for Coop Lite tig_net (ENet).
 *
 *   tig_net_loopback_test              — host; waits for client, round-trips
 *   tig_net_loopback_test --client HOST PORT — client peer
 *
 * The CTest wrapper runs host+client as two processes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>

#include <enet/enet.h>

#include "net/tig_net.h"

#define TEST_TYPE 67
#define MAGIC_VALUE 0xC00F1

typedef struct TestPacket {
    int type;
    int field_4;
} TestPacket;

static volatile int g_received;
static volatile int g_received_value;
static volatile int g_my_slot = -1;
static volatile int g_peer_connected;

static bool test_validate(void* msg)
{
    int type = *(int*)msg;
    return type > 0 && type < 131;
}

static void test_on_message(void* msg)
{
    TestPacket* pkt = (TestPacket*)msg;
    if (pkt->type == TEST_TYPE) {
        g_received++;
        g_received_value = pkt->field_4;
    }
}

static bool test_on_event(int type, int client_id, void* data, int size)
{
    (void)data;
    (void)size;
    if (type == TIG_NET_EVENT_CONNECT) {
        g_peer_connected++;
        if (!tig_net_is_host()) {
            g_my_slot = client_id;
        }
    }
    return true;
}

static void pump_ms(int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        tig_net_pump();
        usleep(1000);
    }
}

static int run_client(const char* host, int port)
{
    TestPacket pkt;

    tig_net_set_coop_enabled(true);
    tig_net_local_client_set_name("Client");
    tig_net_configure_join(host, port);
    tig_net_on_message(test_on_message);
    tig_net_on_message_validation(test_validate);
    tig_net_on_network_event(test_on_event);

    if (tig_net_start_client() != TIG_OK || !tig_net_is_active()) {
        fprintf(stderr, "client: connect failed\n");
        return 1;
    }

    /* Wait for WELCOME (slot assignment). */
    pump_ms(2000);
    if (g_my_slot < 1) {
        fprintf(stderr, "client: no slot assigned (got %d)\n", g_my_slot);
        return 1;
    }
    printf("client: assigned slot %d\n", g_my_slot);

    /* Wait for host packet, then reply. */
    {
        int waits = 0;
        while (g_received == 0 && waits < 3000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (g_received == 0 || g_received_value != MAGIC_VALUE) {
        fprintf(stderr, "client: did not receive host packet (recv=%d val=%d)\n",
            g_received, g_received_value);
        return 1;
    }
    printf("client: got host packet value=%d\n", g_received_value);

    pkt.type = TEST_TYPE;
    pkt.field_4 = MAGIC_VALUE ^ 0xFF;
    tig_net_send_app_all(&pkt, sizeof(pkt));
    pump_ms(200);

    tig_net_reset_connection();
    printf("client: PASS\n");
    return 0;
}

static int run_host(int port, int wait_client_ms)
{
    TestPacket pkt;
    int waits;

    tig_net_set_coop_enabled(true);
    tig_net_local_server_set_max_players(8);
    tig_net_local_client_set_name("Host");
    tig_net_configure_host(port);
    tig_net_on_message(test_on_message);
    tig_net_on_message_validation(test_validate);
    tig_net_on_network_event(test_on_event);

    if (tig_net_start_client() != TIG_OK || !tig_net_is_host()) {
        fprintf(stderr, "host: start failed\n");
        return 1;
    }
    if (sub_529520() != 0) {
        fprintf(stderr, "host: local slot %d != 0\n", sub_529520());
        return 1;
    }
    printf("host: listening on %d slot 0\n", port);

    waits = 0;
    while (!tig_net_client_is_active(1) && waits < wait_client_ms) {
        tig_net_pump();
        usleep(1000);
        waits++;
    }
    if (!tig_net_client_is_active(1)) {
        fprintf(stderr, "host: client never joined slot 1\n");
        return 1;
    }
    printf("host: client slot 1 active\n");

    pkt.type = TEST_TYPE;
    pkt.field_4 = MAGIC_VALUE;
    tig_net_send_app_all(&pkt, sizeof(pkt));
    pump_ms(100);

    waits = 0;
    g_received = 0;
    while (g_received == 0 && waits < 3000) {
        tig_net_pump();
        usleep(1000);
        waits++;
    }
    if (g_received == 0 || g_received_value != (MAGIC_VALUE ^ 0xFF)) {
        fprintf(stderr, "host: no client reply (recv=%d val=%d)\n",
            g_received, g_received_value);
        return 1;
    }
    printf("host: got client reply value=%d\n", g_received_value);

    if (sub_52A530() != 1) {
        /* Sender should be client slot 1 when host receives. */
        printf("host: WARN last sender slot=%d (expected 1)\n", sub_52A530());
    } else {
        printf("host: last sender slot 1 OK\n");
    }

    tig_net_reset_connection();
    printf("host: PASS\n");
    return 0;
}

/* Review PT24 #1: a raw ENet peer that never sends the HELLO. Its app
   packet must not reach the game, and the host must drop it after the
   handshake timeout. Exit 0 = it was disconnected. */
static int run_rawpeer(int port)
{
    ENetHost* h;
    ENetAddress addr;
    ENetPeer* peer;
    ENetEvent ev;
    TestPacket pkt;
    int ms;
    bool sent = false;

    if (enet_initialize() != 0) {
        return 1;
    }
    h = enet_host_create(NULL, 1, 3, 0, 0);
    enet_address_set_host(&addr, "127.0.0.1");
    addr.port = (enet_uint16)port;
    peer = h != NULL ? enet_host_connect(h, &addr, 3, 0) : NULL;
    if (peer == NULL) {
        return 1;
    }
    for (ms = 0; ms < 9000; ms += 10) {
        while (enet_host_service(h, &ev, 10) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT && !sent) {
                pkt.type = TEST_TYPE;
                pkt.field_4 = 0xBAD;
                enet_peer_send(peer, 0, enet_packet_create(&pkt, sizeof(pkt), ENET_PACKET_FLAG_RELIABLE));
                enet_host_flush(h);
                sent = true;
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                printf("rawpeer: disconnected by host after %d ms\n", ms);
                enet_host_destroy(h);
                return sent ? 0 : 1;
            }
        }
    }
    fprintf(stderr, "rawpeer: never disconnected\n");
    return 1;
}

static int run_gate_host(int port)
{
    int ms;

    tig_net_configure_host(port);
    g_received = 0;
    g_peer_connected = 0;
    if (tig_net_start_client() != TIG_OK || !tig_net_is_host()) {
        fprintf(stderr, "gate host: start failed\n");
        return 1;
    }
    for (ms = 0; ms < 9000; ms++) {
        tig_net_pump();
        usleep(1000);
    }
    tig_net_reset_connection();
    if (g_received != 0 || g_peer_connected != 1) {
        /* 1 = our own hosting CONNECT; the raw peer must not count. */
        fprintf(stderr, "gate host: pre-handshake traffic leaked (recv=%d connects=%d)\n",
            g_received, g_peer_connected);
        return 1;
    }
    printf("gate host: un-handshaken peer got no app delivery, no CONNECT event\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && strcmp(argv[1], "--rawpeer") == 0) {
        return run_rawpeer(atoi(argv[2]));
    }
    if (argc >= 2 && strcmp(argv[1], "--client") == 0) {
        const char* host = argc >= 3 ? argv[2] : "127.0.0.1";
        int port = argc >= 4 ? atoi(argv[3]) : TIG_NET_DEFAULT_PORT;
        return run_client(host, port);
    }

    if (argc >= 2 && strcmp(argv[1], "--host") == 0) {
        int port = argc >= 3 ? atoi(argv[2]) : 17777;
        return run_host(port, 15000);
    }

    /* Default: spawn client child and run host. */
    {
        int port = 17777;
        pid_t child;
        char self[512];
        char port_str[16];
        int status = 1;
        ssize_t n;

        n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n < 0) {
            fprintf(stderr, "cannot resolve self path\n");
            return 1;
        }
        self[n] = '\0';
        snprintf(port_str, sizeof(port_str), "%d", port);

        child = fork();
        if (child < 0) {
            perror("fork");
            return 1;
        }
        if (child == 0) {
            /* Give host a moment to bind. */
            usleep(400000);
            execl(self, self, "--client", "127.0.0.1", port_str, (char*)NULL);
            perror("execl");
            _exit(1);
        }

        if (run_host(port, 10000) != 0) {
            return 1;
        }
        waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "client child failed status=%d\n", status);
            return 1;
        }
        printf("PASS: host/client connect, slot assign, packet round-trip\n");

        snprintf(port_str, sizeof(port_str), "%d", port + 1);
        child = fork();
        if (child < 0) {
            perror("fork");
            return 1;
        }
        if (child == 0) {
            usleep(400000);
            execl(self, self, "--rawpeer", port_str, (char*)NULL);
            _exit(1);
        }
        if (run_gate_host(port + 1) != 0) {
            return 1;
        }
        waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "rawpeer child failed status=%d\n", status);
            return 1;
        }
        printf("PASS: handshake gate + timeout\n");
        return 0;
    }
}

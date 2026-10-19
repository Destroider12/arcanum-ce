/*
 * Policy + follow-packet round-trip for Coop Lite priority 1.
 * Host/client via fork; verifies worldmap/time gates and PACKET_COOP_FOLLOW_HOST.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "game/coop_lite.h"
/* Minimal TeleportData matching coop_lite.c test stubs. */
typedef unsigned int TeleportFlags;
typedef struct TeleportData {
    TeleportFlags flags;
    int64_t obj;
    int64_t loc;
    int map;
    int movie1;
    unsigned int movie_flags1;
    int movie2;
    unsigned int movie_flags2;
    char fade_pad[40];
    int sound_id;
    int time;
} TeleportData;
#include "net/tig_net.h"

#define TEST_PORT 17887

static int g_applied_map = -1;
static int64_t g_applied_loc = 0;
static int g_teleport_calls = 0;

/* Stubs used by coop_lite.c when COOP_LITE_TEST_STUBS is defined. */
int64_t player_get_local_pc_obj(void)
{
    return 1;
}

bool teleport_do(TeleportData* teleport_data)
{
    g_teleport_calls++;
    g_applied_map = teleport_data->map;
    g_applied_loc = teleport_data->loc;
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

static void on_message(void* msg)
{
    int type = *(int*)msg;
    if (type == PACKET_COOP_FOLLOW_HOST) {
        coop_lite_handle_follow_packet((PacketCoopFollowHost*)msg);
    }
}

static bool on_validate(void* msg)
{
    int type = *(int*)msg;
    return type > 0 && type < 131;
}

static int run_client(void)
{
    tig_net_set_coop_enabled(true);
    tig_net_configure_join("127.0.0.1", TEST_PORT);
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_local_client_set_name("Client");

    if (tig_net_start_client() != TIG_OK) {
        fprintf(stderr, "client: start failed\n");
        return 1;
    }

    /* Wait for WELCOME / slot assignment without racing host teardown. */
    {
        int waits = 0;
        while (sub_529520() < 1 && waits < 3000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (!tig_net_is_active() || tig_net_is_host() || sub_529520() < 1) {
        fprintf(stderr, "client: not a connected client (active=%d host=%d slot=%d)\n",
            tig_net_is_active(), tig_net_is_host(), sub_529520());
        return 1;
    }
    if (coop_lite_can_use_worldmap()) {
        fprintf(stderr, "client: worldmap should be blocked\n");
        return 1;
    }
    if (coop_lite_can_advance_game_time()) {
        fprintf(stderr, "client: time advance should be blocked\n");
        return 1;
    }
    printf("client: worldmap/time gates OK\n");

    g_teleport_calls = 0;
    g_applied_map = -1;
    {
        int waits = 0;
        while (g_teleport_calls == 0 && waits < 5000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (g_teleport_calls != 1 || g_applied_map != 42 || g_applied_loc != 0x1234) {
        fprintf(stderr, "client: follow not applied (calls=%d map=%d loc=%lld)\n",
            g_teleport_calls, g_applied_map, (long long)g_applied_loc);
        return 1;
    }
    printf("client: follow applied map=%d loc=%lld\n", g_applied_map, (long long)g_applied_loc);
    tig_net_reset_connection();
    printf("client: PASS\n");
    return 0;
}

static int run_host(void)
{
    tig_net_set_coop_enabled(true);
    tig_net_configure_host(TEST_PORT);
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_local_client_set_name("Host");
    tig_net_local_server_set_max_players(8);

    if (tig_net_start_client() != TIG_OK || !tig_net_is_host()) {
        fprintf(stderr, "host: start failed\n");
        return 1;
    }
    if (!coop_lite_can_use_worldmap() || !coop_lite_can_advance_game_time()) {
        fprintf(stderr, "host: should allow worldmap/time\n");
        return 1;
    }
    printf("host: worldmap/time gates OK\n");

    {
        int waits = 0;
        while (!tig_net_client_is_active(1) && waits < 10000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (!tig_net_client_is_active(1)) {
        fprintf(stderr, "host: no client\n");
        return 1;
    }

    coop_lite_notify_host_teleported(42, 0x1234);
    pump_ms(800);

    /* Do not reset here — keep the session up until the child client exits. */
    printf("host: PASS\n");
    return 0;
}

int main(int argc, char** argv)
{
    /* SP policy without coop. */
    tig_net_set_coop_enabled(false);
    if (!coop_lite_can_use_worldmap() || !coop_lite_can_advance_game_time()) {
        fprintf(stderr, "SP: gates should allow\n");
        return 1;
    }
    printf("SP: worldmap/time allowed OK\n");

    if (argc >= 2 && strcmp(argv[1], "--client") == 0) {
        return run_client();
    }
    if (argc >= 2 && strcmp(argv[1], "--host") == 0) {
        return run_host();
    }

    {
        char self[512];
        char port_str[16];
        pid_t child;
        int status;
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n < 0) {
            return 1;
        }
        self[n] = '\0';
        child = fork();
        if (child == 0) {
            usleep(400000);
            execl(self, self, "--client", (char*)NULL);
            _exit(1);
        }
        if (run_host() != 0) {
            return 1;
        }
        waitpid(child, &status, 0);
        tig_net_reset_connection();
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "client failed\n");
            return 1;
        }
        printf("PASS: coop_lite worldmap/time gates + follow packet\n");
        return 0;
    }
}

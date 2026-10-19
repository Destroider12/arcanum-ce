/*
 * Session sync + New Game skip policy for Coop Lite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#include "game/coop_lite.h"

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

#define TEST_PORT 17899

static int g_got_char_offer;
static int g_teleport_calls;
static int g_applied_map = -1;

int64_t player_get_local_pc_obj(void)
{
    return 42;
}

bool teleport_do(TeleportData* teleport_data)
{
    g_teleport_calls++;
    g_applied_map = teleport_data->map;
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
    if (type == PACKET_COOP_CHAR_OFFER) {
        g_got_char_offer++;
        coop_lite_handle_char_offer((PacketCoopCharOffer*)msg);
    } else if (type == PACKET_COOP_SESSION_SYNC) {
        coop_lite_handle_session_sync((PacketCoopSessionSync*)msg);
    }
}

static bool on_validate(void* msg)
{
    int type = *(int*)msg;
    return type > 0 && type < PACKET_COOP_TYPE_LIMIT;
}

static int test_skip_and_roles(void)
{
    tig_net_set_coop_enabled(false);
    if (coop_lite_should_skip_independent_new_game()) {
        fprintf(stderr, "offline should not skip NG\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(PACKET_COOP_CHAR_OFFER) != COOP_MP_PKT_HOST_RESOLVE) {
        fprintf(stderr, "CHAR_OFFER role\n");
        return 1;
    }
    if (coop_lite_mp_packet_role(PACKET_COOP_SESSION_SYNC) != COOP_MP_PKT_CLIENT_APPLY) {
        fprintf(stderr, "SESSION_SYNC role\n");
        return 1;
    }
    return 0;
}

static int run_client(void)
{
    PacketCoopSessionSync sync;

    tig_net_set_coop_enabled(true);
    tig_net_configure_join("127.0.0.1", TEST_PORT);
    tig_net_on_message(on_message);
    tig_net_on_message_validation(on_validate);
    tig_net_local_client_set_name("Client");

    if (tig_net_start_client() != TIG_OK) {
        fprintf(stderr, "client: start failed\n");
        return 1;
    }
    {
        int waits = 0;
        while (sub_529520() < 1 && waits < 3000) {
            tig_net_pump();
            usleep(1000);
            waits++;
        }
    }
    if (!tig_net_is_active() || tig_net_is_host()) {
        fprintf(stderr, "client: not connected as client\n");
        return 1;
    }
    if (!coop_lite_should_skip_independent_new_game()) {
        fprintf(stderr, "client: should skip independent NG\n");
        return 1;
    }
    if (!coop_lite_client_offer_character(16067, "TestCompanion")) {
        fprintf(stderr, "client: offer failed\n");
        return 1;
    }
    if (!coop_lite_client_waiting_for_sync()) {
        fprintf(stderr, "client: expected waiting\n");
        return 1;
    }
    pump_ms(200);

    memset(&sync, 0, sizeof(sync));
    sync.type = PACKET_COOP_SESSION_SYNC;
    sync.map = 7;
    sync.host_loc = 100;
    sync.client_loc = 200;
    sync.client_slot = 1;
    sync.client_proto = 16067;
    sync.host_proto = 16066;
    strncpy(sync.client_name, "TestCompanion", sizeof(sync.client_name) - 1);
    /* A teleport-done with no pending sync must not mark the session ready. */
    coop_lite_client_on_teleport_done();
    if (coop_lite_client_session_ready()) {
        fprintf(stderr, "client: ready without SESSION_SYNC\n");
        return 1;
    }

    coop_lite_handle_session_sync(&sync);

    /* Ordering: map load (teleport) is requested first; PCs are bound only
       once the teleport completes. */
    if (g_teleport_calls < 1 || g_applied_map != 7) {
        fprintf(stderr, "client: teleport map=%d calls=%d\n", g_applied_map, g_teleport_calls);
        return 1;
    }
    if (coop_lite_client_session_ready() || !coop_lite_client_waiting_for_sync()) {
        fprintf(stderr, "client: session bound before map load finished\n");
        return 1;
    }

    coop_lite_client_on_teleport_done(); /* teleport.c hook */
    if (!coop_lite_client_session_ready() || coop_lite_client_waiting_for_sync()) {
        fprintf(stderr, "client: expected session_ready after teleport done\n");
        return 1;
    }
    /* Second teleport-done (later travel) is a no-op. */
    coop_lite_client_on_teleport_done();
    coop_lite_acknowledge_session_ready();
    if (coop_lite_client_session_ready()) {
        fprintf(stderr, "client: ack failed\n");
        return 1;
    }
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
    pump_ms(800);
    if (g_got_char_offer < 1) {
        fprintf(stderr, "host: missed CHAR_OFFER\n");
        return 1;
    }
    printf("host: PASS (got CHAR_OFFER)\n");
    return 0;
}

int main(int argc, char** argv)
{
    pid_t pid;
    int status;

    (void)argv;
    coop_lite_session_reset();
    g_got_char_offer = 0;
    g_teleport_calls = 0;
    g_applied_map = -1;

    if (test_skip_and_roles() != 0) {
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "client") == 0) {
        return run_client();
    }
    if (argc > 1 && strcmp(argv[1], "host") == 0) {
        return run_host();
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        _exit(run_client());
    }
    usleep(100000);
    if (run_host() != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return 1;
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "client child failed (%d)\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 1;
    }
    printf("coop_lite_session_sync_test OK\n");
    return 0;
}

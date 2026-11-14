/* PT58: the ENet backend - the exact ENet calls tig_net made before. */
#include "net/tig_net_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <enet/enet.h>

#ifdef TIG_NET_TEST_STUB_DEBUG
void tig_debug_printf(const char* fmt, ...);
#else
#include "tig/debug.h"
#endif

static ENetHost* tne_host;
static bool tne_ready;

static bool tne_init(void)
{
    if (tne_ready) {
        return true;
    }
    if (enet_initialize() != 0) {
        tig_debug_printf("tig_net: enet_initialize failed\n");
        return false;
    }
    tne_ready = true;
    atexit(enet_deinitialize);
    return true;
}

static bool tne_listen(int port, int max_peers, int channels)
{
    ENetAddress address;

    if (tne_host != NULL) {
        return true;
    }
    address.host = ENET_HOST_ANY;
    address.port = (enet_uint16)port;
    tne_host = enet_host_create(&address, (size_t)max_peers, (size_t)channels, 0, 0);
    return tne_host != NULL;
}

static TigNetTPeer* tne_connect(const char* target, int port, int channels)
{
    ENetAddress address;
    ENetPeer* peer;

    tne_host = enet_host_create(NULL, 1, (size_t)channels, 0, 0);
    if (tne_host == NULL) {
        tig_debug_printf("tig_net: client host create failed\n");
        return NULL;
    }
    if (enet_address_set_host(&address, target) != 0) {
        /* Review PT24 #2: unresolvable name - address.host is garbage. */
        tig_debug_printf("tig_net: cannot resolve join host '%s'\n", target);
        enet_host_destroy(tne_host);
        tne_host = NULL;
        return NULL;
    }
    address.port = (enet_uint16)port;
    peer = enet_host_connect(tne_host, &address, (size_t)channels, 0);
    if (peer == NULL) {
        tig_debug_printf("tig_net: connect to %s:%d failed\n", target, port);
        enet_host_destroy(tne_host);
        tne_host = NULL;
        return NULL;
    }
    return (TigNetTPeer*)peer;
}

static int tne_service(TigNetTEvent* ev, unsigned int timeout_ms)
{
    ENetEvent e;

    memset(ev, 0, sizeof(*ev));
    if (tne_host == NULL || enet_host_service(tne_host, &e, timeout_ms) <= 0) {
        return 0;
    }
    ev->peer = (TigNetTPeer*)e.peer;
    switch (e.type) {
    case ENET_EVENT_TYPE_CONNECT:
        ev->type = TIG_NET_TEV_CONNECT;
        break;
    case ENET_EVENT_TYPE_RECEIVE:
        ev->type = TIG_NET_TEV_RECEIVE;
        ev->channel = e.channelID;
        ev->data = e.packet->data;
        ev->size = e.packet->dataLength;
        ev->internal = e.packet;
        break;
    case ENET_EVENT_TYPE_DISCONNECT:
        ev->type = TIG_NET_TEV_DISCONNECT;
        break;
    default:
        ev->type = TIG_NET_TEV_NONE;
        break;
    }
    return 1;
}

static void tne_event_done(TigNetTEvent* ev)
{
    if (ev != NULL && ev->internal != NULL) {
        enet_packet_destroy((ENetPacket*)ev->internal);
        ev->internal = NULL;
    }
}

static int tne_send(TigNetTPeer* const* peers, int count, int channel, const void* data, size_t size, bool reliable)
{
    ENetPacket* packet;
    int i;
    int sent = 0;

    if (tne_host == NULL || count <= 0) {
        return 0;
    }
    /* Review L3: one packet for every recipient; ENet takes a reference per
       successful send, and a failed send leaves it ours to free. */
    packet = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet == NULL) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (peers[i] != NULL && enet_peer_send((ENetPeer*)peers[i], (enet_uint8)channel, packet) == 0) {
            sent++;
        }
    }
    if (packet->referenceCount == 0) {
        enet_packet_destroy(packet);
    }
    return sent;
}

static void tne_flush(void)
{
    if (tne_host != NULL) {
        enet_host_flush(tne_host);
    }
}

static void tne_disconnect(TigNetTPeer* peer, bool now)
{
    if (peer == NULL) {
        return;
    }
    if (now) {
        enet_peer_disconnect_now((ENetPeer*)peer, 0);
    } else {
        enet_peer_disconnect((ENetPeer*)peer, 0);
    }
}

static void tne_reset_peer(TigNetTPeer* peer)
{
    if (peer != NULL) {
        enet_peer_reset((ENetPeer*)peer);
    }
}

static void tne_set_timeout(TigNetTPeer* peer, unsigned int min_ms, unsigned int max_ms)
{
    if (peer != NULL) {
        enet_peer_timeout((ENetPeer*)peer, 32, min_ms, max_ms);
    }
}

static void tne_close(void)
{
    if (tne_host != NULL) {
        enet_host_destroy(tne_host);
        tne_host = NULL;
    }
}

static bool tne_is_open(void)
{
    return tne_host != NULL;
}

static void** tne_peer_data(TigNetTPeer* peer)
{
    return &((ENetPeer*)peer)->data;
}

static unsigned int tne_now_ms(void)
{
    return (unsigned int)enet_time_get();
}

static void tne_describe_peer(TigNetTPeer* peer, char* buf, size_t size)
{
    ENetPeer* p = (ENetPeer*)peer;

    /* PT23: queue depth / RTT per peer */
    snprintf(buf, size, "rtt=%u ms lost=%u throttle=%u inflight=%u q_out=%u q_rel=%u q_sent=%u",
        p->roundTripTime, p->packetsLost, p->packetThrottle, p->reliableDataInTransit,
        (unsigned int)enet_list_size(&p->outgoingCommands),
        (unsigned int)enet_list_size(&p->outgoingSendReliableCommands),
        (unsigned int)enet_list_size(&p->sentReliableCommands));
}

static bool tne_local_id(char* buf, size_t size)
{
    (void)buf;
    (void)size;
    return false;
}

static const TigNetTransport tne_transport = {
    "enet",
    tne_init,
    tne_listen,
    tne_connect,
    tne_service,
    tne_event_done,
    tne_send,
    tne_flush,
    tne_disconnect,
    tne_reset_peer,
    tne_set_timeout,
    tne_close,
    tne_is_open,
    tne_peer_data,
    tne_now_ms,
    tne_describe_peer,
    tne_local_id,
};

const TigNetTransport* tig_net_transport_enet(void)
{
    return &tne_transport;
}

const TigNetTransport* tig_net_transport_pick(const char* v)
{
    if (v != NULL && strcasecmp(v, "steam") == 0) {
        return tig_net_transport_steam();
    }
    if (v != NULL && v[0] != '\0' && strcasecmp(v, "enet") != 0 && strcasecmp(v, "direct") != 0
        && strcasecmp(v, "upnp") != 0) {
        tig_debug_printf("tig_net: unknown ARCANUM_COOP_TRANSPORT '%s' - using ENet (direct)\n", v);
    }
    return tig_net_transport_enet();
}

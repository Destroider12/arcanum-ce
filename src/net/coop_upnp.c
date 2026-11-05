#include "net/coop_upnp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PT55: English unless ARCANUM_COOP_LANG=ru (then "RU / EN"), same rule as
   coop_lite_lang_ru(); the main menu status line embeds this text. */
static bool coop_upnp_ru(void)
{
    const char* v = getenv("ARCANUM_COOP_LANG");

    return v != NULL && (v[0] == 'r' || v[0] == 'R') && (v[1] == 'u' || v[1] == 'U');
}

void coop_upnp_format(CoopUpnpState state, int port, const char* ext_ip, const char* err, char* buf, size_t size)
{
    bool ru = coop_upnp_ru();

    if (buf == NULL || size == 0) {
        return;
    }
    switch (state) {
    case COOP_UPNP_BUSY:
        snprintf(buf, size, ru ? "UPnP: проброс / mapping UDP %d..." : "UPnP: mapping UDP %d...", port);
        break;
    case COOP_UPNP_MAPPED:
        snprintf(buf, size, ru ? "UPnP: UDP %d открыт / open, IP %s" : "UPnP: UDP %d open, IP %s", port,
            ext_ip != NULL && ext_ip[0] != '\0' ? ext_ip : "?");
        break;
    case COOP_UPNP_FAILED:
        snprintf(buf, size, ru ? "UPnP: ошибка / failed (%s), UDP %d вручную / by hand" : "UPnP: failed (%s), forward UDP %d by hand",
            err != NULL && err[0] != '\0' ? err : "?", port);
        break;
    default:
        snprintf(buf, size, "%s", ru ? "UPnP: выкл / off" : "UPnP: off");
        break;
    }
}

#ifdef ARCANUM_UPNP
#include <SDL3/SDL.h>
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include "tig/debug.h"

/* Invariant (PT52d): coop_upnp_ext/err/port are written only while the
   state is OFF or BUSY (start on the main thread, then the worker), and the
   worker publishes MAPPED/FAILED with SDL_SetAtomicInt after the write.
   coop_upnp_format reads the strings only for MAPPED/FAILED, so no reader
   overlaps a writer. Keep it that way (or add a mutex) when adding states.
   Lease "0" (permanent) on purpose: many IGDv1 routers reject finite leases
   (725 OnlyPermanentLeasesSupported); stop/atexit removes the mapping. */
static SDL_Thread* coop_upnp_thread;
static SDL_AtomicInt coop_upnp_st; /* CoopUpnpState */
static int coop_upnp_port;
static char coop_upnp_ext[64];
static char coop_upnp_err[96];
static char coop_upnp_lan[64];
static struct UPNPUrls coop_upnp_urls;
static struct IGDdatas coop_upnp_data;
static bool coop_upnp_have_urls;
static bool coop_upnp_atexit;

static int SDLCALL coop_upnp_worker(void* arg)
{
    struct UPNPDev* devs;
    int error = 0;
    int igd;
    int rc;
    char port_str[16];
    char wan[64];

    (void)arg;
    devs = upnpDiscover(1500, NULL, NULL, 0, 0, 2, &error);
    if (devs == NULL) {
        snprintf(coop_upnp_err, sizeof(coop_upnp_err), "no UPnP router found");
        SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_FAILED);
        return 0;
    }
    /* PT52b: a restart after coop_upnp_stop reuses the statics */
    memset(&coop_upnp_urls, 0, sizeof(coop_upnp_urls));
    memset(&coop_upnp_data, 0, sizeof(coop_upnp_data));
    igd = UPNP_GetValidIGD(devs, &coop_upnp_urls, &coop_upnp_data, coop_upnp_lan, sizeof(coop_upnp_lan), wan,
        sizeof(wan));
    freeUPNPDevlist(devs);
    if (igd <= 0) {
        if (coop_upnp_urls.controlURL != NULL) {
            FreeUPNPUrls(&coop_upnp_urls); /* defensive: never leak a partial fill */
        }
        snprintf(coop_upnp_err, sizeof(coop_upnp_err), "no internet gateway device");
        SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_FAILED);
        return 0;
    }
    coop_upnp_have_urls = true;
    if (igd == 1 && (coop_upnp_urls.controlURL == NULL || coop_upnp_urls.controlURL[0] == '\0'
                        || coop_upnp_data.first.servicetype[0] == '\0')) {
        snprintf(coop_upnp_err, sizeof(coop_upnp_err), "gateway without WAN control URL"); /* PT52g */
        SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_FAILED);
        return 0;
    }
    if (igd != 1) {
        snprintf(coop_upnp_err, sizeof(coop_upnp_err), "gateway not connected or private address");
        SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_FAILED);
        return 0;
    }
    snprintf(port_str, sizeof(port_str), "%d", coop_upnp_port);
    rc = UPNP_AddPortMapping(coop_upnp_urls.controlURL, coop_upnp_data.first.servicetype, port_str, port_str,
        coop_upnp_lan, "Arcanum Coop", "UDP", NULL, "0");
    if (rc != UPNPCOMMAND_SUCCESS) {
        /* PT52g: a stale mapping (crash, 718 ConflictInMappingEntry): drop it, retry once */
        tig_debug_printf("CoopUPnP: AddPortMapping %d, delete + retry\n", rc);
        UPNP_DeletePortMapping(coop_upnp_urls.controlURL, coop_upnp_data.first.servicetype, port_str, "UDP", NULL);
        rc = UPNP_AddPortMapping(coop_upnp_urls.controlURL, coop_upnp_data.first.servicetype, port_str, port_str,
            coop_upnp_lan, "Arcanum Coop", "UDP", NULL, "0");
    }
    if (rc != UPNPCOMMAND_SUCCESS) {
        snprintf(coop_upnp_err, sizeof(coop_upnp_err), "AddPortMapping %d %s", rc, strupnperror(rc));
        SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_FAILED);
        return 0;
    }
    if (UPNP_GetExternalIPAddress(coop_upnp_urls.controlURL, coop_upnp_data.first.servicetype, coop_upnp_ext)
        != UPNPCOMMAND_SUCCESS) {
        snprintf(coop_upnp_ext, sizeof(coop_upnp_ext), "%s", wan);
    }
    SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_MAPPED);
    return 0;
}

static void coop_upnp_join(void)
{
    char buf[160];

    if (coop_upnp_thread != NULL) {
        SDL_WaitThread(coop_upnp_thread, NULL);
        coop_upnp_thread = NULL;
        coop_upnp_describe(buf, sizeof(buf));
        tig_debug_printf("CoopUPnP: %s (LAN %s)\n", buf, coop_upnp_lan);
    }
}

void coop_upnp_start(int port)
{
    const char* off = getenv("ARCANUM_COOP_NO_UPNP");

    if (coop_upnp_thread != NULL || SDL_GetAtomicInt(&coop_upnp_st) != COOP_UPNP_OFF) {
        return;
    }
    if (off != NULL && off[0] != '\0' && off[0] != '0') {
        tig_debug_printf("CoopUPnP: disabled (ARCANUM_COOP_NO_UPNP)\n");
        return;
    }
    coop_upnp_port = port;
    coop_upnp_ext[0] = '\0';
    coop_upnp_err[0] = '\0';
    SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_BUSY);
    coop_upnp_thread = SDL_CreateThread(coop_upnp_worker, "coop_upnp", NULL);
    if (coop_upnp_thread == NULL) {
        snprintf(coop_upnp_err, sizeof(coop_upnp_err), "thread");
        SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_FAILED);
        return;
    }
    if (!coop_upnp_atexit) {
        coop_upnp_atexit = true;
        atexit(coop_upnp_stop);
    }
    tig_debug_printf("CoopUPnP: mapping UDP %d in the background\n", port);
}

void coop_upnp_stop(void)
{
    char port_str[16];

    coop_upnp_join(); /* discovery is bounded (~1.5 s + HTTP timeouts) */
    if (SDL_GetAtomicInt(&coop_upnp_st) == COOP_UPNP_MAPPED) {
        int rc;

        snprintf(port_str, sizeof(port_str), "%d", coop_upnp_port);
        rc = UPNP_DeletePortMapping(coop_upnp_urls.controlURL, coop_upnp_data.first.servicetype, port_str, "UDP",
            NULL);
        tig_debug_printf("CoopUPnP: removed UDP %d mapping (%d)\n", coop_upnp_port, rc);
    }
    if (coop_upnp_have_urls) {
        FreeUPNPUrls(&coop_upnp_urls);
        coop_upnp_have_urls = false;
    }
    SDL_SetAtomicInt(&coop_upnp_st, COOP_UPNP_OFF);
}

CoopUpnpState coop_upnp_state(void)
{
    CoopUpnpState st = (CoopUpnpState)SDL_GetAtomicInt(&coop_upnp_st);

    if (st != COOP_UPNP_BUSY && coop_upnp_thread != NULL) {
        coop_upnp_join(); /* finished: reap it and log the result once */
    }
    return st;
}

void coop_upnp_describe(char* buf, size_t size)
{
    CoopUpnpState st = (CoopUpnpState)SDL_GetAtomicInt(&coop_upnp_st);

    coop_upnp_format(st, coop_upnp_port, coop_upnp_ext, coop_upnp_err, buf, size);
}
#else
void coop_upnp_start(int port) { (void)port; }
void coop_upnp_stop(void) { }
CoopUpnpState coop_upnp_state(void) { return COOP_UPNP_OFF; }
void coop_upnp_describe(char* buf, size_t size) { coop_upnp_format(COOP_UPNP_OFF, 0, NULL, NULL, buf, size); }
#endif

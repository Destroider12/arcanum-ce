/*
 * PT58: Arcanum Coop Lite launcher (Win32, no runtime deps).
 *
 * Picks name / Host or Join / transport (Direct+UPnP or Steam) / address,
 * then starts arcanum-ce.exe with the usual command line
 *   -window -coop -host <port> | -join <ip:port | SteamID64> -log <file>
 * and the ARCANUM_COOP_* environment the game reads:
 *   ARCANUM_COOP_NAME, ARCANUM_COOP_TRANSPORT (direct | steam),
 *   ARCANUM_COOP_STEAM_HOST (client, Steam).
 * The Coop-Setup layout (coop-host\ / coop-client\ instances) is used when
 * present, else arcanum-ce.exe next to the launcher. Joining a host that is
 * already playing is the game's normal late-join. Settings persist in
 * arcanum-coop-launcher.ini next to the launcher.
 *
 * PT62: the launcher stays open and watches the game process. A non-zero
 * exit or a new crash-*.txt shows "Game closed unexpectedly" with
 * Reconnect / Create bug report / Close. A crashed host is restarted at
 * once with ARCANUM_COOP_AUTOLOAD=latest (the game loads its newest save and
 * skips the menu); clients wait for it in their main menu and rejoin.
 *
 * PT68: one game folder. The game keeps a coop role's saves, settings, log
 * and crash files in coop\host\ / coop\client\ of that folder, so host and
 * client can even run at once from it. The old Coop-Setup instances are used
 * only when the root arcanum-ce.exe is missing; if they exist, their saves,
 * Players and arcanum.cfg are copied once (never moved or deleted) into the
 * new role folders.
 *
 * PT65: on separate PCs one folder is enough (Coop-Setup is only for host +
 * client on one PC). Optional update check ([update] enabled=1 in the ini,
 * default off): replaces only the root arcanum-ce.exe from a verified
 * GitHub release zip.
 *
 * PT77: while the game runs, a lobby pane lists connected players and mirrors
 * in-game chat (game log [CHAT] lines + lobby_players.txt / lobby_chat_in.txt
 * in the role folder).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wininet.h>
#include <bcrypt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LAUNCHER_UPNP
#include <miniupnpc.h>
#include <upnpcommands.h>
#endif

#define IDC_NAME 101
#define IDC_HOST 102
#define IDC_JOIN 103
#define IDC_TRANSPORT 104
#define IDC_ADDR 105
#define IDC_STEAMID 106
#define IDC_LOCALIP 107
#define IDC_PUBIP 108
#define IDC_COPY 109
#define IDC_WINDOWED 110
#define IDC_START 111
#define IDC_STATUS 112
#define IDC_ADDR_LABEL 113
#define IDC_STEAM_LABEL 114
#define WM_APP_PUBIP (WM_APP + 1)
#define IDC_CHECK 115
#define IDC_FIREWALL 116
#define IDC_CHECK_OUT 117
#define IDC_RECONNECT 118
#define IDC_BUGREPORT 119
#define IDC_PANEL_CLOSE 120
#define IDC_UPD_TEXT 121 /* PT65 */
#define IDC_UPD_CHECK 122
#define IDC_UPD_GO 123
#define IDC_PLAYERS 124 /* PT77 */
#define IDC_LOBBY_CHAT 125
#define IDC_LOBBY_INPUT 126
#define IDC_LOBBY_SEND 127
#define IDC_LOBBY_LABEL 128
#define WM_APP_GAMEEXIT (WM_APP + 3)
#define IDT_LOBBY 1

#ifndef COOP_BUILD_SHA
#define COOP_BUILD_SHA "unknown"
#endif
#ifndef COOP_PROTOCOL
#define COOP_PROTOCOL 0
#endif

static HWND g_wnd;
static HFONT g_font;
static char g_dir[MAX_PATH]; /* launcher folder, trailing backslash */
static char g_ini[MAX_PATH];
static char g_local_ip[64];
static char g_public_ip[64];

/* PT62: the running game. */
static HANDLE g_game;
static bool g_game_host;
static char g_game_dir_now[MAX_PATH];
static bool g_game_legacy; /* PT70 */
static DWORD g_game_start_tick; /* PT70 */
static char g_game_data_dir[MAX_PATH]; /* PT68: where logs / crash files of the running role go */
static char g_game_log[64];
static FILETIME g_game_started;
static DWORD g_game_exit;
/* PT76a: a crashed host's exit code + log survive its auto-restart. */
static DWORD g_prev_crash_exit;
static bool g_prev_crash_valid;
static DWORD g_restart_ticks[3]; /* host auto-restarts, crash-loop guard */
static bool g_autoload_next;
static long g_lobby_log_off; /* PT77: byte offset into the game log */
static bool g_lobby_on;

static HWND item(int id)
{
    return GetDlgItem(g_wnd, id);
}

static bool is_host(void)
{
    return SendMessageA(item(IDC_HOST), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static bool is_steam(void)
{
    return SendMessageA(item(IDC_TRANSPORT), CB_GETCURSEL, 0, 0) == 1;
}

static void set_status(const char* text)
{
    SetWindowTextA(item(IDC_STATUS), text);
}

static int port_of(const char* addr)
{
    const char* colon = strrchr(addr, ':');
    int port = colon != NULL ? atoi(colon + 1) : atoi(addr);

    if (strchr(addr, '.') != NULL && colon == NULL) {
        port = 0; /* bare IP */
    }
    return port > 0 && port < 65536 ? port : 7777;
}

static void refresh(void)
{
    bool host = is_host();
    bool steam = is_steam();
    char ip[160];
    char addr[128];

    SetWindowTextA(item(IDC_ADDR_LABEL), host ? "Port (UDP):" : "Host IP:port:");
    EnableWindow(item(IDC_ADDR), !steam);
    EnableWindow(item(IDC_STEAMID), steam && !host);
    SetWindowTextA(item(IDC_STEAM_LABEL), steam && host ? "Steam: your SteamID64 shows in the game's main menu."
                                                        : "Host SteamID64:");
    GetWindowTextA(item(IDC_ADDR), addr, sizeof(addr));
    snprintf(ip, sizeof(ip), "Local IP: %s", g_local_ip[0] ? g_local_ip : "?");
    SetWindowTextA(item(IDC_LOCALIP), ip);
    snprintf(ip, sizeof(ip), "Public IP: %s", g_public_ip[0] ? g_public_ip : "(UPnP: none)");
    SetWindowTextA(item(IDC_PUBIP), ip);
    EnableWindow(item(IDC_COPY), host && !steam);
}

static void find_local_ip(void)
{
    char name[256];
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    struct addrinfo* it;

    if (gethostname(name, sizeof(name)) != 0) {
        return;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) != 0) {
        return;
    }
    for (it = res; it != NULL; it = it->ai_next) {
        struct sockaddr_in* sin = (struct sockaddr_in*)it->ai_addr;
        unsigned char* b = (unsigned char*)&sin->sin_addr;

        if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) {
            continue;
        }
        snprintf(g_local_ip, sizeof(g_local_ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        /* prefer a private LAN address */
        if (b[0] == 192 || b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] < 32)) {
            break;
        }
    }
    freeaddrinfo(res);
}

static DWORD WINAPI upnp_thread(LPVOID arg)
{
    static char ext[64];

    (void)arg;
    ext[0] = '\0';
#ifdef LAUNCHER_UPNP
    {
        int error = 0;
        struct UPNPDev* devs = upnpDiscover(1500, NULL, NULL, 0, 0, 2, &error);

        if (devs != NULL) {
            struct UPNPUrls urls;
            struct IGDdatas data;
            char lan[64];
            char wan[64];

            memset(&urls, 0, sizeof(urls));
            memset(&data, 0, sizeof(data));
            if (UPNP_GetValidIGD(devs, &urls, &data, lan, sizeof(lan), wan, sizeof(wan)) == 1) {
                if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext) != UPNPCOMMAND_SUCCESS) {
                    ext[0] = '\0';
                }
            }
            if (urls.controlURL != NULL) {
                FreeUPNPUrls(&urls);
            }
            freeUPNPDevlist(devs);
        }
    }
#endif
    PostMessageA(g_wnd, WM_APP_PUBIP, 0, (LPARAM)ext);
    return 0;
}

static void copy_address(void)
{
    char addr[128];
    char text[160];
    HGLOBAL mem;

    GetWindowTextA(item(IDC_ADDR), addr, sizeof(addr));
    snprintf(text, sizeof(text), "%s:%d", g_public_ip[0] ? g_public_ip : g_local_ip, port_of(addr));
    if (!OpenClipboard(g_wnd)) {
        return;
    }
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, strlen(text) + 1);
    if (mem != NULL) {
        memcpy(GlobalLock(mem), text, strlen(text) + 1);
        GlobalUnlock(mem);
        SetClipboardData(CF_TEXT, mem);
    }
    CloseClipboard();
    {
        char msg[220];

        snprintf(msg, sizeof(msg), "Copied %s - send it to your friends.", text);
        set_status(msg);
    }
}

static void load_settings(void)
{
    char buf[128];

    GetPrivateProfileStringA("coop", "name", "Player", buf, sizeof(buf), g_ini);
    SetWindowTextA(item(IDC_NAME), buf);
    GetPrivateProfileStringA("coop", "mode", "host", buf, sizeof(buf), g_ini);
    SendMessageA(item(_stricmp(buf, "join") == 0 ? IDC_JOIN : IDC_HOST), BM_SETCHECK, BST_CHECKED, 0);
    GetPrivateProfileStringA("coop", "transport", "direct", buf, sizeof(buf), g_ini);
    SendMessageA(item(IDC_TRANSPORT), CB_SETCURSEL, _stricmp(buf, "steam") == 0 ? 1 : 0, 0);
    GetPrivateProfileStringA("coop", "address", "7777", buf, sizeof(buf), g_ini);
    SetWindowTextA(item(IDC_ADDR), buf);
    GetPrivateProfileStringA("coop", "steam_host", "", buf, sizeof(buf), g_ini);
    SetWindowTextA(item(IDC_STEAMID), buf);
    SendMessageA(item(IDC_WINDOWED), BM_SETCHECK,
        GetPrivateProfileIntA("coop", "windowed", 1, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void save_settings(void)
{
    char buf[128];

    GetWindowTextA(item(IDC_NAME), buf, sizeof(buf));
    WritePrivateProfileStringA("coop", "name", buf, g_ini);
    WritePrivateProfileStringA("coop", "mode", is_host() ? "host" : "join", g_ini);
    WritePrivateProfileStringA("coop", "transport", is_steam() ? "steam" : "direct", g_ini);
    GetWindowTextA(item(IDC_ADDR), buf, sizeof(buf));
    WritePrivateProfileStringA("coop", "address", buf, g_ini);
    GetWindowTextA(item(IDC_STEAMID), buf, sizeof(buf));
    WritePrivateProfileStringA("coop", "steam_host", buf, g_ini);
    WritePrivateProfileStringA("coop", "windowed",
        SendMessageA(item(IDC_WINDOWED), BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0", g_ini);
}

static bool file_exists(const char* path)
{
    DWORD a = GetFileAttributesA(path);

    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* PT68: copy every plain file of src_dir into dst_dir, never overwriting. */
static int copy_files_once(const char* src_dir, const char* dst_dir)
{
    char pat[MAX_PATH];
    char src[MAX_PATH];
    char dst[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int copied = 0;

    snprintf(pat, sizeof(pat), "%s*", src_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    CreateDirectoryA(dst_dir, NULL);
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue; /* Save\Current is scratch; slots are plain files */
        }
        snprintf(src, sizeof(src), "%s%s", src_dir, fd.cFileName);
        snprintf(dst, sizeof(dst), "%s%s", dst_dir, fd.cFileName);
        if (CopyFileA(src, dst, TRUE)) {
            copied++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return copied;
}

/* PT68: one-time copy of a Coop-Setup instance (coop-host\ / coop-client\)
   into coop\host\ / coop\client\. A marker file makes it run once; the
   old folders are left untouched. */
static void migrate_legacy(bool host, const char* base)
{
    char legacy[MAX_PATH];
    char role[MAX_PATH];
    char src[MAX_PATH];
    char dst[MAX_PATH];
    char marker[MAX_PATH];
    char msg[200];
    int n = 0;
    FILE* f;

    snprintf(legacy, sizeof(legacy), "%s%s\\", g_dir, host ? "coop-host" : "coop-client");
    if (GetFileAttributesA(legacy) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    snprintf(dst, sizeof(dst), "%scoop\\", base);
    CreateDirectoryA(dst, NULL);
    snprintf(role, sizeof(role), "%scoop\\%s\\", base, host ? "host" : "client");
    CreateDirectoryA(role, NULL);
    snprintf(marker, sizeof(marker), "%smigrated-from-%s.txt", role, host ? "coop-host" : "coop-client");
    if (file_exists(marker)) {
        return;
    }
    snprintf(src, sizeof(src), "%smodules\\Arcanum\\Save\\", legacy);
    snprintf(dst, sizeof(dst), "%sSave\\", role);
    n += copy_files_once(src, dst);
    snprintf(src, sizeof(src), "%smodules\\Arcanum\\Players\\", legacy);
    snprintf(dst, sizeof(dst), "%sPlayers\\", role);
    n += copy_files_once(src, dst);
    snprintf(src, sizeof(src), "%sdata\\Players\\", legacy);
    n += copy_files_once(src, dst);
    snprintf(src, sizeof(src), "%sarcanum.cfg", legacy);
    snprintf(dst, sizeof(dst), "%sarcanum.cfg", role);
    if (file_exists(src) && CopyFileA(src, dst, TRUE)) {
        n++;
    }
    f = fopen(marker, "w");
    if (f != NULL) {
        fprintf(f, "Copied %d file(s) from %s (saves, Players, arcanum.cfg). The old folder was not changed.\n", n,
            legacy);
        fclose(f);
    }
    snprintf(msg, sizeof(msg), "Copied %d file(s) from %s into coop\\%s\\ (old folder kept).", n,
        host ? "coop-host" : "coop-client", host ? "host" : "client");
    set_status(msg);
}

/* Game folder (PT68): the launcher's own folder when arcanum-ce.exe is there;
   the old coop-host\ / coop-client\ instance only when it is not. */
static bool has_game_data(const char* dir)
{
    char p[MAX_PATH];

    snprintf(p, sizeof(p), "%stig.dat", dir);
    if (!file_exists(p)) {
        return false;
    }
    snprintf(p, sizeof(p), "%sarcanum1.dat", dir);
    return file_exists(p);
}

static LONGLONG file_size(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        return -1;
    }
    return ((LONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

/* PT70: where to run the game. exe_out = the exe to start, out = its working
   folder (must hold the Arcanum data: tig.dat, arcanum1.dat, modules\), and
   *legacy = an old Coop-Setup instance (coop-host\ / coop-client\).
   1) the launcher folder, if the data is there (normal one-folder install);
   2) an Arcanum\ subfolder or the parent folder with the data (the package
      unpacked next to / inside the game, as Coop-Setup -Source allowed);
   3) an old instance folder (its exe refreshed from the root build);
   Returns false when no game data was found anywhere. */
static bool game_dir(bool host, char* out, size_t size, char* exe_out, size_t exe_size, bool* legacy)
{
    char root_exe[MAX_PATH];
    char cand[MAX_PATH];
    char* slash;

    *legacy = false;
    snprintf(root_exe, sizeof(root_exe), "%sarcanum-ce.exe", g_dir);
    if (file_exists(root_exe)) {
        snprintf(exe_out, exe_size, "%s", root_exe);
        if (has_game_data(g_dir)) {
            snprintf(out, size, "%s", g_dir);
            return true;
        }
        snprintf(cand, sizeof(cand), "%sArcanum\\", g_dir);
        if (has_game_data(cand)) {
            snprintf(out, size, "%s", cand);
            return true;
        }
        snprintf(cand, sizeof(cand), "%s", g_dir);
        cand[strlen(cand) - 1] = '\0';
        slash = strrchr(cand, '\\');
        if (slash != NULL) {
            slash[1] = '\0';
            if (has_game_data(cand)) {
                snprintf(out, size, "%s", cand);
                return true;
            }
        }
    }
    snprintf(cand, sizeof(cand), "%s%s\\", g_dir, host ? "coop-host" : "coop-client");
    if (has_game_data(cand)) {
        char inst_exe[MAX_PATH];

        snprintf(inst_exe, sizeof(inst_exe), "%sarcanum-ce.exe", cand);
        if (file_exists(root_exe) && file_size(root_exe) != file_size(inst_exe)) {
            CopyFileA(root_exe, inst_exe, FALSE); /* like the old Start-*.bat */
        }
        if (file_exists(inst_exe)) {
            snprintf(out, size, "%s", cand);
            snprintf(exe_out, exe_size, "%s", inst_exe);
            *legacy = true;
            return true;
        }
    }
    snprintf(out, size, "%s", g_dir);
    snprintf(exe_out, exe_size, "%s", root_exe);
    return false;
}

static DWORD WINAPI game_watch_thread(LPVOID arg)
{
    HANDLE proc = (HANDLE)arg;
    DWORD code = 0;

    WaitForSingleObject(proc, INFINITE);
    GetExitCodeProcess(proc, &code);
    PostMessageA(g_wnd, WM_APP_GAMEEXIT, (WPARAM)code, (LPARAM)proc);
    return 0;
}


/* ---- PT77 lobby (player list + chat mirror) ---- */

static void lobby_path(char* out, size_t size, const char* name)
{
    snprintf(out, size, "%s%s", g_game_data_dir[0] != '\0' ? g_game_data_dir : g_dir, name);
}

static void lobby_append_chat(const char* line)
{
    HWND h = item(IDC_LOBBY_CHAT);
    int len;

    if (h == NULL || line == NULL || line[0] == '\0') {
        return;
    }
    len = GetWindowTextLengthA(h);
    SendMessageA(h, EM_SETSEL, len, len);
    SendMessageA(h, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageA(h, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
    SendMessageA(h, EM_SCROLLCARET, 0, 0);
}

static void lobby_send_chat(void)
{
    char text[256];
    char path[MAX_PATH];
    FILE* f;
    char* p;

    if (g_game == NULL) {
        set_status("Start the game first to use lobby chat.");
        return;
    }
    GetWindowTextA(item(IDC_LOBBY_INPUT), text, sizeof(text));
    for (p = text; *p == ' ' || *p == '\t'; p++) {
    }
    memmove(text, p, strlen(p) + 1);
    for (p = text + strlen(text); p > text && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n'); p--) {
        p[-1] = '\0';
    }
    if (text[0] == '\0') {
        return;
    }
    if (g_game_data_dir[0] != '\0') {
        CreateDirectoryA(g_game_data_dir, NULL);
    }
    lobby_path(path, sizeof(path), "lobby_chat_in.txt");
    f = fopen(path, "ab");
    if (f == NULL) {
        set_status("Could not write lobby_chat_in.txt (is the game role folder ready?).");
        return;
    }
    fprintf(f, "CHAT\t%s\n", text);
    fclose(f);
    SetWindowTextA(item(IDC_LOBBY_INPUT), "");
}

static void lobby_poll(void)
{
    char path[MAX_PATH];
    char buf[4096];
    FILE* f;
    long size;
    size_t n;
    char* line;
    char* nl;
    char players[1024];
    size_t poff = 0;

    if (g_game == NULL || !g_lobby_on) {
        return;
    }

    /* Player list file from the game. */
    lobby_path(path, sizeof(path), "lobby_players.txt");
    f = fopen(path, "rb");
    players[0] = '\0';
    if (f != NULL) {
        while (fgets(buf, sizeof(buf), f) != NULL) {
            int slot = -1;
            char name[64];
            char role[16];
            int ok = 0;
            char* tab1;
            char* tab2;
            char* tab3;
            char row[96];

            buf[strcspn(buf, "\r\n")] = '\0';
            tab1 = strchr(buf, '\t');
            if (tab1 == NULL) {
                continue;
            }
            *tab1 = '\0';
            slot = atoi(buf);
            tab2 = strchr(tab1 + 1, '\t');
            if (tab2 == NULL) {
                continue;
            }
            *tab2 = '\0';
            snprintf(name, sizeof(name), "%s", tab1 + 1);
            tab3 = strchr(tab2 + 1, '\t');
            if (tab3 != NULL) {
                *tab3 = '\0';
            }
            snprintf(role, sizeof(role), "%s", tab2 + 1);
            (void)ok;
            snprintf(row, sizeof(row), "%s%s (%s)", poff > 0 ? "\r\n" : "", name, role);
            if (poff + strlen(row) + 1 < sizeof(players)) {
                memcpy(players + poff, row, strlen(row) + 1);
                poff += strlen(row);
            }
            (void)slot;
        }
        fclose(f);
    }
    if (players[0] == '\0') {
        snprintf(players, sizeof(players), "(waiting for game...)");
    }
    if (GetWindowTextLengthA(item(IDC_PLAYERS)) != (int)strlen(players)
        || GetWindowTextA(item(IDC_PLAYERS), buf, sizeof(buf)) <= 0
        || strcmp(buf, players) != 0) {
        SetWindowTextA(item(IDC_PLAYERS), players);
    }

    /* Tail the game log for [CHAT] lines. */
    lobby_path(path, sizeof(path), g_game_log);
    f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size < g_lobby_log_off) {
        g_lobby_log_off = 0; /* log rotated / truncated */
    }
    if (size > g_lobby_log_off) {
        fseek(f, g_lobby_log_off, SEEK_SET);
        while (g_lobby_log_off < size) {
            long chunk = size - g_lobby_log_off;

            if (chunk > (long)sizeof(buf) - 1) {
                chunk = (long)sizeof(buf) - 1;
            }
            n = fread(buf, 1, (size_t)chunk, f);
            if (n == 0) {
                break;
            }
            buf[n] = '\0';
            g_lobby_log_off += (long)n;
            line = buf;
            while (line != NULL && *line != '\0') {
                nl = strchr(line, '\n');
                if (nl != NULL) {
                    *nl = '\0';
                    if (nl > line && nl[-1] == '\r') {
                        nl[-1] = '\0';
                    }
                }
                {
                    char* chat = strstr(line, "[CHAT] ");
                    if (chat != NULL) {
                        lobby_append_chat(chat + 7);
                    }
                }
                line = nl != NULL ? nl + 1 : NULL;
            }
        }
    }
    fclose(f);
}

static void show_lobby(bool on)
{
    int show = on ? SW_SHOW : SW_HIDE;
    RECT r;
    HWND hwnd = g_wnd;

    g_lobby_on = on;
    ShowWindow(item(IDC_LOBBY_LABEL), show);
    ShowWindow(item(IDC_PLAYERS), show);
    ShowWindow(item(IDC_LOBBY_CHAT), show);
    ShowWindow(item(IDC_LOBBY_INPUT), show);
    ShowWindow(item(IDC_LOBBY_SEND), show);
    if (on) {
        g_lobby_log_off = 0;
        SetWindowTextA(item(IDC_LOBBY_CHAT), "");
        SetWindowTextA(item(IDC_PLAYERS), "(waiting for game...)");
        SetTimer(g_wnd, IDT_LOBBY, 500, NULL);
        SetWindowLongA(item(IDC_START), GWL_STYLE, GetWindowLongA(item(IDC_START), GWL_STYLE) & ~BS_DEFPUSHBUTTON);
        SetWindowLongA(item(IDC_LOBBY_SEND), GWL_STYLE, GetWindowLongA(item(IDC_LOBBY_SEND), GWL_STYLE) | BS_DEFPUSHBUTTON);
        SetWindowPos(hwnd, NULL, 0, 0, 460, 780, SWP_NOMOVE | SWP_NOZORDER);
        lobby_poll();
    } else {
        KillTimer(g_wnd, IDT_LOBBY);
        SetWindowLongA(item(IDC_LOBBY_SEND), GWL_STYLE, GetWindowLongA(item(IDC_LOBBY_SEND), GWL_STYLE) & ~BS_DEFPUSHBUTTON);
        SetWindowLongA(item(IDC_START), GWL_STYLE, GetWindowLongA(item(IDC_START), GWL_STYLE) | BS_DEFPUSHBUTTON);
        SetWindowPos(hwnd, NULL, 0, 0, 460, 573, SWP_NOMOVE | SWP_NOZORDER);
    }
    (void)r;
}

static void show_panel(bool on)
{
    ShowWindow(item(IDC_RECONNECT), on ? SW_SHOW : SW_HIDE);
    ShowWindow(item(IDC_BUGREPORT), on ? SW_SHOW : SW_HIDE);
    ShowWindow(item(IDC_PANEL_CLOSE), on ? SW_SHOW : SW_HIDE);
}

static void start_game(void)
{
    char name[64];
    char addr[128];
    char steam_id[64];
    char dir[MAX_PATH];
    char exe[MAX_PATH];
    char cmd[1024];
    bool host = is_host();
    bool steam = is_steam();
    bool windowed = SendMessageA(item(IDC_WINDOWED), BM_GETCHECK, 0, 0) == BST_CHECKED;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char* p;
    bool legacy;

    GetWindowTextA(item(IDC_NAME), name, sizeof(name));
    GetWindowTextA(item(IDC_ADDR), addr, sizeof(addr));
    GetWindowTextA(item(IDC_STEAMID), steam_id, sizeof(steam_id));
    for (p = name; *p; p++) {
        if (*p == '"') {
            *p = '\'';
        }
    }
    for (p = addr; *p == ' '; p++) {
    }
    memmove(addr, p, strlen(p) + 1);
    if (name[0] == '\0') {
        set_status("Enter a player name.");
        return;
    }
    if (!host && steam && strlen(steam_id) < 15) {
        set_status("Enter the host's SteamID64 (17 digits, shown in the host's main menu).");
        return;
    }
    if (!host && !steam && addr[0] == '\0') {
        set_status("Enter the host's IP:port.");
        return;
    }
    save_settings();
    if (!game_dir(host, dir, sizeof(dir), exe, sizeof(exe), &legacy)) {
        set_status(file_exists(exe)
                ? "Arcanum data (tig.dat, arcanum1.dat) not found: put the launcher and arcanum-ce.exe into the Arcanum folder."
                : "arcanum-ce.exe not found next to the launcher.");
        return;
    }
    {
        /* PT68: the root folder: the game writes its log to coop\<role>\ by
           itself; an old instance folder keeps its log in its own root. */
        const char* log_arg = legacy ? (host ? " -log coop_host.log" : " -log coop_client.log") : "";

        if (!legacy) {
            migrate_legacy(host, dir);
            snprintf(g_game_data_dir, sizeof(g_game_data_dir), "%scoop\\%s\\", dir, host ? "host" : "client");
        } else {
            snprintf(g_game_data_dir, sizeof(g_game_data_dir), "%s", dir);
        }
        if (host) {
            snprintf(cmd, sizeof(cmd), "\"%s\"%s -coop -host %d%s", exe, windowed ? " -window" : "", port_of(addr),
                log_arg);
        } else if (steam) {
            snprintf(cmd, sizeof(cmd), "\"%s\"%s -coop -join %s%s", exe, windowed ? " -window" : "", steam_id,
                log_arg);
        } else {
            if (strchr(addr, ':') == NULL) {
                strncat(addr, ":7777", sizeof(addr) - strlen(addr) - 1);
            }
            snprintf(cmd, sizeof(cmd), "\"%s\"%s -coop -join %s%s", exe, windowed ? " -window" : "", addr, log_arg);
        }
        SetEnvironmentVariableA("ARCANUM_LOG_FILE", NULL); /* the role log path above wins */
    }
    SetEnvironmentVariableA("ARCANUM_COOP_NAME", name);
    SetEnvironmentVariableA("ARCANUM_COOP_TRANSPORT", steam ? "steam" : "direct");
    SetEnvironmentVariableA("ARCANUM_COOP_STEAM_HOST", !host && steam ? steam_id : NULL);
    SetEnvironmentVariableA("ARCANUM_COOP_AUTOLOAD", host && g_autoload_next ? "latest" : NULL);
    /* PT63: a client's Reconnect skips the main menu and rejoins (parked PC). */
    SetEnvironmentVariableA("ARCANUM_COOP_AUTOJOIN", !host && g_autoload_next ? "1" : NULL);
    g_autoload_next = false;
    if (steam) {
        char dll[MAX_PATH];
        char appid[MAX_PATH];

        snprintf(dll, sizeof(dll), "%ssteam_api64.dll", dir);
        if (!file_exists(dll)) {
            set_status("Steam: steam_api64.dll is missing in the game folder - see docs (PT58).");
            return;
        }
        snprintf(appid, sizeof(appid), "%ssteam_appid.txt", dir);
        if (!file_exists(appid)) {
            FILE* f = fopen(appid, "w");

            if (f != NULL) {
                fputs("500810\n", f);
                fclose(f);
            }
        }
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        char msg[160];

        snprintf(msg, sizeof(msg), "Could not start the game (error %lu).", GetLastError());
        set_status(msg);
        return;
    }
    CloseHandle(pi.hThread);
    g_game = pi.hProcess;
    g_game_host = host;
    snprintf(g_game_dir_now, sizeof(g_game_dir_now), "%s", dir);
    g_game_legacy = legacy;
    g_game_start_tick = GetTickCount();
    snprintf(g_game_log, sizeof(g_game_log), "%s", host ? "coop_host.log" : "coop_client.log");
    GetSystemTimeAsFileTime(&g_game_started);
    show_panel(false);
    EnableWindow(item(IDC_START), FALSE);
    EnableWindow(item(IDC_UPD_CHECK), FALSE); /* PT65: no update while the game runs */
    EnableWindow(item(IDC_UPD_GO), FALSE);
    CloseHandle(CreateThread(NULL, 0, game_watch_thread, pi.hProcess, 0, NULL));
    show_lobby(true);
    set_status(host ? "Game started as host. Friends can join any time (late join)." : "Game started - joining the host.");
}

/* ---- PT62: game exit / crash handling and the bug report ---- */

/* crash-*.txt written in the game folder since the game started. */
static bool new_crash_file(void)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    bool found = false;

    snprintf(pat, sizeof(pat), "%scrash-*.txt", g_game_data_dir[0] != '\0' ? g_game_data_dir : g_game_dir_now);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &g_game_started) >= 0) {
            found = true;
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static unsigned int crc_table[256];

static unsigned int crc32_update(unsigned int crc, const unsigned char* p, size_t n)
{
    size_t i;

    if (crc_table[1] == 0) {
        unsigned int k, c;
        int j;

        for (k = 0; k < 256; k++) {
            c = k;
            for (j = 0; j < 8; j++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            crc_table[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* Minimal "stored" (uncompressed) zip writer: no deps, Explorer opens it. */
#define ZIP_MAX 64
typedef struct ZipEntry {
    char name[96];
    unsigned int crc, size, offset;
} ZipEntry;

typedef struct Zip {
    FILE* f;
    ZipEntry e[ZIP_MAX];
    int n;
} Zip;

static void put16(FILE* f, unsigned int v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static void put32(FILE* f, unsigned int v)
{
    put16(f, v & 0xFFFF);
    put16(f, v >> 16);
}

static void zip_header(FILE* f, const ZipEntry* e, bool central)
{
    put32(f, central ? 0x02014b50u : 0x04034b50u);
    if (central) {
        put16(f, 20);
    }
    put16(f, 20); /* version needed */
    put16(f, 0); /* flags */
    put16(f, 0); /* stored */
    put16(f, 0); /* time */
    put16(f, 0x21); /* date 1980-01-01 */
    put32(f, e->crc);
    put32(f, e->size);
    put32(f, e->size);
    put16(f, (unsigned int)strlen(e->name));
    put16(f, 0); /* extra */
    if (central) {
        put16(f, 0); /* comment */
        put16(f, 0); /* disk */
        put16(f, 0); /* internal attr */
        put32(f, 0); /* external attr */
        put32(f, e->offset);
    }
    fwrite(e->name, 1, strlen(e->name), f);
}

static bool zip_add_mem(Zip* z, const char* name, const unsigned char* data, size_t size)
{
    ZipEntry* e;

    if (z->n >= ZIP_MAX || size > 0x7FFFFFFFu) {
        return false;
    }
    e = &z->e[z->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->crc = crc32_update(0, data, size);
    e->size = (unsigned int)size;
    e->offset = (unsigned int)ftell(z->f);
    zip_header(z->f, e, false);
    fwrite(data, 1, size, z->f);
    return true;
}

static bool zip_add_file(Zip* z, const char* path, const char* name)
{
    HANDLE h;
    LARGE_INTEGER sz;
    unsigned char* buf;
    DWORD got = 0;
    bool ok;

    /* Shared read: the log may still be open. */
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 512LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    buf = (unsigned char*)malloc(sz.QuadPart > 0 ? (size_t)sz.QuadPart : 1);
    if (buf == NULL) {
        CloseHandle(h);
        return false;
    }
    ok = ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) && zip_add_mem(z, name, buf, got);
    free(buf);
    CloseHandle(h);
    return ok;
}

static void zip_finish(Zip* z)
{
    unsigned int start = (unsigned int)ftell(z->f);
    unsigned int end;
    int i;

    for (i = 0; i < z->n; i++) {
        zip_header(z->f, &z->e[i], true);
    }
    end = (unsigned int)ftell(z->f);
    put32(z->f, 0x06054b50u);
    put16(z->f, 0);
    put16(z->f, 0);
    put16(z->f, (unsigned int)z->n);
    put16(z->f, (unsigned int)z->n);
    put32(z->f, end - start);
    put32(z->f, start);
    put16(z->f, 0);
    fclose(z->f);
}

/* The newest `count` files matching the pattern (crash-*.txt / .dmp). */
static void zip_add_newest(Zip* z, const char* dir, const char* pattern, int count)
{
    char pat[MAX_PATH];
    char path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    struct { char name[MAX_PATH]; FILETIME t; } best[4];
    int nbest = 0;
    int i, j;
    HANDLE h;

    snprintf(pat, sizeof(pat), "%s%s", dir, pattern);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    if (count > 4) {
        count = 4;
    }
    do {
        for (i = 0; i < nbest && CompareFileTime(&fd.ftLastWriteTime, &best[i].t) <= 0; i++) {
        }
        if (i >= count) {
            continue;
        }
        for (j = (nbest < count ? nbest : count - 1); j > i; j--) {
            best[j] = best[j - 1];
        }
        snprintf(best[i].name, sizeof(best[i].name), "%s", fd.cFileName);
        best[i].t = fd.ftLastWriteTime;
        if (nbest < count) {
            nbest++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (i = 0; i < nbest; i++) {
        snprintf(path, sizeof(path), "%s%s", dir, best[i].name);
        zip_add_file(z, path, best[i].name);
    }
}

static void create_bug_report(void)
{
    char zpath[MAX_PATH];
    char path[MAX_PATH];
    char info[1024];
    char params[MAX_PATH + 32];
    SYSTEMTIME t;
    Zip z;
    const char* dir = g_game_dir_now[0] != '\0' ? g_game_dir_now : g_dir;

    save_settings();
    GetLocalTime(&t);
    snprintf(zpath, sizeof(zpath), "%sbugreport-%04d%02d%02d-%02d%02d%02d.zip", g_dir, t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond);
    memset(&z, 0, sizeof(z));
    z.f = fopen(zpath, "wb");
    if (z.f == NULL) {
        set_status("Could not create the bug report zip next to the launcher.");
        return;
    }
    snprintf(info, sizeof(info),
        "Arcanum Coop bug report\r\nbuild: %s\r\nprotocol: %d\r\nrole: %s\r\nexit code: 0x%08lX (%lu)\r\n"
        "time: %04d-%02d-%02d %02d:%02d:%02d (local)\r\ngame folder: %s\r\n"
        "Note: the launcher ini is included as is (player name, host address / SteamID).\r\n",
        COOP_BUILD_SHA, COOP_PROTOCOL, g_game_host ? "host" : "client", g_game_exit, g_game_exit, t.wYear, t.wMonth,
        t.wDay, t.wHour, t.wMinute, t.wSecond, dir);
    if (g_prev_crash_valid) {
        size_t len = strlen(info);

        /* PT76a */
        snprintf(info + len, sizeof(info) - len,
            "previous host run crashed: exit code 0x%08lX (auto-restarted; its log is coop_host.crash.log)\r\n",
            g_prev_crash_exit);
    }
    zip_add_mem(&z, "info.txt", (const unsigned char*)info, strlen(info));
    if (!g_game_legacy) {
        /* PT68: one folder: each role's log and crash files in coop\<role>\ */
        char role_dir[MAX_PATH];

        snprintf(path, sizeof(path), "%scoop\\host\\coop_host.log", dir);
        zip_add_file(&z, path, "coop_host.log");
        if (g_prev_crash_valid) {
            snprintf(path, sizeof(path), "%scoop\\host\\coop_host.crash.log", dir);
            zip_add_file(&z, path, "coop_host.crash.log");
        }
        snprintf(path, sizeof(path), "%scoop\\client\\coop_client.log", dir);
        zip_add_file(&z, path, "coop_client.log");
        snprintf(role_dir, sizeof(role_dir), "%scoop\\%s\\", dir, g_game_host ? "host" : "client");
        zip_add_newest(&z, role_dir, "crash-*.txt", 2);
        zip_add_newest(&z, role_dir, "crash-*.dmp", 2);
    } else {
        snprintf(path, sizeof(path), "%scoop_host.log", dir);
        zip_add_file(&z, path, "coop_host.log");
        if (g_prev_crash_valid) {
            snprintf(path, sizeof(path), "%scoop_host.crash.log", dir);
            zip_add_file(&z, path, "coop_host.crash.log");
        }
        snprintf(path, sizeof(path), "%scoop_client.log", dir);
        zip_add_file(&z, path, "coop_client.log");
        zip_add_newest(&z, dir, "crash-*.txt", 2);
        zip_add_newest(&z, dir, "crash-*.dmp", 2);
    }
    zip_add_file(&z, g_ini, "arcanum-coop-launcher.ini");
    zip_finish(&z);
    snprintf(params, sizeof(params), "/select,\"%s\"", zpath);
    ShellExecuteA(g_wnd, "open", "explorer.exe", params, NULL, SW_SHOWNORMAL);
    snprintf(info, sizeof(info), "Bug report saved: %s", strrchr(zpath, '\\') + 1);
    set_status(info);
}

/* Host crash loop guard: at most 3 automatic restarts per 2 minutes. */
static bool host_restart_allowed(void)
{
    DWORD now = GetTickCount();

    if (g_restart_ticks[0] != 0 && now - g_restart_ticks[0] < 120000) {
        return false;
    }
    g_restart_ticks[0] = g_restart_ticks[1];
    g_restart_ticks[1] = g_restart_ticks[2];
    g_restart_ticks[2] = now != 0 ? now : 1;
    return true;
}

/* PT70: the game closed within seconds: show its exit code and the end of
   the role log (the game writes "Error initializing ..." there). */
static void show_early_exit(DWORD code)
{
    char log[MAX_PATH];
    char text[2600];
    char tail[2048];
    char status[200];
    FILE* f;
    long size;
    size_t n = 0;

    if (g_game_legacy) {
        snprintf(log, sizeof(log), "%s%s", g_game_dir_now, g_game_log);
    } else {
        snprintf(log, sizeof(log), "%s%s", g_game_data_dir, g_game_log);
    }
    tail[0] = '\0';
    f = fopen(log, "rb");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, size > (long)sizeof(tail) - 1 ? size - (long)(sizeof(tail) - 1) : 0, SEEK_SET);
        n = fread(tail, 1, sizeof(tail) - 1, f);
        tail[n] = '\0';
        fclose(f);
    }
    snprintf(text, sizeof(text),
        "The game closed right after the start (exit code %lu).\r\n\r\nGame folder: %s\r\nLog: %s\r\n\r\n%s",
        code, g_game_dir_now, log, n > 0 ? tail : "(the log is missing or empty)");
    snprintf(status, sizeof(status), "Game closed right after the start (exit code %lu) - see %s", code, log);
    set_status(status);
    MessageBoxA(g_wnd, text, "Arcanum Coop - game closed at start", MB_OK | MB_ICONWARNING);
}

static void on_game_exit(DWORD code, HANDLE proc)
{
    char msg[256];
    bool crashed;

    if (proc != g_game) {
        CloseHandle(proc);
        return;
    }
    CloseHandle(g_game);
    g_game = NULL;
    g_game_exit = code;
    show_lobby(false);
    EnableWindow(item(IDC_START), TRUE);
    EnableWindow(item(IDC_UPD_CHECK), TRUE);
    EnableWindow(item(IDC_UPD_GO), TRUE);
    crashed = code != 0 || new_crash_file();
    if (GetTickCount() - g_game_start_tick < 15000) {
        /* PT70: closed right after the start: show why, never auto-restart. */
        show_early_exit(code);
        show_panel(true);
        return;
    }
    if (!crashed) {
        set_status("Game closed.");
        return;
    }
    show_panel(true);
    if (g_game_host && host_restart_allowed()) {
        char log[MAX_PATH];
        char keep[MAX_PATH + 16];

        /* PT76a: the restarted host overwrites its log - keep the crashed
           run's log next to it (coop_host.crash.log) for the bug report. */
        snprintf(log, sizeof(log), "%s%s", g_game_legacy ? g_game_dir_now : g_game_data_dir, g_game_log);
        snprintf(keep, sizeof(keep), "%.*s.crash.log", (int)(strlen(log) - 4), log);
        CopyFileA(log, keep, FALSE);
        g_prev_crash_exit = code;
        g_prev_crash_valid = true;
        snprintf(msg, sizeof(msg),
            "Game closed unexpectedly (exit 0x%08lX). Restarting the host from the last save - friends rejoin by themselves.",
            code);
        set_status(msg);
        g_autoload_next = true;
        start_game();
        show_panel(true);
        set_status(msg);
        return;
    }
    snprintf(msg, sizeof(msg), "Game closed unexpectedly (exit 0x%08lX).%s", code,
        g_game_host ? " Several crashes in a row - not restarting." : "");
    set_status(msg);
}

/* ---- PT59: port check (launcher only) ----
 * No server of ours: local bind test, UPnP query / test mapping, router WAN
 * IP vs the public IP seen by a public STUN server (CGNAT), STUN mapped port
 * from the game port, and our firewall rule. A true inbound UDP test needs
 * an outside peer, so OPEN is only ever "likely". */
#define WM_APP_CHECK (WM_APP + 2)
#define FW_RULE_FMT "Arcanum Coop UDP %d"

typedef struct PortCheckJob {
    int port;
    bool probe_port; /* bind the game port for STUN + try a test mapping */
} PortCheckJob;

static volatile LONG g_check_running;

static bool ip_private(const char* ip)
{
    unsigned a, b, c, d;

    if (ip == NULL || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168)
        || (a == 100 && b >= 64 && b < 128) || (a == 169 && b == 254) || a == 0;
}

/* STUN binding request (RFC 5389) on sock; fills ip / port. */
static bool stun_query(SOCKET sock, char* ip, size_t ip_size, int* mapped_port)
{
    static const char* const servers[] = { "stun.l.google.com", "stun1.l.google.com", "stun.cloudflare.com" };
    static const char* const ports[] = { "19302", "19302", "3478" };
    size_t s;
    int attempt;

    for (s = 0; s < sizeof(servers) / sizeof(servers[0]); s++) {
        struct addrinfo hints;
        struct addrinfo* res = NULL;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(servers[s], ports[s], &hints, &res) != 0 || res == NULL) {
            continue;
        }
        for (attempt = 0; attempt < 2; attempt++) {
            unsigned char req[20];
            unsigned char resp[512];
            fd_set rd;
            struct timeval tv = { 0, 800000 };
            int n;
            int i;
            int off;

            memset(req, 0, sizeof(req));
            req[1] = 0x01; /* Binding Request */
            req[4] = 0x21;
            req[5] = 0x12;
            req[6] = 0xA4;
            req[7] = 0x42;
            for (i = 8; i < 20; i++) {
                req[i] = (unsigned char)(rand() & 0xFF);
            }
            sendto(sock, (const char*)req, sizeof(req), 0, res->ai_addr, (int)res->ai_addrlen);
            FD_ZERO(&rd);
            FD_SET(sock, &rd);
            if (select(0, &rd, NULL, NULL, &tv) <= 0) {
                continue;
            }
            n = recv(sock, (char*)resp, sizeof(resp), 0);
            if (n < 20 || resp[0] != 0x01 || resp[1] != 0x01 || memcmp(resp + 8, req + 8, 12) != 0) {
                continue;
            }
            for (off = 20; off + 4 <= n;) {
                int type = (resp[off] << 8) | resp[off + 1];
                int len = (resp[off + 2] << 8) | resp[off + 3];

                if ((type == 0x0020 || type == 0x0001) && len >= 8 && off + 4 + 8 <= n && resp[off + 5] == 0x01) {
                    unsigned char* v = resp + off + 4;
                    int p = (v[2] << 8) | v[3];
                    unsigned char a[4] = { v[4], v[5], v[6], v[7] };

                    if (type == 0x0020) {
                        p ^= 0x2112;
                        a[0] ^= 0x21;
                        a[1] ^= 0x12;
                        a[2] ^= 0xA4;
                        a[3] ^= 0x42;
                    }
                    snprintf(ip, ip_size, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
                    *mapped_port = p;
                    freeaddrinfo(res);
                    return true;
                }
                off += 4 + ((len + 3) & ~3);
            }
        }
        freeaddrinfo(res);
    }
    return false;
}

/* Our netsh rule exists? (by name: language-independent exit code) */
static int firewall_rule_state(int port)
{
    char cmd[256];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 2;

    snprintf(cmd, sizeof(cmd), "netsh.exe advfirewall firewall show rule name=\"" FW_RULE_FMT "\"", port);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return -1;
    }
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0 ? 1 : code == 1 ? 0 : -1;
}

static DWORD WINAPI check_thread(LPVOID arg)
{
    PortCheckJob job = *(PortCheckJob*)arg;
    char* out = (char*)malloc(4096);
    char lines[2048] = "";
    char verdict[1024] = "";
    char wan_ip[64] = "";
    char stun_ip[64] = "";
    char port_str[16];
    int stun_port = 0;
    bool stun_ok = false;
    bool bound = false;
    bool in_use = false;
    bool upnp_router = false;
    bool upnp_mapped = false;
    bool upnp_refused = false;
    int fw;
    bool cgnat = false;
    SOCKET sock;
    struct sockaddr_in sin;

    free(arg);
    snprintf(port_str, sizeof(port_str), "%d", job.port);

    /* 1. local bind */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((u_short)job.port);
    if (sock != INVALID_SOCKET && job.probe_port && bind(sock, (struct sockaddr*)&sin, sizeof(sin)) == 0) {
        bound = true;
        strcat(lines, "Local: UDP port free.\r\n");
    } else if (sock != INVALID_SOCKET && job.probe_port) {
        in_use = WSAGetLastError() == WSAEADDRINUSE;
        strcat(lines, in_use ? "Local: UDP port in use (the game is already hosting?).\r\n"
                             : "Local: cannot bind the UDP port.\r\n");
    }
    if (sock != INVALID_SOCKET && !bound) {
        closesocket(sock);
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); /* any port: public IP only */
        sin.sin_port = 0;
        if (sock != INVALID_SOCKET) {
            bind(sock, (struct sockaddr*)&sin, sizeof(sin));
        }
    }

    /* 4. STUN */
    if (sock != INVALID_SOCKET) {
        stun_ok = stun_query(sock, stun_ip, sizeof(stun_ip), &stun_port);
        closesocket(sock);
    }
    {
        char l[256];

        if (!stun_ok) {
            snprintf(l, sizeof(l), "STUN: no answer (offline, or UDP blocked outbound).\r\n");
        } else if (bound) {
            snprintf(l, sizeof(l), "STUN: public %s, port %d -> %d (%s).\r\n", stun_ip, job.port, stun_port,
                stun_port == job.port ? "preserved" : "rewritten by NAT");
        } else {
            snprintf(l, sizeof(l), "STUN: public IP %s.\r\n", stun_ip);
        }
        strcat(lines, l);
    }

    /* 2. UPnP */
#ifdef LAUNCHER_UPNP
    {
        int error = 0;
        struct UPNPDev* devs = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &error);

        if (devs != NULL) {
            struct UPNPUrls urls;
            struct IGDdatas data;
            char lan[64];
            char wan[64];
            int igd;

            memset(&urls, 0, sizeof(urls));
            memset(&data, 0, sizeof(data));
            igd = UPNP_GetValidIGD(devs, &urls, &data, lan, sizeof(lan), wan, sizeof(wan));
            if (igd == 1 || igd == 2) {
                char ic[40] = "", ip_[8] = "", desc[80] = "", en[8] = "", lease[16] = "";

                upnp_router = true;
                if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, wan_ip) != UPNPCOMMAND_SUCCESS) {
                    wan_ip[0] = '\0';
                }
                if (UPNP_GetSpecificPortMappingEntry(urls.controlURL, data.first.servicetype, port_str, "UDP", NULL,
                        ic, ip_, desc, en, lease)
                    == UPNPCOMMAND_SUCCESS) {
                    char l[200];

                    upnp_mapped = true;
                    snprintf(l, sizeof(l), "UPnP: UDP %d is mapped to %s (%s).\r\n", job.port, ic, desc);
                    strcat(lines, l);
                    if (strcmp(ic, lan) != 0) {
                        snprintf(l, sizeof(l), "UPnP: but that is not this PC (%s)!\r\n", lan);
                        strcat(lines, l);
                        upnp_mapped = false;
                    }
                } else if (job.probe_port && !in_use) {
                    /* test mapping, removed again: the game maps for itself */
                    int rc = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port_str, port_str, lan,
                        "Arcanum Coop (launcher test)", "UDP", NULL, "0");

                    if (rc == UPNPCOMMAND_SUCCESS) {
                        upnp_mapped = true;
                        UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port_str, "UDP", NULL);
                        strcat(lines, "UPnP: router accepts the mapping (the game maps it when hosting).\r\n");
                    } else {
                        char l[160];

                        upnp_refused = true;
                        snprintf(l, sizeof(l), "UPnP: router refused the mapping (%d).\r\n", rc);
                        strcat(lines, l);
                    }
                } else {
                    strcat(lines, "UPnP: router found, port not mapped yet.\r\n");
                }
                if (wan_ip[0] != '\0') {
                    char l[120];

                    snprintf(l, sizeof(l), "Router WAN IP: %s.\r\n", wan_ip);
                    strcat(lines, l);
                }
            } else {
                strcat(lines, "UPnP: no usable router (UPnP off, or a second router in front).\r\n");
            }
            if (urls.controlURL != NULL) {
                FreeUPNPUrls(&urls);
            }
            freeUPNPDevlist(devs);
        } else {
            strcat(lines, "UPnP: no router answered (UPnP disabled?).\r\n");
        }
    }
#else
    strcat(lines, "UPnP: not built in.\r\n");
#endif

    /* 3. CGNAT */
    if (wan_ip[0] != '\0' && (ip_private(wan_ip) || (stun_ok && strcmp(wan_ip, stun_ip) != 0))) {
        cgnat = true;
    }
    if (stun_ok && ip_private(stun_ip)) {
        cgnat = true;
    }

    /* 5. firewall */
    fw = firewall_rule_state(job.port);
    strcat(lines, fw == 1 ? "Firewall: our inbound rule exists.\r\n"
                 : fw == 0 ? "Firewall: no 'Arcanum Coop' rule (Windows may ask on first host, or block silently).\r\n"
                           : "Firewall: state unknown.\r\n");

    if (cgnat) {
        snprintf(verdict, sizeof(verdict),
            "CLOSED - your provider uses CGNAT (router WAN %s, public %s). Port forwarding cannot work here: use the Steam transport, or ask the provider for a public IP.",
            wan_ip[0] ? wan_ip : "?", stun_ok ? stun_ip : "?");
    } else if (upnp_mapped) {
        snprintf(verdict, sizeof(verdict),
            "OPEN (likely) - UPnP maps UDP %d to this PC, no CGNAT. Give friends %s:%d. (Only a friend joining proves inbound UDP.)%s",
            job.port, stun_ok ? stun_ip : (wan_ip[0] ? wan_ip : "your public IP"), job.port,
            fw == 0 ? " If they cannot connect, press 'Add firewall rule'." : "");
    } else if (upnp_router || upnp_refused || stun_ok) {
        snprintf(verdict, sizeof(verdict),
            "CLOSED (likely) - UDP %d is not mapped. Enable UPnP on the router, or forward UDP %d to %s by hand%s. %s",
            job.port, job.port, g_local_ip[0] ? g_local_ip : "this PC",
            fw == 1 ? "" : ", and allow it in the firewall ('Add firewall rule')",
            "If you already forwarded it manually, ignore this.");
    } else {
        snprintf(verdict, sizeof(verdict),
            "UNKNOWN - no UPnP router answered and no STUN server replied (offline or UDP blocked); cannot tell from here.");
    }
    snprintf(out, 4096, "%s\r\n\r\n%s", verdict, lines);
    PostMessageA(g_wnd, WM_APP_CHECK, 0, (LPARAM)out);
    return 0;
}

static void start_check(bool probe_port)
{
    char addr[128];
    PortCheckJob* job;

    if (InterlockedCompareExchange(&g_check_running, 1, 0) != 0) {
        return;
    }
    job = (PortCheckJob*)malloc(sizeof(*job));
    GetWindowTextA(item(IDC_ADDR), addr, sizeof(addr));
    job->port = port_of(addr);
    job->probe_port = probe_port;
    SetWindowTextA(item(IDC_CHECK_OUT), "Checking the port... (a few seconds)");
    CloseHandle(CreateThread(NULL, 0, check_thread, job, 0, NULL));
}

static void add_firewall_rule(void)
{
    char addr[128];
    char params[300];
    char exe[MAX_PATH];
    HINSTANCE rc;
    int port;

    GetWindowTextA(item(IDC_ADDR), addr, sizeof(addr));
    port = port_of(addr);
    snprintf(params, sizeof(params),
        "advfirewall firewall add rule name=\"" FW_RULE_FMT "\" dir=in action=allow protocol=UDP localport=%d", port,
        port);
    (void)exe;
    rc = ShellExecuteA(g_wnd, "runas", "netsh.exe", params, NULL, SW_HIDE);
    set_status((INT_PTR)rc > 32 ? "Firewall rule requested (confirm the Windows prompt), then press 'Check port'."
                                : "Firewall rule not added (cancelled or no admin rights).");
}

/* ---- PT65: optional self-update of arcanum-ce.exe from GitHub Releases ----
   Off unless arcanum-coop-launcher.ini has [update] enabled=1. Source:
   [update] releases_url (default the fork's API), tag_prefix (coop-lite-).
   Only arcanum-ce.exe next to the launcher is replaced: never coop-host\ /
   coop-client\, the launcher, game data, saves or the ini. */
#define WM_APP_UPD (WM_APP + 4)
#define UPD_DEFAULT_URL "https://api.github.com/repos/Destroider12/arcanum-ce/releases"

typedef struct UpdInfo {
    char tag[96];
    char zip_url[512];
    char sha_url[512];
    char sha_hex[65]; /* from the body, if no sha256.txt */
} UpdInfo;

static UpdInfo g_upd;
static volatile LONG g_upd_busy;
static bool g_upd_ready;

static bool upd_enabled(void)
{
    return GetPrivateProfileIntA("update", "enabled", 0, g_ini) != 0;
}

static void upd_post(int ready, const char* fmt, const char* a)
{
    char* t = (char*)malloc(300);

    snprintf(t, 300, fmt, a != NULL ? a : "");
    PostMessageA(g_wnd, WM_APP_UPD, (WPARAM)ready, (LPARAM)t);
}

/* whole body of a GET into a malloc'd buffer (NUL-terminated), or NULL */
static char* upd_get(const char* url, DWORD* out_len, DWORD max)
{
    HINTERNET net;
    HINTERNET req;
    char* buf = NULL;
    DWORD len = 0;
    DWORD cap = 0;
    DWORD got;
    char chunk[16384];

    net = InternetOpenA("arcanum-coop-launcher", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (net == NULL) {
        return NULL;
    }
    req = InternetOpenUrlA(net, url, "Accept: application/vnd.github+json\r\n", (DWORD)-1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (req != NULL) {
        DWORD code = 0;
        DWORD sz = sizeof(code);

        HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &sz, NULL);
        if (code == 200) {
            while (InternetReadFile(req, chunk, sizeof(chunk), &got) && got > 0) {
                if (len + got + 1 > cap) {
                    char* nb;

                    cap = (len + got + 1) * 2;
                    if (cap > max) {
                        free(buf);
                        buf = NULL;
                        break;
                    }
                    nb = (char*)realloc(buf, cap);
                    if (nb == NULL) {
                        free(buf);
                        buf = NULL;
                        break;
                    }
                    buf = nb;
                }
                memcpy(buf + len, chunk, got);
                len += got;
            }
            if (buf != NULL) {
                buf[len] = '\0';
            }
        }
        InternetCloseHandle(req);
    }
    InternetCloseHandle(net);
    if (out_len != NULL) {
        *out_len = len;
    }
    return buf;
}

/* "key":"value" at or after p (before end); copies value; returns past it */
static const char* upd_json_str(const char* p, const char* end, const char* key, char* out, size_t size)
{
    char pat[64];
    const char* q;
    size_t n = 0;

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    for (q = strstr(p, pat); q != NULL && q < end; q = strstr(q + 1, pat)) {
        q += strlen(pat);
        while (*q == ' ') {
            q++;
        }
        if (*q != '"') {
            continue;
        }
        q++;
        while (*q != '\0' && *q != '"' && q < end) {
            if (*q == '\\' && q[1] != '\0') {
                q++;
            }
            if (n + 1 < size) {
                out[n++] = *q;
            }
            q++;
        }
        out[n] = '\0';
        return q;
    }
    return NULL;
}

static bool upd_find_hex64(const char* p, const char* end, char* out)
{
    int run = 0;

    for (; p < end && *p != '\0'; p++) {
        bool hx = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F');

        run = hx ? run + 1 : 0;
        if (run == 64 && !(p + 1 < end && ((p[1] >= '0' && p[1] <= '9') || ((p[1] | 32) >= 'a' && (p[1] | 32) <= 'f')))) {
            int i;

            for (i = 0; i < 64; i++) {
                out[i] = (char)(p[i - 63] | 32);
            }
            out[64] = '\0';
            return true;
        }
    }
    return false;
}

/* newest release (the API lists newest first, prereleases included) whose
   tag starts with the prefix */
static bool upd_pick(const char* json, const char* prefix, UpdInfo* u)
{
    const char* p = json;
    const char* end = json + strlen(json);

    while ((p = strstr(p, "\"tag_name\":")) != NULL) {
        const char* next = strstr(p + 1, "\"tag_name\":");
        const char* seg_end = next != NULL ? next : end;
        const char* a;
        char url[512];

        memset(u, 0, sizeof(*u));
        upd_json_str(p, seg_end, "tag_name", u->tag, sizeof(u->tag));
        if (strncmp(u->tag, prefix, strlen(prefix)) == 0) {
            for (a = p; (a = upd_json_str(a, seg_end, "browser_download_url", url, sizeof(url))) != NULL;) {
                size_t n = strlen(url);

                if (n > 4 && _stricmp(url + n - 4, ".zip") == 0 && u->zip_url[0] == '\0') {
                    snprintf(u->zip_url, sizeof(u->zip_url), "%s", url);
                } else if (n > 10 && _stricmp(url + n - 10, "sha256.txt") == 0) {
                    snprintf(u->sha_url, sizeof(u->sha_url), "%s", url);
                }
            }
            {
                const char* b = strstr(p, "\"body\":");

                if (b != NULL && b < seg_end) {
                    upd_find_hex64(b, seg_end, u->sha_hex);
                }
            }
            return true;
        }
        p = seg_end;
        if (next == NULL) {
            break;
        }
    }
    return false;
}

static bool upd_is_current(const char* tag, const char* prefix)
{
    const char* sha = tag + strlen(prefix);
    size_t n = strlen(COOP_BUILD_SHA);

    return n >= 7 && _strnicmp(sha, COOP_BUILD_SHA, n) == 0;
}

static DWORD WINAPI upd_check_thread(LPVOID arg)
{
    char url[512];
    char prefix[64];
    char* json;
    UpdInfo u;

    (void)arg;
    GetPrivateProfileStringA("update", "releases_url", UPD_DEFAULT_URL, url, sizeof(url), g_ini);
    GetPrivateProfileStringA("update", "tag_prefix", "coop-lite-", prefix, sizeof(prefix), g_ini);
    json = upd_get(url, NULL, 8u << 20);
    if (json == NULL) {
        upd_post(0, "Update check failed (offline or no releases).%s", NULL);
    } else if (!upd_pick(json, prefix, &u) || u.zip_url[0] == '\0') {
        upd_post(0, "No release with a zip found (build " COOP_BUILD_SHA ").%s", NULL);
    } else if (upd_is_current(u.tag, prefix)) {
        upd_post(0, "Up to date: %s", u.tag);
    } else {
        g_upd = u;
        upd_post(1, "Update available: %s", u.tag);
    }
    free(json);
    InterlockedExchange(&g_upd_busy, 0);
    return 0;
}

static bool upd_sha256_file(const char* path, char* hex)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char dig[32];
    unsigned char buf[65536];
    FILE* f;
    size_t n;
    bool ok = false;
    int i;

    f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0
        && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        ok = true;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (BCryptHashData(h, buf, (ULONG)n, 0) != 0) {
                ok = false;
                break;
            }
        }
        ok = ok && BCryptFinishHash(h, dig, sizeof(dig), 0) == 0;
    }
    if (h != NULL) {
        BCryptDestroyHash(h);
    }
    if (alg != NULL) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    fclose(f);
    for (i = 0; ok && i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", dig[i]);
    }
    return ok;
}

static bool upd_run_hidden(char* cmd)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

/* arcanum-ce.exe anywhere under dir (zips may have a top folder) */
static bool upd_find_exe(const char* dir, char* out, int depth)
{
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    char pat[MAX_PATH];
    bool found = false;

    snprintf(pat, sizeof(pat), "%s\\*", dir);
    fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        char sub[MAX_PATH];

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            found = depth < 3 && upd_find_exe(sub, out, depth + 1);
        } else if (_stricmp(fd.cFileName, "arcanum-ce.exe") == 0) {
            snprintf(out, MAX_PATH, "%s", sub);
            found = true;
        }
    } while (!found && FindNextFileA(fh, &fd));
    FindClose(fh);
    return found;
}

static DWORD WINAPI upd_apply_thread(LPVOID arg)
{
    UpdInfo u = g_upd;
    char tmp[MAX_PATH];
    char work[MAX_PATH];
    char zip[MAX_PATH];
    char ext[MAX_PATH];
    char cmd[2048];
    char hex[65];
    char want[65] = "";
    char new_exe[MAX_PATH];
    char dst[MAX_PATH];
    char bak[MAX_PATH];
    char* data;
    DWORD len = 0;
    FILE* f;

    (void)arg;
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(work, sizeof(work), "%sarcanum-coop-update-%lu", tmp, (unsigned long)GetTickCount());
    CreateDirectoryA(work, NULL);
    snprintf(zip, sizeof(zip), "%s\\update.zip", work);
    snprintf(ext, sizeof(ext), "%s\\x", work);
    upd_post(0, "Downloading %s ...", u.tag);
    data = upd_get(u.zip_url, &len, 256u << 20);
    if (data == NULL || len == 0) {
        upd_post(1, "Download failed: %s", u.tag);
        goto done;
    }
    f = fopen(zip, "wb");
    if (f == NULL || fwrite(data, 1, len, f) != len) {
        if (f != NULL) {
            fclose(f);
        }
        free(data);
        upd_post(1, "Cannot write the temp file.%s", NULL);
        goto done;
    }
    fclose(f);
    free(data);
    if (u.sha_url[0] != '\0') {
        char* t = upd_get(u.sha_url, NULL, 1u << 20);

        if (t != NULL) {
            upd_find_hex64(t, t + strlen(t), want);
            free(t);
        }
    }
    if (want[0] == '\0') {
        snprintf(want, sizeof(want), "%s", u.sha_hex);
    }
    if (want[0] == '\0') {
        upd_post(1, "Not updated: %s has no sha256 (sha256.txt or body).", u.tag);
        goto done;
    }
    if (!upd_sha256_file(zip, hex) || strcmp(hex, want) != 0) {
        upd_post(1, "Not updated: sha256 mismatch for %s.", u.tag);
        goto done;
    }
    snprintf(cmd, sizeof(cmd),
        "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\"",
        zip, ext);
    if (!upd_run_hidden(cmd) || !upd_find_exe(ext, new_exe, 0)) {
        upd_post(1, "Not updated: could not unpack arcanum-ce.exe.%s", NULL);
        goto done;
    }
    if (g_game != NULL) {
        upd_post(1, "Not updated: the game is running.%s", NULL);
        goto done;
    }
    /* only the root exe; the old one is kept as .old until it works */
    snprintf(dst, sizeof(dst), "%sarcanum-ce.exe", g_dir);
    snprintf(bak, sizeof(bak), "%sarcanum-ce.exe.old", g_dir);
    DeleteFileA(bak);
    if (file_exists(dst) && !MoveFileExA(dst, bak, MOVEFILE_REPLACE_EXISTING)) {
        upd_post(1, "Not updated: arcanum-ce.exe is in use.%s", NULL);
        goto done;
    }
    if (!CopyFileA(new_exe, dst, FALSE)) {
        MoveFileExA(bak, dst, MOVEFILE_REPLACE_EXISTING);
        upd_post(1, "Not updated: copy failed (old exe restored).%s", NULL);
        goto done;
    }
    upd_post(0, "Updated arcanum-ce.exe to %s.", u.tag);
done:
    snprintf(cmd, sizeof(cmd), "cmd.exe /c rmdir /s /q \"%s\"", work);
    upd_run_hidden(cmd);
    InterlockedExchange(&g_upd_busy, 0);
    return 0;
}

static void upd_start(bool apply)
{
    if (!upd_enabled()) {
        SetWindowTextA(item(IDC_UPD_TEXT), "Updates off ([update] enabled=1 in the ini).");
        return;
    }
    if (g_game != NULL) {
        SetWindowTextA(item(IDC_UPD_TEXT), "Updates are disabled while the game runs.");
        return;
    }
    if (InterlockedCompareExchange(&g_upd_busy, 1, 0) != 0) {
        return;
    }
    EnableWindow(item(IDC_UPD_GO), FALSE);
    SetWindowTextA(item(IDC_UPD_TEXT), apply ? "Updating..." : "Checking for updates...");
    CloseHandle(CreateThread(NULL, 0, apply ? upd_apply_thread : upd_check_thread, NULL, 0, NULL));
}

static void upd_on_msg(WPARAM ready, char* text)
{
    SetWindowTextA(item(IDC_UPD_TEXT), text);
    free(text);
    g_upd_ready = ready != 0;
    ShowWindow(item(IDC_UPD_GO), g_upd_ready ? SW_SHOW : SW_HIDE);
    EnableWindow(item(IDC_UPD_GO), g_upd_ready && g_game == NULL);
}

static HWND make(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND h_ = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
        (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);

    SendMessageA(h_, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h_;
}

static void create_controls(void)
{
    HWND combo;

    make("STATIC", "Player name:", 0, 12, 14, 110, 20, 0);
    make("EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 130, 12, 300, 22, IDC_NAME);
    SendMessageA(item(IDC_NAME), EM_LIMITTEXT, 30, 0);
    make("BUTTON", "Host a game", WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 130, 44, 130, 20, IDC_HOST);
    make("BUTTON", "Join a game", BS_AUTORADIOBUTTON, 270, 44, 130, 20, IDC_JOIN);
    make("STATIC", "Transport:", 0, 12, 76, 110, 20, 0);
    combo = make("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 130, 72, 300, 120, IDC_TRANSPORT);
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM) "Direct (IP, UPnP port mapping)");
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM) "Steam (P2P via Steam, needs steam_api64.dll)");
    make("STATIC", "Port (UDP):", 0, 12, 108, 116, 20, IDC_ADDR_LABEL);
    make("EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 130, 106, 300, 22, IDC_ADDR);
    make("STATIC", "Host SteamID64:", 0, 12, 140, 116, 34, IDC_STEAM_LABEL);
    make("EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, 130, 138, 300, 22, IDC_STEAMID);
    make("STATIC", "Local IP: ?", 0, 12, 182, 300, 20, IDC_LOCALIP);
    make("STATIC", "Public IP: (looking up via UPnP...)", 0, 12, 204, 300, 20, IDC_PUBIP);
    make("BUTTON", "Copy", WS_TABSTOP, 330, 190, 100, 26, IDC_COPY);
    make("BUTTON", "Windowed", WS_TABSTOP | BS_AUTOCHECKBOX, 12, 234, 200, 20, IDC_WINDOWED);
    make("BUTTON", "Check port", WS_TABSTOP, 220, 230, 100, 26, IDC_CHECK);
    make("BUTTON", "Add firewall rule", WS_TABSTOP, 326, 230, 104, 26, IDC_FIREWALL);
    make("EDIT", "Press 'Check port' to test whether friends can reach you.", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        12, 262, 418, 150, IDC_CHECK_OUT);
    make("STATIC", "", SS_LEFT, 12, 418, 418, 36, IDC_STATUS);
    make("BUTTON", "Start", WS_TABSTOP | BS_DEFPUSHBUTTON, 330, 460, 100, 30, IDC_START);
    make("BUTTON", "Reconnect", WS_TABSTOP, 12, 460, 96, 30, IDC_RECONNECT);
    make("BUTTON", "Create bug report", WS_TABSTOP, 114, 460, 130, 30, IDC_BUGREPORT);
    make("BUTTON", "Close", WS_TABSTOP, 250, 460, 70, 30, IDC_PANEL_CLOSE);
    make("STATIC", "", SS_LEFT | SS_ENDELLIPSIS, 12, 504, 196, 20, IDC_UPD_TEXT);
    make("BUTTON", "Check for updates", WS_TABSTOP, 214, 500, 120, 26, IDC_UPD_CHECK);
    make("BUTTON", "Update", WS_TABSTOP, 340, 500, 90, 26, IDC_UPD_GO);
    ShowWindow(item(IDC_UPD_GO), SW_HIDE);
    make("STATIC", "Lobby - players && chat (while the game runs):", 0, 12, 536, 418, 18, IDC_LOBBY_LABEL);
    make("EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        12, 556, 200, 100, IDC_PLAYERS);
    make("EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        220, 556, 210, 100, IDC_LOBBY_CHAT);
    make("EDIT", "", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 12, 662, 330, 22, IDC_LOBBY_INPUT);
    SendMessageA(item(IDC_LOBBY_INPUT), EM_LIMITTEXT, 120, 0);
    make("BUTTON", "Send", WS_TABSTOP | BS_DEFPUSHBUTTON, 350, 660, 80, 26, IDC_LOBBY_SEND);
    show_panel(false);
    show_lobby(false);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        create_controls();
        load_settings();
        refresh();
        set_status("Host: start, then send your address (or SteamID64) to friends. Join: works while the host plays.");
        CloseHandle(CreateThread(NULL, 0, upnp_thread, NULL, 0, NULL));
        {
            char old[MAX_PATH];

            snprintf(old, sizeof(old), "%sarcanum-ce.exe.old", g_dir);
            DeleteFileA(old); /* PT65: last update's backup */
        }
        if (upd_enabled()) {
            upd_start(false);
        } else {
            SetWindowTextA(item(IDC_UPD_TEXT), "Updates off ([update] enabled=1 in the ini).");
        }
        return 0;
    case WM_APP_UPD:
        upd_on_msg(wp, (char*)lp);
        return 0;
    case WM_APP_CHECK:
        SetWindowTextA(item(IDC_CHECK_OUT), (const char*)lp);
        free((void*)lp);
        InterlockedExchange(&g_check_running, 0);
        return 0;
    case WM_APP_GAMEEXIT:
        on_game_exit((DWORD)wp, (HANDLE)lp);
        return 0;
    case WM_APP_PUBIP:
        snprintf(g_public_ip, sizeof(g_public_ip), "%s", (const char*)lp);
        refresh();
        return 0;
    case WM_TIMER:
        if (wp == IDT_LOBBY) {
            lobby_poll();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_HOST:
            refresh();
            if (!is_steam()) {
                start_check(true); /* PT59: automatic check on Host */
            }
            break;
        case IDC_JOIN:
            refresh();
            break;
        case IDC_CHECK:
            start_check(true);
            break;
        case IDC_FIREWALL:
            add_firewall_rule();
            break;
        case IDC_TRANSPORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                refresh();
            }
            break;
        case IDC_COPY:
            copy_address();
            break;
        case IDC_RECONNECT:
            if (g_game == NULL) {
                g_autoload_next = true; /* host: resumes its last save; client (PT63): joins straight away */
                start_game();
            }
            break;
        case IDC_BUGREPORT:
            create_bug_report();
            break;
        case IDC_UPD_CHECK:
            upd_start(false);
            break;
        case IDC_UPD_GO:
            if (g_upd_ready) {
                upd_start(true);
            }
            break;
        case IDC_PANEL_CLOSE:
            show_panel(false);
            set_status("");
            break;
        case IDC_LOBBY_SEND:
            lobby_send_chat();
            break;
        case IDC_LOBBY_INPUT:
            if (HIWORD(wp) == EN_UPDATE && g_lobby_on) {
                /* Enter in a single-line edit notifies the parent dialog-style;
                   IsDialogMessage already routes default button - Send is DEFPUSHBUTTON
                   only while lobby is up; keep an explicit path via button. */
            }
            break;
        case IDC_START:
            if (g_game != NULL) {
                set_status("The game is already running.");
                break;
            }
            if (g_upd_busy) {
                set_status("Wait for the update to finish.");
                break;
            }
            start_game();
            if (is_host() && !is_steam()) {
                /* PT59: never blocks the start; the game owns the port now */
                start_check(false);
            }
            break;
        }
        return 0;
    case WM_CLOSE:
        save_settings();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    WSADATA wsa;
    MSG m;
    RECT r = { 0, 0, 444, 534 };
    char* slash;
    NONCLIENTMETRICSA ncm;

    (void)prev;
    (void)cmdline;
    GetModuleFileNameA(NULL, g_dir, sizeof(g_dir));
    slash = strrchr(g_dir, '\\');
    if (slash != NULL) {
        slash[1] = '\0';
    }
    snprintf(g_ini, sizeof(g_ini), "%sarcanum-coop-launcher.ini", g_dir);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    find_local_ip();
    InitCommonControls();
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "ArcanumCoopLauncher";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowExA(0, wc.lpszClassName, "Arcanum Coop Launcher", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(g_wnd, show);
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    WSACleanup();
    return 0;
}

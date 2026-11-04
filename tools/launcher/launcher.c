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
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
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

static HWND g_wnd;
static HFONT g_font;
static char g_dir[MAX_PATH]; /* launcher folder, trailing backslash */
static char g_ini[MAX_PATH];
static char g_local_ip[64];
static char g_public_ip[64];

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

static LONGLONG file_size(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        return -1;
    }
    return ((LONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

/* Game folder: coop-host\ / coop-client\ (Coop-Setup) or the launcher's. */
static void game_dir(bool host, char* out, size_t size)
{
    char exe[MAX_PATH];
    char root_exe[MAX_PATH];

    snprintf(out, size, "%s%s\\", g_dir, host ? "coop-host" : "coop-client");
    snprintf(exe, sizeof(exe), "%sarcanum-ce.exe", out);
    if (!file_exists(exe)) {
        snprintf(out, size, "%s", g_dir);
        return;
    }
    /* Like Start-*.bat: refresh the instance exe from a newer root build. */
    snprintf(root_exe, sizeof(root_exe), "%sarcanum-ce.exe", g_dir);
    if (file_exists(root_exe) && file_size(root_exe) != file_size(exe)) {
        DeleteFileA(exe);
        CopyFileA(root_exe, exe, FALSE);
    }
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
    game_dir(host, dir, sizeof(dir));
    snprintf(exe, sizeof(exe), "%sarcanum-ce.exe", dir);
    if (!file_exists(exe)) {
        set_status("arcanum-ce.exe not found next to the launcher (or in coop-host\\ / coop-client\\).");
        return;
    }
    if (host) {
        snprintf(cmd, sizeof(cmd), "\"%s\"%s -coop -host %d -log coop_host.log", exe, windowed ? " -window" : "",
            port_of(addr));
    } else if (steam) {
        snprintf(cmd, sizeof(cmd), "\"%s\"%s -coop -join %s -log coop_client.log", exe, windowed ? " -window" : "",
            steam_id);
    } else {
        if (strchr(addr, ':') == NULL) {
            strncat(addr, ":7777", sizeof(addr) - strlen(addr) - 1);
        }
        snprintf(cmd, sizeof(cmd), "\"%s\"%s -coop -join %s -log coop_client.log", exe, windowed ? " -window" : "",
            addr);
    }
    SetEnvironmentVariableA("ARCANUM_COOP_NAME", name);
    SetEnvironmentVariableA("ARCANUM_COOP_TRANSPORT", steam ? "steam" : "direct");
    SetEnvironmentVariableA("ARCANUM_COOP_STEAM_HOST", !host && steam ? steam_id : NULL);
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
    CloseHandle(pi.hProcess);
    set_status(host ? "Game started as host. Friends can join any time (late join)." : "Game started - joining the host.");
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
    make("STATIC", "", SS_LEFT, 12, 262, 418, 36, IDC_STATUS);
    make("BUTTON", "Start", WS_TABSTOP | BS_DEFPUSHBUTTON, 330, 304, 100, 30, IDC_START);
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
        return 0;
    case WM_APP_PUBIP:
        snprintf(g_public_ip, sizeof(g_public_ip), "%s", (const char*)lp);
        refresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_HOST:
        case IDC_JOIN:
            refresh();
            break;
        case IDC_TRANSPORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                refresh();
            }
            break;
        case IDC_COPY:
            copy_address();
            break;
        case IDC_START:
            start_game();
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
    RECT r = { 0, 0, 444, 346 };
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

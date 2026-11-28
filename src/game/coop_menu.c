#include "game/coop_menu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COOP_MENU_ADDR_FILE "coop_join.txt"
#define COOP_MENU_DEFAULT_ADDR "127.0.0.1"
#define COOP_MENU_ADDR_MAX 80

bool coop_menu_address_valid(const char* addr)
{
    size_t len;
    size_t i;
    const char* colon;

    if (addr == NULL) {
        return false;
    }
    len = strlen(addr);
    if (len == 0 || len > COOP_MENU_ADDR_MAX || !isalnum((unsigned char)addr[0])) {
        return false;
    }
    colon = strchr(addr, ':');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)addr[i];

        if (!isalnum(c) && c != '.' && c != '-' && c != '_' && c != ':') {
            return false;
        }
    }
    if (colon != NULL) {
        int port;

        if (strchr(colon + 1, ':') != NULL || colon[1] == '\0' || colon == addr) {
            return false;
        }
        for (i = (size_t)(colon - addr) + 1; i < len; i++) {
            if (!isdigit((unsigned char)addr[i])) {
                return false;
            }
        }
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) {
            return false;
        }
    }
    return true;
}

bool coop_menu_address_from_text(const char* text, char* out, size_t out_size)
{
    char buf[COOP_MENU_ADDR_MAX + 16];
    size_t len;

    if (text == NULL || out == NULL || out_size == 0) {
        return false;
    }
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    len = strlen(text);
    if (len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, text, len + 1);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) {
        buf[--len] = '\0';
    }
    if (!coop_menu_address_valid(buf) || len + 1 > out_size) {
        return false;
    }
    memcpy(out, buf, len + 1);
    return true;
}

void coop_menu_last_address(char* out, size_t out_size)
{
    FILE* f;
    char line[COOP_MENU_ADDR_MAX + 16];

    if (out == NULL || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", COOP_MENU_DEFAULT_ADDR);
    f = fopen(COOP_MENU_ADDR_FILE, "r");
    if (f == NULL) {
        return;
    }
    if (fgets(line, sizeof(line), f) != NULL) {
        char parsed[COOP_MENU_ADDR_MAX + 16];

        if (coop_menu_address_from_text(line, parsed, sizeof(parsed)) && strlen(parsed) < out_size) {
            strcpy(out, parsed);
        }
    }
    fclose(f);
}

void coop_menu_remember_address(const char* addr)
{
    FILE* f;
    char last[COOP_MENU_ADDR_MAX + 16];

    if (!coop_menu_address_valid(addr)) {
        return;
    }
    coop_menu_last_address(last, sizeof(last));
    if (strcmp(last, addr) == 0) {
        return;
    }
    f = fopen(COOP_MENU_ADDR_FILE, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%s\n", addr);
    fclose(f);
}

void coop_menu_pick_address_from(const char* clip, char* out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    if (clip != NULL) {
        char parsed[COOP_MENU_ADDR_MAX + 16];

        /* A bare word on the clipboard is not an address; the host's IP or
           name:port has a '.' or a ':'. */
        if ((strchr(clip, '.') != NULL || strchr(clip, ':') != NULL)
            && coop_menu_address_from_text(clip, parsed, sizeof(parsed))
            && strlen(parsed) < out_size) {
            strcpy(out, parsed);
            return;
        }
    }
    coop_menu_last_address(out, out_size);
}

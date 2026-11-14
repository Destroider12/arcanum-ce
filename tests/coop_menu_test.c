/* PT33: main-menu co-op helpers: address validation, remembered address. */
#include <stdio.h>
#include <string.h>

#include "game/coop_menu.h"

static int failures;

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
            failures++; \
        } \
    } while (0)

int main(void)
{
    char out[96];

    CHECK(coop_menu_address_valid("127.0.0.1"));
    CHECK(coop_menu_address_valid("192.168.1.20:7778"));
    CHECK(coop_menu_address_valid("my-host.example.com"));
    CHECK(coop_menu_address_valid("localhost"));
    CHECK(!coop_menu_address_valid(""));
    CHECK(!coop_menu_address_valid(NULL));
    CHECK(!coop_menu_address_valid(" 1.2.3.4"));
    CHECK(!coop_menu_address_valid("1.2.3.4:"));
    CHECK(!coop_menu_address_valid("1.2.3.4:99999"));
    CHECK(!coop_menu_address_valid("1.2.3.4:80:80"));
    CHECK(!coop_menu_address_valid("a b"));
    CHECK(!coop_menu_address_valid("-rf"));
    CHECK(!coop_menu_address_valid("1.2.3.4;rm"));

    CHECK(coop_menu_address_from_text("  10.0.0.5\r\n", out, sizeof(out)) && strcmp(out, "10.0.0.5") == 0);
    CHECK(!coop_menu_address_from_text("hello world", out, sizeof(out)));

    remove("coop_join.txt");
    coop_menu_last_address(out, sizeof(out));
    CHECK(strcmp(out, "127.0.0.1") == 0);
    coop_menu_remember_address("10.1.2.3:7777");
    coop_menu_last_address(out, sizeof(out));
    CHECK(strcmp(out, "10.1.2.3:7777") == 0);
    coop_menu_remember_address("not an address");
    coop_menu_last_address(out, sizeof(out));
    CHECK(strcmp(out, "10.1.2.3:7777") == 0);

    coop_menu_pick_address_from("172.16.0.9\n", out, sizeof(out));
    CHECK(strcmp(out, "172.16.0.9") == 0);
    coop_menu_pick_address_from("just some words", out, sizeof(out));
    CHECK(strcmp(out, "10.1.2.3:7777") == 0);
    coop_menu_pick_address_from("password", out, sizeof(out)); /* bare word: not an address */
    CHECK(strcmp(out, "10.1.2.3:7777") == 0);
    coop_menu_pick_address_from(NULL, out, sizeof(out));
    CHECK(strcmp(out, "10.1.2.3:7777") == 0);
    remove("coop_join.txt");

    if (failures != 0) {
        return 1;
    }
    printf("coop_menu_test OK\n");
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <rte_eal.h>

#include "l3_table.h"

int main(int argc, char **argv)
{
    (void)argc;
    printf("=====================================================\n");
    printf("         UNIT TEST: DPDK L3 Table Lookup            \n");
    printf("=====================================================\n");

    /* Khởi tạo EAL với cờ --no-huge --no-pci để test không cần root / hugepages */
    char *fake_argv[] = { argv[0], "--no-huge", "--no-pci", "-m", "128", "--file-prefix=test_l3", NULL };
    int fake_argc = 6;
    int ret = rte_eal_init(fake_argc, fake_argv);
    if (ret < 0) {
        fprintf(stderr, "Error: rte_eal_init failed in unit test\n");
        return 1;
    }

    struct l3_table *table = l3_table_init("TEST_L3", rte_socket_id());
    assert(table != NULL);

    /* 1. Nạp các luật test trực tiếp vào bảng LPM */
    assert(l3_table_add_route(table, "172.217.0.0/16", L3_ACTION_FORWARD) == 0);
    assert(l3_table_add_route(table, "142.250.0.0/16", L3_ACTION_FORWARD) == 0);
    assert(l3_table_add_route(table, "157.240.0.0/16", L3_ACTION_DROP) == 0);
    assert(l3_table_add_route(table, "96.127.0.0/16", L3_ACTION_DROP) == 0);
    assert(l3_table_add_route(table, "0.0.0.0/0", L3_ACTION_FORWARD) == 0);
    printf("Successfully added 5 test rules to LPM table\n");

    /* 2. Test IP Google (172.217.182.112) -> Phải FORWARD */
    struct in_addr ip1;
    inet_pton(AF_INET, "172.217.182.112", &ip1);
    l3_action_t act1 = l3_table_lookup(table, ip1.s_addr);
    printf("Lookup 172.217.182.112 (Google) -> %s (Expected: FORWARD)\n", act1 == L3_ACTION_FORWARD ? "FORWARD" : "DROP");
    assert(act1 == L3_ACTION_FORWARD);

    /* 3. Test IP Google dải 142.250.0.0/16 -> Phải FORWARD */
    struct in_addr ip2;
    inet_pton(AF_INET, "142.250.14.95", &ip2);
    l3_action_t act2 = l3_table_lookup(table, ip2.s_addr);
    printf("Lookup 142.250.14.95   (Google) -> %s (Expected: FORWARD)\n", act2 == L3_ACTION_FORWARD ? "FORWARD" : "DROP");
    assert(act2 == L3_ACTION_FORWARD);

    /* 4. Test IP Meta (157.240.4.224) -> Phải DROP */
    struct in_addr ip3;
    inet_pton(AF_INET, "157.240.4.224", &ip3);
    l3_action_t act3 = l3_table_lookup(table, ip3.s_addr);
    printf("Lookup 157.240.4.224   (Meta)   -> %s (Expected: DROP)\n", act3 == L3_ACTION_DROP ? "DROP" : "FORWARD");
    assert(act3 == L3_ACTION_DROP);

    /* 5. Test IP AWS (96.127.66.138) -> Phải DROP */
    struct in_addr ip4;
    inet_pton(AF_INET, "96.127.66.138", &ip4);
    l3_action_t act4 = l3_table_lookup(table, ip4.s_addr);
    printf("Lookup 96.127.66.138   (AWS)    -> %s (Expected: DROP)\n", act4 == L3_ACTION_DROP ? "DROP" : "FORWARD");
    assert(act4 == L3_ACTION_DROP);

    /* 6. Test IP khác (8.8.8.8) -> Phải FORWARD theo default route */
    struct in_addr ip5;
    inet_pton(AF_INET, "8.8.8.8", &ip5);
    l3_action_t act5 = l3_table_lookup(table, ip5.s_addr);
    printf("Lookup 8.8.8.8         (DNS)    -> %s (Expected: FORWARD)\n", act5 == L3_ACTION_FORWARD ? "FORWARD" : "DROP");
    assert(act5 == L3_ACTION_FORWARD);

    l3_table_free(table);
    rte_eal_cleanup();

    printf("\n\033[1;32m[ALL TESTS PASSED] Toàn bộ test case tra cứu L3 đều thành công 100%%!\033[0m\n");
    return 0;
}

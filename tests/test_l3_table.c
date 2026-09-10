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
    char *fake_argv[] = { argv[0], "--no-huge", "--no-pci", "-m", "128", NULL };
    int fake_argc = 5;
    int ret = rte_eal_init(fake_argc, fake_argv);
    if (ret < 0) {
        fprintf(stderr, "Error: rte_eal_init failed in unit test\n");
        return 1;
    }

    struct l3_table *table = l3_table_init("TEST_L3", rte_socket_id());
    assert(table != NULL);

    /* 1. Nạp file cấu hình pod0-forwarder/routes.conf */
    int loaded = l3_table_load_file(table, "pod0-forwarder/routes.conf");
    printf("Loaded %d rules from pod0-forwarder/routes.conf\n", loaded);
    assert(loaded >= 2);

    /* 2. Test IP hợp lệ (10.0.0.2) -> Phải FORWARD */
    struct in_addr ip1;
    inet_pton(AF_INET, "10.0.0.2", &ip1);
    l3_action_t act1 = l3_table_lookup(table, ip1.s_addr);
    printf("Lookup 10.0.0.2       -> %s (Expected: FORWARD)\n", act1 == L3_ACTION_FORWARD ? "FORWARD" : "DROP");
    assert(act1 == L3_ACTION_FORWARD);

    /* 3. Test IP hợp lệ dải phụ (192.168.10.50) -> Phải FORWARD */
    struct in_addr ip2;
    inet_pton(AF_INET, "192.168.10.50", &ip2);
    l3_action_t act2 = l3_table_lookup(table, ip2.s_addr);
    printf("Lookup 192.168.10.50  -> %s (Expected: FORWARD)\n", act2 == L3_ACTION_FORWARD ? "FORWARD" : "DROP");
    assert(act2 == L3_ACTION_FORWARD);

    /* 4. Test IP ngoài dải (192.168.99.10) -> Phải DROP */
    struct in_addr ip3;
    inet_pton(AF_INET, "192.168.99.10", &ip3);
    l3_action_t act3 = l3_table_lookup(table, ip3.s_addr);
    printf("Lookup 192.168.99.10  -> %s (Expected: DROP)\n", act3 == L3_ACTION_DROP ? "DROP" : "FORWARD");
    assert(act3 == L3_ACTION_DROP);

    /* 5. Test IP public internet (8.8.8.8) -> Phải DROP */
    struct in_addr ip4;
    inet_pton(AF_INET, "8.8.8.8", &ip4);
    l3_action_t act4 = l3_table_lookup(table, ip4.s_addr);
    printf("Lookup 8.8.8.8        -> %s (Expected: DROP)\n", act4 == L3_ACTION_DROP ? "DROP" : "FORWARD");
    assert(act4 == L3_ACTION_DROP);

    l3_table_free(table);
    rte_eal_cleanup();

    printf("\n\033[1;32m[ALL TESTS PASSED] Toàn bộ test case tra cứu L3 đều thành công 100%%!\033[0m\n");
    return 0;
}

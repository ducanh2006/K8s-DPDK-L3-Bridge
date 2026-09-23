#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <arpa/inet.h>
#include <rte_eal.h>

#include "flow_table.h"

static int match_packet(const struct flow_table *ft, uint8_t proto, const char *src_ip, const char *dst_ip, uint16_t src_port, uint16_t dst_port)
{
    struct in_addr s_addr = {0}, d_addr = {0};
    if (src_ip && strcmp(src_ip, "*") != 0) {
        inet_pton(AF_INET, src_ip, &s_addr);
    }
    if (dst_ip && strcmp(dst_ip, "*") != 0) {
        inet_pton(AF_INET, dst_ip, &d_addr);
    }

    rte_be32_t s_net = s_addr.s_addr;
    rte_be32_t d_net = d_addr.s_addr;
    rte_be16_t s_port = rte_cpu_to_be_16(src_port);
    rte_be16_t d_port = rte_cpu_to_be_16(dst_port);

    for (size_t i = 0; i < ft->num_rules; i++) {
        const struct flow_rule *r = &ft->rules[i];

        if (r->proto != 0 && proto != r->proto) continue;
        if (r->dst_mask != 0 && (d_net & r->dst_mask) != r->dst_net) continue;
        if (r->src_mask != 0 && (s_net & r->src_mask) != r->src_net) continue;
        if (r->dst_port != 0 && d_port != r->dst_port) continue;
        if (r->src_port != 0 && s_port != r->src_port) continue;

        return (int)r->group_idx;
    }
    return -1;
}

int main(int argc, char **argv)
{
    (void)argc;
    printf("=====================================================\n");
    printf("     UNIT TEST: DPDK Flow Table Runtime Parser      \n");
    printf("=====================================================\n");

    /* Khởi tạo EAL giả lập offline không cần hugepages/root */
    char *fake_argv[] = { argv[0], "--no-huge", "--no-pci", "-m", "64", "--file-prefix=test_ft", NULL };
    int fake_argc = 6;
    int ret = rte_eal_init(fake_argc, fake_argv);
    if (ret < 0) {
        fprintf(stderr, "Error: rte_eal_init failed in unit test\n");
        return 1;
    }

    struct flow_table ft;

    /* 1. Test nạp file cấu hình chuẩn */
    const char *conf_path = "manifests/ovs_flows.conf";
    printf("[Test 1] Nạp cấu hình từ '%s'...\n", conf_path);
    assert(flow_table_load(&ft, conf_path) == 0);
    assert(ft.num_groups == 8);
    assert(ft.num_rules == 16);
    printf("  -> OK: Đã nạp chính xác %zu groups và %zu rules.\n", ft.num_groups, ft.num_rules);

    /* 2. Test thứ tự ưu tiên giảm dần */
    printf("[Test 2] Kiểm tra thứ tự ưu tiên giảm dần (Priority Descending)...\n");
    for (size_t i = 0; i + 1 < ft.num_rules; i++) {
        assert(ft.rules[i].priority >= ft.rules[i + 1].priority);
    }
    assert(ft.rules[0].priority == 8);
    assert(ft.rules[ft.num_rules - 1].priority == 1);
    printf("  -> OK: Luật đầu tiên có Priority = %u, luật cuối có Priority = %u.\n",
           ft.rules[0].priority, ft.rules[ft.num_rules - 1].priority);

    /* In tóm tắt cấu trúc đã nạp */
    flow_table_dump(&ft);

    /* 3. Test đối sánh luật theo từng dịch vụ */
    printf("[Test 3] Kiểm tra logic so khớp gói tin (First-Match-Wins)...\n");

    /* 3.1. Facebook IP 31.13.70.1 -> Group fg_l34_facebook (Prio 8, DROP) */
    int g_fb = match_packet(&ft, IPPROTO_TCP, "10.0.0.1", "31.13.70.1", 12345, 443);
    printf("  IP 31.13.70.1:443 -> Group %d (%s, action: %s)\n",
           g_fb, ft.groups[g_fb].name, ft.groups[g_fb].action);
    assert(strcmp(ft.groups[g_fb].name, "fg_l34_facebook") == 0);
    assert(ft.groups[g_fb].is_drop == true);

    /* 3.2. AWS IP 96.127.10.2 -> Group fg_l34_aws (Prio 7, DROP) */
    int g_aws = match_packet(&ft, IPPROTO_TCP, "10.0.0.1", "96.127.10.2", 12345, 80);
    printf("  IP 96.127.10.2:80 -> Group %d (%s, action: %s)\n",
           g_aws, ft.groups[g_aws].name, ft.groups[g_aws].action);
    assert(strcmp(ft.groups[g_aws].name, "fg_l34_aws") == 0);
    assert(ft.groups[g_aws].is_drop == true);

    /* 3.3. Youtube IP 142.250.10.1:443 (TCP) -> Group fg_l34_youtube (Prio 6, FORWARD) */
    int g_yt = match_packet(&ft, IPPROTO_TCP, "10.0.0.1", "142.250.10.1", 12345, 443);
    printf("  IP 142.250.10.1:443 -> Group %d (%s, action: %s)\n",
           g_yt, ft.groups[g_yt].name, ft.groups[g_yt].action);
    assert(strcmp(ft.groups[g_yt].name, "fg_l34_youtube") == 0);
    assert(ft.groups[g_yt].is_drop == false);

    /* 3.4. HTTP Port 80 (TCP, random IP 1.2.3.4) -> Group fg_l34_http_sdf1003 (Prio 5, FORWARD) */
    int g_http = match_packet(&ft, IPPROTO_TCP, "10.0.0.1", "1.2.3.4", 12345, 80);
    printf("  IP 1.2.3.4:80 (TCP) -> Group %d (%s, action: %s)\n",
           g_http, ft.groups[g_http].name, ft.groups[g_http].action);
    assert(strcmp(ft.groups[g_http].name, "fg_l34_http_sdf1003") == 0);
    assert(ft.groups[g_http].is_drop == false);

    /* 3.5. HTTPS Port 443 (TCP, random IP 1.2.3.4) -> Group fg_l34_https_sdf1004 (Prio 4, FORWARD) */
    int g_https = match_packet(&ft, IPPROTO_TCP, "10.0.0.1", "1.2.3.4", 12345, 443);
    printf("  IP 1.2.3.4:443 (TCP) -> Group %d (%s, action: %s)\n",
           g_https, ft.groups[g_https].name, ft.groups[g_https].action);
    assert(strcmp(ft.groups[g_https].name, "fg_l34_https_sdf1004") == 0);
    assert(ft.groups[g_https].is_drop == false);

    /* 3.6. DNS Port 53 (UDP) -> Group fg_l34_dns_sdf1005 (Prio 3, FORWARD) */
    int g_dns = match_packet(&ft, IPPROTO_UDP, "10.0.0.1", "8.8.8.8", 12345, 53);
    printf("  IP 8.8.8.8:53 (UDP) -> Group %d (%s, action: %s)\n",
           g_dns, ft.groups[g_dns].name, ft.groups[g_dns].action);
    assert(strcmp(ft.groups[g_dns].name, "fg_l34_dns_sdf1005") == 0);
    assert(ft.groups[g_dns].is_drop == false);

    /* 3.7. UDP Port 9999 -> Group fg_l34_udp_sdf1006 (Prio 2, DROP) */
    int g_udp = match_packet(&ft, IPPROTO_UDP, "10.0.0.1", "8.8.8.8", 12345, 9999);
    printf("  IP 8.8.8.8:9999 (UDP) -> Group %d (%s, action: %s)\n",
           g_udp, ft.groups[g_udp].name, ft.groups[g_udp].action);
    assert(strcmp(ft.groups[g_udp].name, "fg_l34_udp_sdf1006") == 0);
    assert(ft.groups[g_udp].is_drop == true);

    /* 3.8. ICMP Ping -> Group fg_l34_default (Prio 1, FORWARD) */
    int g_def = match_packet(&ft, IPPROTO_ICMP, "10.0.0.1", "1.1.1.1", 0, 0);
    printf("  IP 1.1.1.1 (ICMP) -> Group %d (%s, action: %s)\n",
           g_def, ft.groups[g_def].name, ft.groups[g_def].action);
    assert(strcmp(ft.groups[g_def].name, "fg_l34_default") == 0);
    assert(ft.groups[g_def].is_drop == false);

    /* 4. Test trường hợp file không tồn tại */
    printf("[Test 4] Kiểm tra bắt lỗi file không tồn tại...\n");
    struct flow_table bad_ft;
    assert(flow_table_load(&bad_ft, "manifests/non_existent_file.conf") == -1);
    printf("  -> OK: Hàm trả về -1 chính xác khi file không tồn tại.\n");

    /* Dọn dẹp tài nguyên */
    flow_table_free(&ft);
    rte_eal_cleanup();

    printf("\n=====================================================\n");
    printf("    [THÀNH CÔNG] TOÀN BỘ UNIT TEST FLOW TABLE ĐÃ PASS!\n");
    printf("=====================================================\n");
    return 0;
}

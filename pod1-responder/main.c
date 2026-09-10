#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>

#include "dpdk_init.h"
#include "l3_table.h"
#include "pkt_utils.h"

static volatile bool force_quit = false;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n[Pod1] Signal %d received, preparing to exit...\n", signum);
        force_quit = true;
    }
}

struct app_config {
    char config_path[256];
};

static void print_usage(const char *prgname)
{
    printf("Usage: %s [EAL options] -- [APP options]\n"
           "APP Options:\n"
           "  --config <path>    Đường dẫn file bảng luật L3 (mặc định: pod1-responder/routes.conf)\n",
           prgname);
}

static int parse_app_args(int argc, char **argv, struct app_config *cfg)
{
    snprintf(cfg->config_path, sizeof(cfg->config_path), "pod1-responder/routes.conf");

    static struct option lgopts[] = {
        {"config", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };

    int opt, opt_idx;
    while ((opt = getopt_long(argc, argv, "c:", lgopts, &opt_idx)) != EOF) {
        switch (opt) {
        case 'c':
            snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", optarg);
            break;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=====================================================\n");
    printf("   K8s DPDK L3 Bridge: Pod 1 (Responder / Echo-ACK)  \n");
    printf("=====================================================\n");

    /* 1. Khởi tạo hạ tầng DPDK */
    uint16_t port_id = 0;
    int eal_consumed = 0;
    struct rte_mempool *pool = init_dpdk_subsystem(argc, argv, &port_id, &eal_consumed);
    if (!pool) {
        return 1;
    }

    /* 2. Đọc các tham số ứng dụng */
    int app_argc = argc - eal_consumed;
    char **app_argv = argv + eal_consumed;
    struct app_config cfg;
    if (parse_app_args(app_argc, app_argv, &cfg) < 0) {
        cleanup_dpdk_subsystem(port_id);
        return 1;
    }

    /* 3. Khởi tạo bảng luật L3 */
    struct l3_table *table = l3_table_init("POD1_L3_TABLE", rte_socket_id());
    if (!table) {
        cleanup_dpdk_subsystem(port_id);
        return 1;
    }

    if (l3_table_load_file(table, cfg.config_path) < 0) {
        printf("[Pod1] Warning: Could not load '%s', using default rules\n", cfg.config_path);
        l3_table_add_route(table, "10.0.0.0/24", L3_ACTION_FORWARD);
        l3_table_add_route(table, "0.0.0.0/0", L3_ACTION_DROP);
    }

    printf("[Pod1] Configuration:\n");
    printf("       - Route File: %s\n", cfg.config_path);
    printf("       - Listening for PING requests on port %u...\n", port_id);
    printf("-----------------------------------------------------\n");

    uint32_t rx_ping_count = 0;
    uint32_t rx_dropped_l3 = 0;
    uint32_t tx_ack_count = 0;

    /* 4. Vòng lặp nhận và phản hồi */
    while (!force_quit) {
        struct rte_mbuf *rx_pkts[32];
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_pkts, 32);

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = rx_pkts[i];
            struct rte_ether_hdr *eth = NULL;
            struct rte_ipv4_hdr *ip = NULL;
            struct rte_udp_hdr *udp = NULL;
            struct test_payload *payload = NULL;

            if (parse_ipv4_packet(m, &eth, &ip, &udp, &payload)) {
                /* Kiểm tra IP nguồn trong bảng luật chuyển mạch L3 */
                l3_action_t act = l3_table_lookup(table, ip->src_addr);

                if (act == L3_ACTION_DROP) {
                    /* DROP gói tin theo luật L3 */
                    rx_dropped_l3++;
                    char src_str[INET_ADDRSTRLEN];
                    ip_to_str(ip->src_addr, src_str, sizeof(src_str));
                    printf("[Pod1] [DROP] Incoming packet from %s denied by L3 table -> Dropped\n", src_str);
                    rte_pktmbuf_free(m);
                    continue;
                }

                /* Nếu hợp lệ và là gói PING */
                if (payload && payload->magic == PING_MAGIC) {
                    rx_ping_count++;

                    char src_str[INET_ADDRSTRLEN];
                    char dst_str[INET_ADDRSTRLEN];
                    ip_to_str(ip->src_addr, src_str, sizeof(src_str));
                    ip_to_str(ip->dst_addr, dst_str, sizeof(dst_str));

                    printf("[Pod1] [ACCEPT/RX] Received PING #%u from %s to %s | Msg: \"%s\"\n",
                           payload->seq, src_str, dst_str, payload->message);

                    /* 1. Cập nhật payload thành gói ACK */
                    payload->magic = ACK_MAGIC;
                    snprintf(payload->message, sizeof(payload->message), "ACK_RESP_#%u", payload->seq);

                    /* 2. Đảo chiều MAC, IP và UDP port */
                    swap_l2_l3_addresses(eth, ip, udp);

                    /* 3. Tính lại checksum IPv4 */
                    recalculate_ipv4_checksum(ip);

                    /* 4. Gửi trả gói tin ACK qua OVS về cho Pod 0 */
                    uint16_t sent = rte_eth_tx_burst(port_id, 0, &m, 1);
                    if (sent == 1) {
                        tx_ack_count++;
                        print_packet_log("Pod1", "FORWARD/TX_ACK", ip->src_addr, ip->dst_addr, payload->seq, "ACK Sent back to Pod 0");
                    } else {
                        fprintf(stderr, "[Pod1] [TX_FAIL] Failed to send ACK #%u\n", payload->seq);
                        rte_pktmbuf_free(m);
                    }
                    continue;
                }
            }

            /* Giải phóng nếu gói không phù hợp */
            rte_pktmbuf_free(m);
        }

        /* Nghỉ nhẹ 1ms */
        usleep(1000);
    }

    printf("\n=====================================================\n");
    printf("             Pod 1 Statistics Summary                \n");
    printf("=====================================================\n");
    printf("PING Received   : %u\n", rx_ping_count);
    printf("L3 Dropped      : %u\n", rx_dropped_l3);
    printf("ACK Transmitted : %u\n", tx_ack_count);
    printf("=====================================================\n");

    l3_table_free(table);
    cleanup_dpdk_subsystem(port_id);
    return 0;
}

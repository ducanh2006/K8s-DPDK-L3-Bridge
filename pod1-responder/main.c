#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <inttypes.h>

#include "dpdk_init.h"
#include "l3_table.h"
#include "pkt_utils.h"

#define BURST_SIZE 64

static volatile bool force_quit = false;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n[Pod1] Signal %d received, shutting down gracefully...\n", signum);
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
           "  --config <path>    Đường dẫn file bảng luật L3 (mặc định: /app/routes.conf hoặc routes.conf)\n",
           prgname);
}

static int parse_app_args(int argc, char **argv, struct app_config *cfg)
{
    snprintf(cfg->config_path, sizeof(cfg->config_path), "/app/routes.conf");

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
    printf("  K8s DPDK L3 Bridge: Pod 1 (High-Speed Traffic Sink)\n");
    printf("=====================================================\n");

    /* 1. Khởi tạo DPDK EAL và cổng mạng virtio-user */
    uint16_t nb_ports = 0;
    int eal_consumed = 0;
    if (!init_dpdk_subsystem(argc, argv, &nb_ports, &eal_consumed)) {
        return 1;
    }

    uint16_t port_id = 0;
    printf("[Pod1] Listening on Port %u (Virtio-User from OVS)...\n", port_id);

    /* 2. Đọc các tham số ứng dụng */
    int app_argc = argc - eal_consumed;
    char **app_argv = argv + eal_consumed;
    struct app_config cfg;
    if (parse_app_args(app_argc, app_argv, &cfg) < 0) {
        cleanup_dpdk_subsystem(nb_ports);
        return 1;
    }

    /* 3. Nạp bảng luật định tuyến L3 cho Pod 1 */
    struct l3_table *table = l3_table_init("POD1_L3_TABLE", rte_socket_id());
    if (table) {
        if (l3_table_load_file(table, cfg.config_path) < 0) {
            l3_table_load_file(table, "pod1-responder/routes.conf");
        }
    }

    printf("[Pod1] Ready to receive high-throughput traffic from Pod 0 via OVS!\n");
    printf("-----------------------------------------------------\n");

    /* Thống kê tích lũy */
    uint64_t total_rx_pkts = 0;
    uint64_t total_rx_bytes = 0;
    uint64_t total_l3_dropped = 0;
    uint64_t total_tcp_pkts = 0;
    uint64_t total_udp_pkts = 0;
    uint64_t total_icmp_pkts = 0;
    uint64_t total_other_pkts = 0;

    /* Thống kê chu kỳ 1 giây */
    uint64_t period_rx_pkts = 0;
    uint64_t period_rx_bytes = 0;
    uint64_t period_l3_dropped = 0;
    uint64_t period_tcp_pkts = 0;
    uint64_t period_udp_pkts = 0;
    uint64_t period_icmp_pkts = 0;
    uint64_t period_other_pkts = 0;

    uint64_t last_stat_time = get_current_time_ns();

    struct rte_mbuf *rx_pkts[BURST_SIZE];

    /* 4. Vòng lặp nhận và phân tích gói tin */
    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_pkts, BURST_SIZE);

        if (nb_rx == 0) {
            uint64_t now = get_current_time_ns();
            if (now - last_stat_time >= 1000000000ULL) {
                double elapsed_sec = (double)(now - last_stat_time) / 1e9;
                double rx_pps = (double)period_rx_pkts / elapsed_sec;
                double rx_mbps = ((double)period_rx_bytes * 8.0) / (elapsed_sec * 1e6);

                if (period_rx_pkts > 0) {
                    printf("[Pod1-Sink] RX: %.0f pps (%.2f Mbps) | Protocols: TCP: %" PRIu64 ", UDP: %" PRIu64 ", ICMP: %" PRIu64 " | Total: %" PRIu64 " pkts\n",
                           rx_pps, rx_mbps, period_tcp_pkts, period_udp_pkts, period_icmp_pkts, total_rx_pkts);
                    fflush(stdout);
                }

                period_rx_pkts = 0;
                period_rx_bytes = 0;
                period_l3_dropped = 0;
                period_tcp_pkts = 0;
                period_udp_pkts = 0;
                period_icmp_pkts = 0;
                period_other_pkts = 0;
                last_stat_time = now;
            }
            usleep(50);
            continue;
        }

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = rx_pkts[i];
            period_rx_pkts++;
            total_rx_pkts++;
            period_rx_bytes += m->pkt_len;
            total_rx_bytes += m->pkt_len;

            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            uint16_t eth_type = rte_be_to_cpu_16(eth->ether_type);

            if (eth_type == RTE_ETHER_TYPE_IPV4) {
                struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((uint8_t *)eth + sizeof(struct rte_ether_hdr));

                /* Kiểm tra bảng luật L3 nếu có */
                if (table) {
                    l3_action_t act = l3_table_lookup(table, ip->dst_addr);
                    if (act == L3_ACTION_DROP) {
                        period_l3_dropped++;
                        total_l3_dropped++;
                        rte_pktmbuf_free(m);
                        continue;
                    }
                }

                if (ip->next_proto_id == IPPROTO_TCP) {
                    period_tcp_pkts++;
                    total_tcp_pkts++;
                } else if (ip->next_proto_id == IPPROTO_UDP) {
                    period_udp_pkts++;
                    total_udp_pkts++;
                } else if (ip->next_proto_id == IPPROTO_ICMP) {
                    period_icmp_pkts++;
                    total_icmp_pkts++;
                } else {
                    period_other_pkts++;
                    total_other_pkts++;
                }
            } else {
                period_other_pkts++;
                total_other_pkts++;
            }

            rte_pktmbuf_free(m);
        }

        /* Định kỳ xuất thống kê mỗi 1.0 giây */
        uint64_t now = get_current_time_ns();
        if (now - last_stat_time >= 1000000000ULL) {
            double elapsed_sec = (double)(now - last_stat_time) / 1e9;
            double rx_pps = (double)period_rx_pkts / elapsed_sec;
            double rx_mbps = ((double)period_rx_bytes * 8.0) / (elapsed_sec * 1e6);

            printf("[Pod1-Sink] RX: %.0f pps (%.2f Mbps) | Protocols: TCP: %" PRIu64 ", UDP: %" PRIu64 ", ICMP: %" PRIu64 " | Total: %" PRIu64 " pkts\n",
                   rx_pps, rx_mbps, period_tcp_pkts, period_udp_pkts, period_icmp_pkts, total_rx_pkts);
            fflush(stdout);

            period_rx_pkts = 0;
            period_rx_bytes = 0;
            period_l3_dropped = 0;
            period_tcp_pkts = 0;
            period_udp_pkts = 0;
            period_icmp_pkts = 0;
            period_other_pkts = 0;
            last_stat_time = now;
        }
    }

    printf("\n=====================================================\n");
    printf("             Pod 1 Final Statistics Summary          \n");
    printf("=====================================================\n");
    printf("Total Received   : %" PRIu64 " pkts (%.2f MB)\n", total_rx_pkts, (double)total_rx_bytes / (1024.0 * 1024.0));
    printf("L3 Dropped       : %" PRIu64 " pkts\n", total_l3_dropped);
    printf("TCP Packets      : %" PRIu64 " (%.1f%%)\n", total_tcp_pkts, total_rx_pkts ? (double)total_tcp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("UDP Packets      : %" PRIu64 " (%.1f%%)\n", total_udp_pkts, total_rx_pkts ? (double)total_udp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("ICMP Packets     : %" PRIu64 " (%.1f%%)\n", total_icmp_pkts, total_rx_pkts ? (double)total_icmp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("Other Packets    : %" PRIu64 "\n", total_other_pkts);
    printf("=====================================================\n");

    if (table) {
        l3_table_free(table);
    }
    cleanup_dpdk_subsystem(nb_ports);
    return 0;
}

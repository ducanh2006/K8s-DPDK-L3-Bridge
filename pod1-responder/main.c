#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <inttypes.h>

#include "dpdk_init.h"
#include "pkt_utils.h"
#include "flow_tracker.h"

#ifdef LEGACY_L3_FILTER
#include "l3_table.h"
#endif

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
           "  --config <path>    (Optional) Path to legacy routes.conf\n",
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
    /* Đảm bảo stdout không bị buffer để kubectl logs -f mượt mà ngay lập tức */
    setvbuf(stdout, NULL, _IONBF, 0);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=====================================================\n");
    printf("  K8s DPDK L3 Bridge: Pod 1 (Traffic Sink & Inspector)\n");
    printf("  MODE: RECEIVE_FROM_OVS (Verifying routed flows)     \n");
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

#ifdef LEGACY_L3_FILTER
    printf("[Pod1] Note: Running with LEGACY_L3_FILTER enabled\n");
    struct l3_table *table = l3_table_init("POD1_L3_TABLE", rte_socket_id());
    if (table) {
        l3_table_load_file(table, cfg.config_path);
    }
#else
    printf("[Pod1] Note: Pure Sink & Flow Inspector mode. All filtering was done by OVS-DPDK.\n");
#endif

    /* 3. Khởi tạo Flow Tracker theo dõi 5-tuple IP/Port */
    struct flow_tracker ft;
    flow_tracker_init(&ft, "[Pod1-RX]");

    printf("[Pod1] Ready to receive and verify filtered traffic from OVS-DPDK!\n");
    printf("-----------------------------------------------------\n");

    /* Thống kê tích lũy */
    uint64_t total_rx_pkts = 0;
    uint64_t total_rx_bytes = 0;
    uint64_t total_tcp_pkts = 0;
    uint64_t total_udp_pkts = 0;
    uint64_t total_icmp_pkts = 0;
    uint64_t total_other_pkts = 0;

    /* Thống kê chu kỳ */
    uint64_t period_rx_pkts = 0;
    uint64_t period_rx_bytes = 0;
    uint64_t period_tcp_pkts = 0;
    uint64_t period_udp_pkts = 0;
    uint64_t period_icmp_pkts = 0;
    uint64_t period_other_pkts = 0;

    uint64_t last_1s_time = get_current_time_ns();
    uint64_t last_5s_time = last_1s_time;

    struct rte_mbuf *rx_pkts[BURST_SIZE];

    /* 4. Vòng lặp nhận và phân tích gói tin */
    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_pkts, BURST_SIZE);

        if (nb_rx > 0) {
            period_rx_pkts += nb_rx;
            total_rx_pkts += nb_rx;

            for (uint16_t i = 0; i < nb_rx; i++) {
                struct rte_mbuf *m = rx_pkts[i];
                period_rx_bytes += m->pkt_len;
                total_rx_bytes += m->pkt_len;

                /* Phân tích protocol header */
                struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                uint16_t eth_type = rte_be_to_cpu_16(eth->ether_type);

                if (eth_type == RTE_ETHER_TYPE_IPV4) {
                    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((uint8_t *)eth + sizeof(struct rte_ether_hdr));
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

                    /* Ghi nhận vào bảng Flow Tracker 5-tuple */
                    flow_tracker_record(&ft, m);
                } else {
                    period_other_pkts++;
                    total_other_pkts++;
                }

                /* Giải phóng mbuf về pool */
                rte_pktmbuf_free(m);
            }
        } else {
            usleep(50);
        }

        uint64_t now = get_current_time_ns();

        /* Chu kỳ 1 giây: In Throughput pps / Mbps & Protocols */
        if (now - last_1s_time >= 1000000000ULL) {
            double elapsed = (double)(now - last_1s_time) / 1e9;
            double rx_pps = (double)period_rx_pkts / elapsed;
            double rx_mbps = ((double)period_rx_bytes * 8.0) / (elapsed * 1e6);

            printf("[Pod1-Sink] RX: %.0f pps (%.2f Mbps) | Protocols: TCP: %" PRIu64 ", UDP: %" PRIu64 ", ICMP: %" PRIu64 " | Total: %" PRIu64 " pkts\n",
                   rx_pps, rx_mbps, period_tcp_pkts, period_udp_pkts, period_icmp_pkts, total_rx_pkts);
            print_port_hw_stats(port_id, "[Pod1]");
            fflush(stdout);

            period_rx_pkts = 0;
            period_rx_bytes = 0;
            period_tcp_pkts = 0;
            period_udp_pkts = 0;
            period_icmp_pkts = 0;
            period_other_pkts = 0;
            last_1s_time = now;
        }

        /* Chu kỳ 5 giây: In Top-5 Active Flows nhận được từ OVS */
        if (now - last_5s_time >= 5000000000ULL) {
            flow_tracker_print_top(&ft, 5, "[Pod1-RX]");
            flow_tracker_reset_period(&ft);
            last_5s_time = now;
        }
    }

    /* 5. Bảng tổng kết khi dừng ứng dụng */
    printf("\n=====================================================\n");
    printf("             Pod 1 Final Statistics Summary          \n");
    printf("=====================================================\n");
    printf("Total Received   : %" PRIu64 " pkts (%.2f MB)\n", total_rx_pkts, (double)total_rx_bytes / (1024.0 * 1024.0));
    printf("TCP Packets      : %" PRIu64 " (%.1f%%)\n", total_tcp_pkts, total_rx_pkts ? (double)total_tcp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("UDP Packets      : %" PRIu64 " (%.1f%%)\n", total_udp_pkts, total_rx_pkts ? (double)total_udp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("ICMP Packets     : %" PRIu64 " (%.1f%%)\n", total_icmp_pkts, total_rx_pkts ? (double)total_icmp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("Other Packets    : %" PRIu64 "\n", total_other_pkts);
    printf("Active Flow Keys : %u unique 5-tuples received\n", ft.active_flows);
    printf("=====================================================\n");

    flow_tracker_print_top(&ft, 10, "[Pod1-RX-FINAL]");

#ifdef LEGACY_L3_FILTER
    if (table) l3_table_free(table);
#endif

    cleanup_dpdk_subsystem(nb_ports);
    return 0;
}

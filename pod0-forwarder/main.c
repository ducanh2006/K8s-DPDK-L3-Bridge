#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <inttypes.h>

#include "dpdk_init.h"
#include "l3_table.h"
#include "pkt_utils.h"

#define BURST_SIZE 64

static volatile bool force_quit = false;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n[Pod0] Signal %d received, shutting down gracefully...\n", signum);
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
    printf("  K8s DPDK L3 Bridge: Pod 0 (PCAP Replayer & L3 Fwd) \n");
    printf("=====================================================\n");

    /* 1. Khởi tạo DPDK EAL và toàn bộ cổng mạng */
    uint16_t nb_ports = 0;
    int eal_consumed = 0;
    if (!init_dpdk_subsystem(argc, argv, &nb_ports, &eal_consumed)) {
        return 1;
    }

    if (nb_ports < 2) {
        printf("[Pod0] Warning: Found %u port(s). Need at least 2 ports for PCAP -> OVS forwarding!\n", nb_ports);
        printf("[Pod0] Port 0 will be used for both Ingress & Egress.\n");
    }

    uint16_t pcap_port = 0;
    uint16_t virtio_port = (nb_ports >= 2) ? 1 : 0;

    printf("[Pod0] Port Mapping: Port %u (PCAP Ingress) -> Port %u (OVS Virtio Egress)\n",
           pcap_port, virtio_port);

    /* 2. Đọc các tham số ứng dụng sau dấu '--' */
    int app_argc = argc - eal_consumed;
    char **app_argv = argv + eal_consumed;
    struct app_config cfg;
    if (parse_app_args(app_argc, app_argv, &cfg) < 0) {
        cleanup_dpdk_subsystem(nb_ports);
        return 1;
    }

    /* 3. Nạp bảng định tuyến Layer 3 */
    struct l3_table *table = l3_table_init("POD0_L3_TABLE", rte_socket_id());
    if (!table) {
        cleanup_dpdk_subsystem(nb_ports);
        return 1;
    }

    if (l3_table_load_file(table, cfg.config_path) < 0) {
        /* Thử tìm file tương đối nếu file tuyệt đối không tồn tại */
        if (l3_table_load_file(table, "pod0-forwarder/routes.conf") < 0) {
            printf("[Pod0] Warning: Could not load routes, applying built-in defaults\n");
            l3_table_add_route(table, "172.217.0.0/16", L3_ACTION_FORWARD);
            l3_table_add_route(table, "142.250.0.0/16", L3_ACTION_FORWARD);
            l3_table_add_route(table, "157.240.0.0/16", L3_ACTION_DROP);
            l3_table_add_route(table, "96.127.0.0/16", L3_ACTION_DROP);
            l3_table_add_route(table, "0.0.0.0/0", L3_ACTION_FORWARD);
        }
    }

    printf("[Pod0] Starting high-speed packet processing loop...\n");
    printf("-----------------------------------------------------\n");

    /* Các biến thống kê */
    uint64_t total_rx_pkts = 0;
    uint64_t total_rx_bytes = 0;
    uint64_t total_fwd_pkts = 0;
    uint64_t total_fwd_bytes = 0;
    uint64_t total_drop_pkts = 0;

    uint64_t period_rx_pkts = 0;
    uint64_t period_rx_bytes = 0;
    uint64_t period_fwd_pkts = 0;
    uint64_t period_drop_pkts = 0;

    uint64_t last_stat_time = get_current_time_ns();

    struct rte_mbuf *rx_pkts[BURST_SIZE];
    struct rte_mbuf *fwd_pkts[BURST_SIZE];

    /* 4. Vòng lặp chuyển tiếp gói tin chính */
    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(pcap_port, 0, rx_pkts, BURST_SIZE);

        if (nb_rx == 0) {
            /* Kiểm tra chu kỳ xuất thống kê ngay cả khi tạm thời không có gói */
            uint64_t now = get_current_time_ns();
            if (now - last_stat_time >= 1000000000ULL) {
                double elapsed_sec = (double)(now - last_stat_time) / 1e9;
                double rx_pps = (double)period_rx_pkts / elapsed_sec;
                double rx_mbps = ((double)period_rx_bytes * 8.0) / (elapsed_sec * 1e6);
                double fwd_pps = (double)period_fwd_pkts / elapsed_sec;
                double drop_pps = (double)period_drop_pkts / elapsed_sec;
                double drop_ratio = (period_rx_pkts > 0) ? ((double)period_drop_pkts * 100.0 / period_rx_pkts) : 0.0;

                printf("[Pod0] RX: %.0f pps (%.2f Mbps) | FWD: %.0f pps | DROP: %.0f pps (%.1f%%) | Total: %" PRIu64 " pkts\n",
                       rx_pps, rx_mbps, fwd_pps, drop_pps, drop_ratio, total_rx_pkts);
                fflush(stdout);

                period_rx_pkts = 0;
                period_rx_bytes = 0;
                period_fwd_pkts = 0;
                period_drop_pkts = 0;
                last_stat_time = now;
            }
            usleep(50);
            continue;
        }

        uint16_t fwd_cnt = 0;
        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = rx_pkts[i];
            period_rx_pkts++;
            total_rx_pkts++;
            period_rx_bytes += m->pkt_len;
            total_rx_bytes += m->pkt_len;

            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

            if (ether_type == RTE_ETHER_TYPE_IPV4) {
                struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((uint8_t *)eth + sizeof(struct rte_ether_hdr));
                l3_action_t act = l3_table_lookup(table, ip->dst_addr);

                if (act == L3_ACTION_FORWARD) {
                    fwd_pkts[fwd_cnt++] = m;
                    period_fwd_pkts++;
                    total_fwd_pkts++;
                    total_fwd_bytes += m->pkt_len;
                } else {
                    /* DROP gói tin tại chỗ theo luật L3 */
                    period_drop_pkts++;
                    total_drop_pkts++;
                    rte_pktmbuf_free(m);
                }
            } else {
                /* Gói không phải IPv4: DROP */
                period_drop_pkts++;
                total_drop_pkts++;
                rte_pktmbuf_free(m);
            }
        }

        /* Đẩy các gói FORWARD sang cổng OVS */
        if (fwd_cnt > 0) {
            uint16_t nb_tx = rte_eth_tx_burst(virtio_port, 0, fwd_pkts, fwd_cnt);
            for (uint16_t i = nb_tx; i < fwd_cnt; i++) {
                rte_pktmbuf_free(fwd_pkts[i]);
            }
        }

        /* Định kỳ xuất thống kê mỗi 1.0 giây */
        uint64_t now = get_current_time_ns();
        if (now - last_stat_time >= 1000000000ULL) {
            double elapsed_sec = (double)(now - last_stat_time) / 1e9;
            double rx_pps = (double)period_rx_pkts / elapsed_sec;
            double rx_mbps = ((double)period_rx_bytes * 8.0) / (elapsed_sec * 1e6);
            double fwd_pps = (double)period_fwd_pkts / elapsed_sec;
            double drop_pps = (double)period_drop_pkts / elapsed_sec;
            double drop_ratio = (period_rx_pkts > 0) ? ((double)period_drop_pkts * 100.0 / period_rx_pkts) : 0.0;

            printf("[Pod0] RX: %.0f pps (%.2f Mbps) | FWD: %.0f pps | DROP: %.0f pps (%.1f%%) | Total: %" PRIu64 " pkts\n",
                   rx_pps, rx_mbps, fwd_pps, drop_pps, drop_ratio, total_rx_pkts);
            fflush(stdout);

            period_rx_pkts = 0;
            period_rx_bytes = 0;
            period_fwd_pkts = 0;
            period_drop_pkts = 0;
            last_stat_time = now;
        }
    }

    printf("\n=====================================================\n");
    printf("             Pod 0 Final Statistics Summary          \n");
    printf("=====================================================\n");
    printf("Total PCAP Read : %" PRIu64 " pkts (%.2f MB)\n", total_rx_pkts, (double)total_rx_bytes / (1024.0 * 1024.0));
    printf("L3 Forwarded    : %" PRIu64 " pkts (%.2f MB)\n", total_fwd_pkts, (double)total_fwd_bytes / (1024.0 * 1024.0));
    printf("L3 Dropped      : %" PRIu64 " pkts\n", total_drop_pkts);
    double overall_drop = (total_rx_pkts > 0) ? ((double)total_drop_pkts * 100.0 / total_rx_pkts) : 0.0;
    printf("Overall Drop Rate: %.2f %%\n", overall_drop);
    printf("=====================================================\n");

    l3_table_free(table);
    cleanup_dpdk_subsystem(nb_ports);
    return 0;
}

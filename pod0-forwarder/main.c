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
    printf("  K8s DPDK L3 Bridge: Pod 0 (PCAP Streamer & Passthrough)\n");
    printf("  MODE: OVS_ROUTER/PASSTHROUGH (L3 Routing on vSwitch) \n");
    printf("=====================================================\n");

    /* 1. Khởi tạo DPDK EAL và toàn bộ cổng mạng */
    uint16_t nb_ports = 0;
    int eal_consumed = 0;
    if (!init_dpdk_subsystem(argc, argv, &nb_ports, &eal_consumed)) {
        return 1;
    }

    if (nb_ports < 2) {
        printf("[Pod0] Warning: Found %u port(s). Need 2 ports (PCAP Ingress + Virtio Egress)!\n", nb_ports);
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

#ifdef LEGACY_L3_FILTER
    printf("[Pod0] Note: Running with LEGACY_L3_FILTER enabled (App-level routing)\n");
    struct l3_table *table = l3_table_init("POD0_L3_TABLE", rte_socket_id());
    if (table) {
        l3_table_load_file(table, cfg.config_path);
    }
#else
    printf("[Pod0] Note: Transparent passthrough active. L3 Flow Table is offloaded to OVS-DPDK.\n");
#endif

    /* 3. Khởi tạo Flow Tracker theo dõi 5-tuple IP/Port */
    struct flow_tracker ft;
    flow_tracker_init(&ft, "[Pod0-TX]");

    printf("[Pod0] Starting high-speed packet forwarding loop...\n");
    printf("-----------------------------------------------------\n");

    /* Biến thống kê tổng thể và chu kỳ */
    uint64_t total_rx_pkts = 0;
    uint64_t total_rx_bytes = 0;
    uint64_t total_tx_pkts = 0;
    uint64_t total_tx_bytes = 0;
    uint64_t total_tx_dropped = 0;

    uint64_t period_rx_pkts = 0;
    uint64_t period_rx_bytes = 0;
    uint64_t period_tx_pkts = 0;
    uint64_t period_tx_bytes = 0;

    uint64_t last_1s_time = get_current_time_ns();
    uint64_t last_20s_time = last_1s_time;

    struct rte_mbuf *pkts[BURST_SIZE];

    /* 4. Vòng lặp chuyển tiếp gói tin chính */
    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(pcap_port, 0, pkts, BURST_SIZE);

        if (nb_rx > 0) {
            period_rx_pkts += nb_rx;
            total_rx_pkts += nb_rx;

            /* Phân tích và ghi nhận từng gói tin vào Flow Tracker (Zero-Copy) */
            for (uint16_t i = 0; i < nb_rx; i++) {
                period_rx_bytes += pkts[i]->pkt_len;
                total_rx_bytes += pkts[i]->pkt_len;
                flow_tracker_record(&ft, pkts[i]);
            }

            /* Chế độ PASSTHROUGH: Đẩy toàn bộ sang OVS-DPDK qua virtio-user */
            uint16_t nb_tx = rte_eth_tx_burst(virtio_port, 0, pkts, nb_rx);
            period_tx_pkts += nb_tx;
            total_tx_pkts += nb_tx;

            for (uint16_t i = 0; i < nb_tx; i++) {
                period_tx_bytes += pkts[i]->pkt_len;
                total_tx_bytes += pkts[i]->pkt_len;
            }

            /* Nếu hàng đợi TX đầy tạm thời, giải phóng gói sót để tránh rò rỉ mbuf */
            if (nb_tx < nb_rx) {
                total_tx_dropped += (nb_rx - nb_tx);
                for (uint16_t i = nb_tx; i < nb_rx; i++) {
                    rte_pktmbuf_free(pkts[i]);
                }
            }
        } else {
            usleep(50);
        }

        uint64_t now = get_current_time_ns();

        /* Chu kỳ 1 giây: In Throughput pps / Mbps & Port HW Stats */
        if (now - last_1s_time >= 1000000000ULL) {
            double elapsed = (double)(now - last_1s_time) / 1e9;
            double rx_pps = (double)period_rx_pkts / elapsed;
            double tx_pps = (double)period_tx_pkts / elapsed;
            double tx_mbps = ((double)period_tx_bytes * 8.0) / (elapsed * 1e6);

            printf("[Pod0] TX: %.0f pps (%.2f Mbps) | RX: %.0f pps | Total Sent: %" PRIu64 " pkts (%.2f MB)\n",
                   tx_pps, tx_mbps, rx_pps, total_tx_pkts, (double)total_tx_bytes / (1024.0 * 1024.0));
            print_port_hw_stats(virtio_port, "[Pod0]");
            fflush(stdout);

            period_rx_pkts = 0;
            period_rx_bytes = 0;
            period_tx_pkts = 0;
            period_tx_bytes = 0;
            last_1s_time = now;
        }

        /* Chu kỳ 20 giây: In Top-5 Active Flows (IP:Port & Mbps) */
        if (now - last_20s_time >= 20000000000ULL) {
            flow_tracker_print_top(&ft, 5, "[Pod0-TX]");
            flow_tracker_reset_period(&ft);
            last_20s_time = now;
        }
    }

    /* 5. Tổng kết toàn diện khi kết thúc */
    printf("\n=====================================================\n");
    printf("             Pod 0 Final Statistics Summary          \n");
    printf("=====================================================\n");
    printf("Total PCAP Read  : %" PRIu64 " pkts (%.2f MB)\n", total_rx_pkts, (double)total_rx_bytes / (1024.0 * 1024.0));
    printf("Total TX to OVS  : %" PRIu64 " pkts (%.2f MB)\n", total_tx_pkts, (double)total_tx_bytes / (1024.0 * 1024.0));
    printf("TX Ring Dropped  : %" PRIu64 " pkts\n", total_tx_dropped);
    printf("Active Flow Keys : %u unique 5-tuples\n", ft.active_flows);
    printf("=====================================================\n");

    flow_tracker_print_top(&ft, 10, "[Pod0-TX-FINAL]");

#ifdef LEGACY_L3_FILTER
    if (table) l3_table_free(table);
#endif

    cleanup_dpdk_subsystem(nb_ports);
    return 0;
}

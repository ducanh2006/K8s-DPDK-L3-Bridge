#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>

#include "dpdk_init.h"
#include "pkt_utils.h"
#include "hw_stats.h"
#include "flow_table.h"
#include "group_stats.h"

#define BURST_SIZE 64

static volatile bool force_quit = false;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n[Pod0] Signal %d received, shutting down gracefully...\n", signum);
        force_quit = true;
    }
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

    /* 0. Bóc tách tham số custom --rules-file trước khi chuyển argc/argv cho DPDK EAL */
    const char *rules_file = "/app/ovs_flows.conf";
    char **eal_argv = (char **)malloc((argc + 1) * sizeof(char *));
    if (!eal_argv) {
        fprintf(stderr, "[Pod0] Lỗi: Không thể cấp phát bộ nhớ cho eal_argv\n");
        return 1;
    }
    int eal_argc = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--rules-file") == 0 && i + 1 < argc) {
            rules_file = argv[++i];
        } else if (strncmp(argv[i], "--rules-file=", 13) == 0) {
            rules_file = argv[i] + 13;
        } else {
            eal_argv[eal_argc++] = argv[i];
        }
    }
    eal_argv[eal_argc] = NULL;

    /* 1. Khởi tạo DPDK EAL và toàn bộ cổng mạng */
    uint16_t nb_ports = 0;
    if (!init_dpdk_subsystem(eal_argc, eal_argv, &nb_ports, NULL)) {
        free(eal_argv);
        return 1;
    }
    free(eal_argv);

    if (nb_ports < 2) {
        printf("[Pod0] Warning: Found %u port(s). Need 2 ports (PCAP Ingress + Virtio Egress)!\n", nb_ports);
    }

    uint16_t pcap_port = 0;
    uint16_t virtio_port = (nb_ports >= 2) ? 1 : 0;

    printf("[Pod0] Port Mapping: Port %u (PCAP Ingress) -> Port %u (OVS Virtio Egress)\n",
           pcap_port, virtio_port);
    printf("[Pod0] Note: Transparent passthrough active. L3 Flow Table is offloaded to OVS-DPDK.\n");

    /* 2. Nạp bảng luật động từ cấu hình (Startup Slow-Path) */
    struct flow_table ft;
    if (flow_table_load(&ft, rules_file) != 0) {
        /* Fallback kiểm tra thư mục manifests nếu chạy dev / test offline ngoài container */
        if (flow_table_load(&ft, "manifests/ovs_flows.conf") != 0) {
            fprintf(stderr, "[Pod0] LỖI: Không thể nạp bảng luật từ '%s'\n", rules_file);
            cleanup_dpdk_subsystem(nb_ports);
            return 1;
        }
        rules_file = "manifests/ovs_flows.conf";
    }
    printf("[Pod0] Đã nạp thành công bảng luật từ '%s' (%zu groups, %zu rules)\n",
           rules_file, ft.num_groups, ft.num_rules);

    /* 3. Khởi tạo Group Stats Tracker theo dõi toàn bộ Groups */
    struct group_stats_tracker gs;
    group_stats_init(&gs, &ft, "[Pod0-GRP]");

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

    /* 4. Vòng lặp chuyển tiếp gói tin chính (Fast-Path) */
    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(pcap_port, 0, pkts, BURST_SIZE);

        if (nb_rx > 0) {
            period_rx_pkts += nb_rx;
            total_rx_pkts += nb_rx;

            /* Đếm RX trước khi đẩy (offered load, kể cả gói rớt ring sau này) */
            for (uint16_t i = 0; i < nb_rx; i++) {
                period_rx_bytes += pkts[i]->pkt_len;
                total_rx_bytes += pkts[i]->pkt_len;
            }

            /* Chế độ PASSTHROUGH: Đẩy toàn bộ sang OVS-DPDK qua virtio-user */
            uint16_t nb_tx = rte_eth_tx_burst(virtio_port, 0, pkts, nb_rx);
            period_tx_pkts += nb_tx;
            total_tx_pkts += nb_tx;

            /* Ghi nhận groups CHỈ cho nb_tx gói đẩy thành công (admitted-side,
               khớp 1-1 với counters OVS để đối chứng E2E) */
            for (uint16_t i = 0; i < nb_tx; i++) {
                period_tx_bytes += pkts[i]->pkt_len;
                total_tx_bytes += pkts[i]->pkt_len;
                group_stats_record(&gs, &ft, pkts[i]);
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

        /* Chu kỳ 20 giây: In bảng thống kê Groups (SSOT) */
        if (now - last_20s_time >= 20000000000ULL) {
            double elapsed = (double)(now - last_20s_time) / 1e9;
            group_stats_print_table(&gs, &ft, "[Pod0-GRP]", elapsed);
            group_stats_reset_period(&gs);
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
    printf("Non-IPv4 Packets : %" PRIu64 "\n", gs.non_ip_pkts);
    printf("=====================================================\n");

    group_stats_print_table(&gs, &ft, "[Pod0-GRP-FINAL]", 0.0);

    group_stats_free(&gs);
    flow_table_free(&ft);
    cleanup_dpdk_subsystem(nb_ports);
    return 0;
}

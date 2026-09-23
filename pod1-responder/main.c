#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>

#include "dpdk_init.h"
#include "pkt_utils.h"
#include "hw_stats.h"
#include "group_stats.h"

#define BURST_SIZE 64

/* Độ dài tối thiểu để đọc an toàn Ethernet + IPv4 header cố định */
#define MIN_L3_PARSE_LEN (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr))

static volatile bool force_quit = false;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n[Pod1] Signal %d received, shutting down gracefully...\n", signum);
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
    printf("  K8s DPDK L3 Bridge: Pod 1 (Traffic Sink & Inspector)\n");
    printf("  MODE: RECEIVE_FROM_OVS (Verifying routed flows)     \n");
    printf("=====================================================\n");

    /* 1. Khởi tạo DPDK EAL và cổng mạng virtio-user */
    uint16_t nb_ports = 0;
    if (!init_dpdk_subsystem(argc, argv, &nb_ports, NULL)) {
        return 1;
    }

    uint16_t port_id = 0;
    printf("[Pod1] Listening on Port %u (Virtio-User from OVS)...\n", port_id);
    printf("[Pod1] Note: Pure Sink & Flow Inspector mode. All filtering was done by OVS-DPDK.\n");

    /* 2. Khởi tạo Group Stats Tracker theo dõi 8 groups */
    struct group_stats_tracker gs;
    group_stats_init(&gs, "[Pod1-GRP]");

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
    uint64_t last_20s_time = last_1s_time;

    struct rte_mbuf *rx_pkts[BURST_SIZE];

    /* 3. Vòng lặp nhận và phân tích gói tin */
    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_pkts, BURST_SIZE);

        if (nb_rx > 0) {
            period_rx_pkts += nb_rx;
            total_rx_pkts += nb_rx;

            for (uint16_t i = 0; i < nb_rx; i++) {
                struct rte_mbuf *m = rx_pkts[i];
                period_rx_bytes += m->pkt_len;
                total_rx_bytes += m->pkt_len;

                /* Ghi nhận vào bảng Group Stats Tracker (hàm tự đếm non-IP) */
                group_stats_record(&gs, m);

                /* Phân tích protocol header (chỉ khi đủ dài để đọc an toàn) */
                if (m->pkt_len >= MIN_L3_PARSE_LEN) {
                    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                    if (rte_be_to_cpu_16(eth->ether_type) == RTE_ETHER_TYPE_IPV4) {
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
                    } else {
                        period_other_pkts++;
                        total_other_pkts++;
                    }
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

            printf("[Pod1-Sink] RX: %.0f pps (%.2f Mbps) | Protocols: TCP: %" PRIu64 ", UDP: %" PRIu64 ", ICMP: %" PRIu64 ", Other: %" PRIu64 " | Total: %" PRIu64 " pkts\n",
                   rx_pps, rx_mbps, period_tcp_pkts, period_udp_pkts, period_icmp_pkts, period_other_pkts, total_rx_pkts);
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

        /* Chu kỳ 20 giây: In bảng thống kê 8 Groups nhận được từ OVS */
        if (now - last_20s_time >= 20000000000ULL) {
            double elapsed = (double)(now - last_20s_time) / 1e9;
            group_stats_print_table(&gs, "[Pod1-GRP]", elapsed);
            group_stats_reset_period(&gs);
            last_20s_time = now;
        }
    }

    /* 4. Bảng tổng kết khi dừng ứng dụng */
    printf("\n=====================================================\n");
    printf("             Pod 1 Final Statistics Summary          \n");
    printf("=====================================================\n");
    printf("Total Received   : %" PRIu64 " pkts (%.2f MB)\n", total_rx_pkts, (double)total_rx_bytes / (1024.0 * 1024.0));
    printf("TCP Packets      : %" PRIu64 " (%.1f%%)\n", total_tcp_pkts, total_rx_pkts ? (double)total_tcp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("UDP Packets      : %" PRIu64 " (%.1f%%)\n", total_udp_pkts, total_rx_pkts ? (double)total_udp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("ICMP Packets     : %" PRIu64 " (%.1f%%)\n", total_icmp_pkts, total_rx_pkts ? (double)total_icmp_pkts * 100.0 / total_rx_pkts : 0.0);
    printf("Other Packets    : %" PRIu64 "\n", total_other_pkts);
    printf("Non-IPv4 Packets : %" PRIu64 "\n", gs.non_ip_pkts);
    printf("=====================================================\n");

    group_stats_print_table(&gs, "[Pod1-GRP-FINAL]", 0.0);

    cleanup_dpdk_subsystem(nb_ports);
    return 0;
}

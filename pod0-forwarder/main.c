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
        printf("\n[Pod0] Signal %d received, preparing to exit...\n", signum);
        force_quit = true;
    }
}

/* Cấu hình mặc định của Pod 0 */
struct app_config {
    char config_path[256];
    char src_ip[32];
    char dst_ip[32];
    uint32_t interval_ms;
    uint32_t count;
};

static void print_usage(const char *prgname)
{
    printf("Usage: %s [EAL options] -- [APP options]\n"
           "APP Options:\n"
           "  --config <path>    Đường dẫn file bảng luật L3 (mặc định: pod0-forwarder/routes.conf)\n"
           "  --src-ip <ip>      Địa chỉ IP nguồn của Pod 0 (mặc định: 10.0.0.1)\n"
           "  --dst-ip <ip>      Địa chỉ IP đích kiểm thử (mặc định: 10.0.0.2)\n"
           "  --interval <ms>    Khoảng cách gửi gói tin tính bằng ms (mặc định: 1000)\n"
           "  --count <n>        Số gói tin cần gửi (0 = không giới hạn, mặc định: 0)\n",
           prgname);
}

static int parse_app_args(int argc, char **argv, struct app_config *cfg)
{
    /* Giá trị mặc định */
    snprintf(cfg->config_path, sizeof(cfg->config_path), "pod0-forwarder/routes.conf");
    snprintf(cfg->src_ip, sizeof(cfg->src_ip), "10.0.0.1");
    snprintf(cfg->dst_ip, sizeof(cfg->dst_ip), "10.0.0.2");
    cfg->interval_ms = 1000;
    cfg->count = 0;

    static struct option lgopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"src-ip", required_argument, NULL, 's'},
        {"dst-ip", required_argument, NULL, 'd'},
        {"interval", required_argument, NULL, 'i'},
        {"count", required_argument, NULL, 'n'},
        {NULL, 0, NULL, 0}
    };

    int opt, opt_idx;
    while ((opt = getopt_long(argc, argv, "c:s:d:i:n:", lgopts, &opt_idx)) != EOF) {
        switch (opt) {
        case 'c':
            snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", optarg);
            break;
        case 's':
            snprintf(cfg->src_ip, sizeof(cfg->src_ip), "%s", optarg);
            break;
        case 'd':
            snprintf(cfg->dst_ip, sizeof(cfg->dst_ip), "%s", optarg);
            break;
        case 'i':
            cfg->interval_ms = (uint32_t)atoi(optarg);
            break;
        case 'n':
            cfg->count = (uint32_t)atoi(optarg);
            break;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/* Hàm tạo một gói tin kiểm thử UDP */
static struct rte_mbuf *craft_test_packet(struct rte_mempool *pool,
                                         const struct app_config *cfg,
                                         uint32_t seq,
                                         struct rte_ether_addr *src_mac)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m) {
        fprintf(stderr, "[Pod0] Error: Failed to allocate mbuf from mempool\n");
        return NULL;
    }

    size_t pkt_size = sizeof(struct rte_ether_hdr) +
                      sizeof(struct rte_ipv4_hdr) +
                      sizeof(struct rte_udp_hdr) +
                      sizeof(struct test_payload);

    m->pkt_len = pkt_size;
    m->data_len = pkt_size;

    /* 1. Ethernet Header */
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    eth->src_addr = *src_mac;
    /* Gán MAC đích giả định của Pod 1 hoặc broadcast */
    eth->dst_addr.addr_bytes[0] = 0x02;
    eth->dst_addr.addr_bytes[1] = 0x00;
    eth->dst_addr.addr_bytes[2] = 0x00;
    eth->dst_addr.addr_bytes[3] = 0x00;
    eth->dst_addr.addr_bytes[4] = 0x00;
    eth->dst_addr.addr_bytes[5] = 0x02;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* 2. IPv4 Header */
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((uint8_t *)eth + sizeof(struct rte_ether_hdr));
    ip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
    ip->type_of_service = 0;
    ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct test_payload));
    ip->packet_id = rte_cpu_to_be_16(seq);
    ip->fragment_offset = 0;
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = inet_addr(cfg->src_ip);
    ip->dst_addr = inet_addr(cfg->dst_ip);
    recalculate_ipv4_checksum(ip);

    /* 3. UDP Header */
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + sizeof(struct rte_ipv4_hdr));
    udp->src_port = rte_cpu_to_be_16(TEST_UDP_PORT);
    udp->dst_port = rte_cpu_to_be_16(TEST_UDP_PORT);
    udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + sizeof(struct test_payload));
    udp->dgram_cksum = 0;

    /* 4. Test Payload */
    struct test_payload *payload = (struct test_payload *)((uint8_t *)udp + sizeof(struct rte_udp_hdr));
    payload->magic = PING_MAGIC;
    payload->seq = seq;
    payload->timestamp_ns = get_current_time_ns();
    snprintf(payload->message, sizeof(payload->message), "PING_REQ_#%u", seq);

    return m;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=====================================================\n");
    printf("   K8s DPDK L3 Bridge: Pod 0 (Forwarder / Sender)   \n");
    printf("=====================================================\n");

    /* 1. Khởi tạo hạ tầng DPDK */
    uint16_t port_id = 0;
    int eal_consumed = 0;
    struct rte_mempool *pool = init_dpdk_subsystem(argc, argv, &port_id, &eal_consumed);
    if (!pool) {
        return 1;
    }

    /* 2. Đọc các tham số ứng dụng sau dấu '--' */
    int app_argc = argc - eal_consumed;
    char **app_argv = argv + eal_consumed;
    struct app_config cfg;
    if (parse_app_args(app_argc, app_argv, &cfg) < 0) {
        cleanup_dpdk_subsystem(port_id);
        return 1;
    }

    /* 3. Khởi tạo bảng luật L3 */
    struct l3_table *table = l3_table_init("POD0_L3_TABLE", rte_socket_id());
    if (!table) {
        cleanup_dpdk_subsystem(port_id);
        return 1;
    }

    if (l3_table_load_file(table, cfg.config_path) < 0) {
        printf("[Pod0] Warning: Could not load '%s', using default rules\n", cfg.config_path);
        l3_table_add_route(table, "10.0.0.0/24", L3_ACTION_FORWARD);
        l3_table_add_route(table, "0.0.0.0/0", L3_ACTION_DROP);
    }

    struct rte_ether_addr my_mac;
    rte_eth_macaddr_get(port_id, &my_mac);

    printf("[Pod0] Configuration:\n");
    printf("       - Route File: %s\n", cfg.config_path);
    printf("       - Test Stream: %s -> %s\n", cfg.src_ip, cfg.dst_ip);
    printf("       - Interval: %u ms | Total Packets: %s\n",
           cfg.interval_ms, cfg.count > 0 ? "Limited" : "Infinite");
    printf("-----------------------------------------------------\n");

    uint32_t seq = 0;
    uint32_t tx_forwarded = 0;
    uint32_t tx_dropped_l3 = 0;
    uint32_t rx_ack_count = 0;

    uint64_t last_send_time = 0;

    /* 4. Vòng lặp chính */
    while (!force_quit) {
        uint64_t now = get_current_time_ns();

        /* TX Path: Định kỳ sinh gói và kiểm tra bảng L3 */
        if (now - last_send_time >= (uint64_t)cfg.interval_ms * 1000000ULL) {
            last_send_time = now;

            if (cfg.count == 0 || seq < cfg.count) {
                seq++;
                struct rte_mbuf *pkt = craft_test_packet(pool, &cfg, seq, &my_mac);
                if (pkt) {
                    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(pkt, uint8_t *) + sizeof(struct rte_ether_hdr));
                    
                    /* Tra cứu bảng chuyển mạch tĩnh Layer 3 */
                    l3_action_t act = l3_table_lookup(table, ip->dst_addr);

                    if (act == L3_ACTION_FORWARD) {
                        uint16_t sent = rte_eth_tx_burst(port_id, 0, &pkt, 1);
                        if (sent == 1) {
                            tx_forwarded++;
                            print_packet_log("Pod0", "FORWARD/TX", ip->src_addr, ip->dst_addr, seq, "PING Sent to OVS");
                        } else {
                            rte_pktmbuf_free(pkt);
                            fprintf(stderr, "[Pod0] [TX_FAIL] Packet #%u dropped by NIC queue\n", seq);
                        }
                    } else {
                        /* DROP gói tin tại chỗ theo luật L3 */
                        tx_dropped_l3++;
                        char dst_str[INET_ADDRSTRLEN];
                        ip_to_str(ip->dst_addr, dst_str, sizeof(dst_str));
                        printf("[Pod0] [DROP] Packet #%u to %s denied by L3 table -> Dropped\n", seq, dst_str);
                        rte_pktmbuf_free(pkt);
                    }
                }
            }
        }

        /* RX Path: Lắng nghe gói tin phản hồi (ACK) từ Pod 1 */
        struct rte_mbuf *rx_pkts[32];
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_pkts, 32);

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = rx_pkts[i];
            struct rte_ether_hdr *eth = NULL;
            struct rte_ipv4_hdr *ip = NULL;
            struct rte_udp_hdr *udp = NULL;
            struct test_payload *payload = NULL;

            if (parse_ipv4_packet(m, &eth, &ip, &udp, &payload) && payload) {
                if (payload->magic == ACK_MAGIC) {
                    rx_ack_count++;
                    uint64_t rtt_ns = now - payload->timestamp_ns;
                    double rtt_ms = (double)rtt_ns / 1000000.0;

                    char src_str[INET_ADDRSTRLEN];
                    ip_to_str(ip->src_addr, src_str, sizeof(src_str));
                    printf("\033[1;32m[Pod0] [SUCCESS/ACK] Received ACK #%u from %s | RTT: %.3f ms | Msg: \"%s\"\033[0m\n",
                           payload->seq, src_str, rtt_ms, payload->message);
                    fflush(stdout);
                }
            }
            rte_pktmbuf_free(m);
        }

        /* Tạm dừng nhẹ để tiết kiệm CPU khi test */
        usleep(1000); /* 1ms */
    }

    printf("\n=====================================================\n");
    printf("             Pod 0 Statistics Summary                \n");
    printf("=====================================================\n");
    printf("Total Generated : %u\n", seq);
    printf("L3 Forwarded    : %u\n", tx_forwarded);
    printf("L3 Dropped      : %u\n", tx_dropped_l3);
    printf("ACK Received    : %u\n", rx_ack_count);
    printf("=====================================================\n");

    l3_table_free(table);
    cleanup_dpdk_subsystem(port_id);
    return 0;
}

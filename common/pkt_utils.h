#ifndef PKT_UTILS_H
#define PKT_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <arpa/inet.h>

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>

#define PING_MAGIC 0x50494E47 /* 'PING' */
#define ACK_MAGIC  0x41434B21 /* 'ACK!' */
#define TEST_UDP_PORT 12345

/* Payload cấu trúc cho gói tin kiểm thử hai chiều */
struct test_payload {
    uint32_t magic;         /* PING_MAGIC hoặc ACK_MAGIC */
    uint32_t seq;           /* Sequence number */
    uint64_t timestamp_ns;  /* Thời điểm phát gói (nanoseconds) */
    char message[32];       /* Chuỗi thông điệp kiểm thử */
} __attribute__((packed));

/* Chuyển địa chỉ IPv4 (network byte order) thành chuỗi x.x.x.x */
static inline void ip_to_str(rte_be32_t ip, char *buf, size_t len)
{
    struct in_addr addr;
    addr.s_addr = ip;
    inet_ntop(AF_INET, &addr, buf, len);
}

/* Lấy thời gian hiện tại tính bằng nano giây */
static inline uint64_t get_current_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Bóc tách các header Ethernet, IPv4, UDP và Payload từ mbuf */
static inline bool parse_ipv4_packet(struct rte_mbuf *m,
                                     struct rte_ether_hdr **eth_hdr,
                                     struct rte_ipv4_hdr **ip_hdr,
                                     struct rte_udp_hdr **udp_hdr,
                                     struct test_payload **payload)
{
    if (unlikely(m == NULL || rte_pktmbuf_pkt_len(m) < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr))) {
        return false;
    }

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);
    size_t l3_offset = sizeof(struct rte_ether_hdr);

    /* Xử lý VLAN nếu có */
    if (unlikely(ether_type == RTE_ETHER_TYPE_VLAN)) {
        struct rte_vlan_hdr *vlan = (struct rte_vlan_hdr *)((uint8_t *)eth + l3_offset);
        ether_type = rte_be_to_cpu_16(vlan->eth_proto);
        l3_offset += sizeof(struct rte_vlan_hdr);
    }

    if (unlikely(ether_type != RTE_ETHER_TYPE_IPV4)) {
        return false;
    }

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((uint8_t *)eth + l3_offset);
    uint16_t ip_len = (ip->version_ihl & RTE_IPV4_HDR_IHL_MASK) * RTE_IPV4_IHL_MULTIPLIER;
    size_t l4_offset = l3_offset + ip_len;

    if (eth_hdr) *eth_hdr = eth;
    if (ip_hdr) *ip_hdr = ip;

    /* Kiểm tra nếu là UDP */
    if (ip->next_proto_id == IPPROTO_UDP &&
        rte_pktmbuf_pkt_len(m) >= l4_offset + sizeof(struct rte_udp_hdr)) {
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)eth + l4_offset);
        if (udp_hdr) *udp_hdr = udp;

        size_t payload_offset = l4_offset + sizeof(struct rte_udp_hdr);
        if (payload && rte_pktmbuf_pkt_len(m) >= payload_offset + sizeof(struct test_payload)) {
            *payload = (struct test_payload *)((uint8_t *)eth + payload_offset);
        } else if (payload) {
            *payload = NULL;
        }
    } else {
        if (udp_hdr) *udp_hdr = NULL;
        if (payload) *payload = NULL;
    }

    return true;
}

/* Hoán đổi MAC và IP nguồn/đích cho gói phản hồi ACK */
static inline void swap_l2_l3_addresses(struct rte_ether_hdr *eth, struct rte_ipv4_hdr *ip, struct rte_udp_hdr *udp)
{
    /* Đổi MAC */
    struct rte_ether_addr tmp_mac = eth->src_addr;
    eth->src_addr = eth->dst_addr;
    eth->dst_addr = tmp_mac;

    /* Đổi IP */
    rte_be32_t tmp_ip = ip->src_addr;
    ip->src_addr = ip->dst_addr;
    ip->dst_addr = tmp_ip;

    /* Đổi UDP port nếu có */
    if (udp) {
        rte_be16_t tmp_port = udp->src_port;
        udp->src_port = udp->dst_port;
        udp->dst_port = tmp_port;
        udp->dgram_cksum = 0; /* UDP checksum optional */
    }
}

/* Tính toán lại Checksum cho IPv4 header */
static inline void recalculate_ipv4_checksum(struct rte_ipv4_hdr *ip)
{
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);
}

/* In log thông tin gói tin */
static inline void print_packet_log(const char *role, const char *action,
                                   rte_be32_t src_ip, rte_be32_t dst_ip,
                                   uint32_t seq, const char *msg)
{
    char src_str[INET_ADDRSTRLEN];
    char dst_str[INET_ADDRSTRLEN];
    ip_to_str(src_ip, src_str, sizeof(src_str));
    ip_to_str(dst_ip, dst_str, sizeof(dst_str));

    printf("[%s] [%s] %s -> %s | Seq: #%u | Msg: \"%s\"\n",
           role, action, src_str, dst_str, seq, msg);
    fflush(stdout);
}

#endif /* PKT_UTILS_H */

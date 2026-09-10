#ifndef PKT_UTILS_H
#define PKT_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <arpa/inet.h>

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_byteorder.h>

/**
 * Chuyển địa chỉ IPv4 (network byte order) thành chuỗi x.x.x.x
 */
static inline void ip_to_str(rte_be32_t ip, char *buf, size_t len)
{
    struct in_addr addr;
    addr.s_addr = ip;
    inet_ntop(AF_INET, &addr, buf, len);
}

/**
 * Lấy thời gian hiện tại tính bằng nano giây (đồng hồ Monotonic độ chính xác cao)
 */
static inline uint64_t get_current_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Tính toán lại Checksum cho IPv4 header
 */
static inline void recalculate_ipv4_checksum(struct rte_ipv4_hdr *ip)
{
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);
}

#endif /* PKT_UTILS_H */

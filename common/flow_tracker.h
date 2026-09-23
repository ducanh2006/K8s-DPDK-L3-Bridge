#ifndef FLOW_TRACKER_H
#define FLOW_TRACKER_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_ethdev.h>

#define FLOW_TABLE_SIZE 1024
#define FLOW_HASH_MASK (FLOW_TABLE_SIZE - 1)

/**
 * Khóa nhận dạng luồng mạng 5-tuple
 */
struct flow_key {
    rte_be32_t src_ip;
    rte_be32_t dst_ip;
    rte_be16_t src_port;
    rte_be16_t dst_port;
    uint8_t    proto;
};

/**
 * Một bản ghi theo dõi luồng mạng
 */
struct flow_entry {
    struct flow_key key;
    bool     is_active;
    uint64_t total_pkts;
    uint64_t total_bytes;
    uint64_t period_pkts;
    uint64_t period_bytes;
    uint64_t last_seen_ns;
};

/**
 * Bộ theo dõi luồng mạng hiệu năng cao cho DPDK Fast-Path
 */
struct flow_tracker {
    char name[32];
    struct flow_entry entries[FLOW_TABLE_SIZE];
    uint32_t active_flows;
    uint64_t total_tracked_pkts;
    uint64_t total_tracked_bytes;
};

/**
 * Khởi tạo bộ theo dõi luồng mạng
 */
void flow_tracker_init(struct flow_tracker *ft, const char *name);

/**
 * Ghi nhận gói tin mbuf vào bộ theo dõi (Zero-Copy fast path)
 */
void flow_tracker_record(struct flow_tracker *ft, struct rte_mbuf *m);

/**
 * In danh sách Top N luồng mạng hoạt động tích cực nhất
 * @param ft Con trỏ bộ theo dõi luồng
 * @param top_n Số lượng dòng hiển thị (ví dụ 5)
 * @param tag Nhãn log định danh (ví dụ "[Pod0-TX]" hoặc "[Pod1-RX]")
 */
void flow_tracker_print_top(struct flow_tracker *ft, int top_n, const char *tag);

/**
 * Reset bộ đếm chu kỳ (được gọi sau mỗi chu kỳ 5 giây)
 */
void flow_tracker_reset_period(struct flow_tracker *ft);

/**
 * In thống kê phần cứng / PMD NIC port (imissed, ierrors, oerrors)
 */
void print_port_hw_stats(uint16_t port_id, const char *tag);

#endif /* FLOW_TRACKER_H */

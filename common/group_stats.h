#ifndef GROUP_STATS_H
#define GROUP_STATS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "flow_table.h"

/**
 * Bộ đếm gói tin và dung lượng cho từng Group
 */
struct group_counter {
    uint64_t period_pkts;
    uint64_t period_bytes;
    uint64_t total_pkts;
};

/**
 * Trình theo dõi thống kê toàn bộ Groups được nạp động
 */
struct group_stats_tracker {
    struct group_counter *groups;
    size_t                num_groups;
    uint64_t              non_ip_pkts;
};

/**
 * Khởi tạo bộ thống kê Group Stats dựa trên bảng luật động
 */
void group_stats_init(struct group_stats_tracker *gs, const struct flow_table *ft, const char *name);

/**
 * Giải phóng bộ nhớ động của tracker
 */
void group_stats_free(struct group_stats_tracker *gs);

/**
 * Phân loại và ghi nhận gói tin mbuf vào Group tương ứng (Zero-Copy fast-path)
 */
void group_stats_record(struct group_stats_tracker *gs, const struct flow_table *ft, struct rte_mbuf *m);

/**
 * In bảng thống kê Groups theo định dạng chuẩn đẹp
 * @param gs Con trỏ bộ theo dõi
 * @param ft Con trỏ bảng luật chứa metadata tên group/action/prio
 * @param tag Nhãn log (ví dụ "[Pod0-GRP]" hoặc "[Pod1-GRP]")
 * @param elapsed_sec Số giây chu kỳ để tính Mbps
 */
void group_stats_print_table(const struct group_stats_tracker *gs, const struct flow_table *ft, const char *tag, double elapsed_sec);

/**
 * Reset bộ đếm chu kỳ (được gọi sau mỗi chu kỳ 20 giây)
 */
void group_stats_reset_period(struct group_stats_tracker *gs);

#endif /* GROUP_STATS_H */

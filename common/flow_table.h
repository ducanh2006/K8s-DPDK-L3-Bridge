#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <rte_byteorder.h>
#include <rte_ip.h>

/**
 * Cấu trúc thông tin của một Group (khớp với [GROUPS_SECTION] trong ovs_flows.conf)
 */
struct flow_group {
    char        name[64];
    uint32_t    priority;
    char        action[16];   /* "DROP" hoặc "FORWARD" */
    bool        is_drop;
};

/**
 * Cấu trúc thông tin của một Filter Rule (khớp với [FILTERS_SECTION] trong ovs_flows.conf)
 * Toàn bộ các trường IP/Mask/Port được lưu trữ theo Network Byte Order (Big-Endian)
 * để vòng lặp Fast-Path so khớp trực tiếp với DPDK packet headers mà không cần hoán đổi byte.
 */
struct flow_rule {
    char        rule_name[64];
    uint32_t    group_idx;    /* Chỉ số index của Group trong mảng ft->groups */
    uint32_t    priority;     /* Độ ưu tiên kế thừa từ Group */
    uint8_t     proto;        /* 0: any, IPPROTO_TCP (6), IPPROTO_UDP (17), IPPROTO_ICMP (1) */
    rte_be32_t  dst_net;      /* Địa chỉ IP đích dạng mạng (Big-Endian) */
    rte_be32_t  dst_mask;     /* Subnet mask đích (Big-Endian) */
    rte_be32_t  src_net;      /* Địa chỉ IP nguồn dạng mạng (Big-Endian) */
    rte_be32_t  src_mask;     /* Subnet mask nguồn (Big-Endian) */
    rte_be16_t  dst_port;     /* Port đích (Big-Endian) */
    rte_be16_t  src_port;     /* Port nguồn (Big-Endian) */
};

/**
 * Bảng luật định tuyến và phân loại lưu lượng hoàn chỉnh trong RAM
 */
struct flow_table {
    size_t              num_groups;
    struct flow_group  *groups;
    size_t              num_rules;
    struct flow_rule   *rules;
};

/**
 * Nạp bảng luật từ file cấu hình text (ovs_flows.conf) vào RAM.
 * Sử dụng giải thuật 2-Pass an toàn (Pass 1 đếm số lượng, Pass 2 nạp dữ liệu).
 * Sau đó sắp xếp các luật theo thứ tự priority giảm dần (First-match wins).
 *
 * @param ft Con trỏ cấu trúc flow_table cần khởi tạo
 * @param conf_path Đường dẫn đến file cấu hình ovs_flows.conf
 * @return 0 nếu thành công, -1 nếu thất bại (kèm log lỗi ra stderr)
 */
int flow_table_load(struct flow_table *ft, const char *conf_path);

/**
 * Giải phóng bộ nhớ động của flow_table
 * @param ft Con trỏ cấu trúc flow_table
 */
void flow_table_free(struct flow_table *ft);

/**
 * In tóm tắt nội dung bảng luật ra stdout (hỗ trợ debug)
 * @param ft Con trỏ cấu trúc flow_table
 */
void flow_table_dump(const struct flow_table *ft);

#endif /* FLOW_TABLE_H */

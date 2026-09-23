#ifndef L3_TABLE_H
#define L3_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_ip.h>

typedef enum {
    L3_ACTION_DROP = 0,
    L3_ACTION_FORWARD = 1
} l3_action_t;

struct l3_table;

/**
 * Khởi tạo bảng tra cứu L3 LPM
 * @param name Tên định danh cho bảng LPM
 * @param socket_id NUMA socket ID
 * @return Con trỏ struct l3_table hoặc NULL nếu lỗi
 */
struct l3_table *l3_table_init(const char *name, uint32_t socket_id);

/**
 * Thêm một luật chuyển mạch vào bảng
 * @param table Con trỏ bảng L3
 * @param ip_cidr Chuỗi định dạng CIDR, ví dụ "10.0.0.0/24"
 * @param action L3_ACTION_FORWARD hoặc L3_ACTION_DROP
 * @return 0 nếu thành công, < 0 nếu lỗi
 */
int l3_table_add_route(struct l3_table *table, const char *ip_cidr, l3_action_t action);

/**
 * (Legacy) Nạp danh sách luật từ file cấu hình routes.conf
 * @note Hệ thống chính thức hiện dùng manifests/ovs_flows.conf và group_stats
 * @param table Con trỏ bảng L3
 * @param conf_path Đường dẫn tới file cấu hình
 * @return Số lượng luật đã nạp thành công, hoặc < 0 nếu file không hợp lệ
 */
int l3_table_load_file(struct l3_table *table, const char *conf_path);

/**
 * Tra cứu hành vi L3 theo IP đích
 * @param table Con trỏ bảng L3
 * @param dst_ip Địa chỉ IP đích (network byte order)
 * @return L3_ACTION_FORWARD hoặc L3_ACTION_DROP
 */
l3_action_t l3_table_lookup(struct l3_table *table, rte_be32_t dst_ip);

/**
 * Giải phóng bộ nhớ bảng L3
 * @param table Con trỏ bảng L3
 */
void l3_table_free(struct l3_table *table);

#endif /* L3_TABLE_H */

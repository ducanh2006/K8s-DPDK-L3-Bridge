#ifndef DPDK_INIT_H
#define DPDK_INIT_H

#include <stdint.h>
#include <rte_mempool.h>
#include <rte_ethdev.h>

#define NUM_MBUFS 32767
#define MBUF_CACHE_SIZE 256
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

/**
 * Khởi tạo toàn bộ hệ thống DPDK và cấu hình tất cả các cổng mạng khả dụng
 * @param argc Số lượng đối số dòng lệnh
 * @param argv Mảng các đối số dòng lệnh
 * @param nb_ports [Out] Số lượng cổng DPDK Ethernet được cấu hình thành công
 * @param eal_consumed [Out] Số lượng đối số dòng lệnh mà EAL đã xử lý
 * @return Con trỏ rte_mempool nếu thành công, NULL nếu thất bại
 */
struct rte_mempool *init_dpdk_subsystem(int argc, char **argv, uint16_t *nb_ports, int *eal_consumed);

/**
 * Đóng và giải phóng tài nguyên toàn bộ các cổng mạng DPDK
 * @param nb_ports Số lượng cổng cần giải phóng
 */
void cleanup_dpdk_subsystem(uint16_t nb_ports);

#endif /* DPDK_INIT_H */

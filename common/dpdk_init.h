#ifndef DPDK_INIT_H
#define DPDK_INIT_H

#include <stdint.h>
#include <rte_mempool.h>
#include <rte_ethdev.h>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 256
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

/**
 * Khởi tạo toàn bộ hệ thống DPDK: EAL, Mempool, Port (Virtio-user) và Queues
 * @param argc Số lượng đối số dòng lệnh
 * @param argv Mảng các đối số dòng lệnh
 * @param port_id [Out] ID của cổng DPDK Ethernet vừa khởi tạo
 * @param eal_consumed [Out] Số lượng đối số dòng lệnh mà EAL đã xử lý
 * @return Con trỏ rte_mempool nếu thành công, NULL nếu thất bại
 */
struct rte_mempool *init_dpdk_subsystem(int argc, char **argv, uint16_t *port_id, int *eal_consumed);

/**
 * Đóng và giải phóng tài nguyên cổng mạng DPDK
 * @param port_id ID của cổng Ethernet
 */
void cleanup_dpdk_subsystem(uint16_t port_id);

#endif /* DPDK_INIT_H */

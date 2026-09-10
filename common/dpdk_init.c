#include "dpdk_init.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_ether.h>

struct rte_mempool *init_dpdk_subsystem(int argc, char **argv, uint16_t *port_id, int *eal_consumed)
{
    /* 1. Khởi tạo EAL */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: rte_eal_init failed (ret: %d)\n", ret);
    }
    if (eal_consumed) {
        *eal_consumed = ret;
    }

    /* 2. Kiểm tra các port có sẵn */
    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: No Ethernet ports available! Please check --vdev argument.\n");
    }

    /* Chọn port 0 (thường là cổng virtio-user được tạo) */
    uint16_t port = 0;
    if (port_id) {
        *port_id = port;
    }

    printf("[DPDK_INIT] Available Ethernet ports: %u. Using port %u.\n", nb_ports, port);

    /* 3. Tạo Mempool */
    char pool_name[32];
    snprintf(pool_name, sizeof(pool_name), "MBUF_POOL_%u", port);

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
        pool_name,
        NUM_MBUFS,
        MBUF_CACHE_SIZE,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_socket_id()
    );

    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: Cannot create mbuf pool '%s': %s\n",
                 pool_name, rte_strerror(rte_errno));
    }
    printf("[DPDK_INIT] Created mbuf pool '%s' successfully\n", pool_name);

    /* 4. Cấu hình Port */
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));

    ret = rte_eth_dev_configure(port, 1, 1, &port_conf);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: Cannot configure port %u (ret: %d)\n", port, ret);
    }

    /* Điều chỉnh số lượng RX/TX descriptors nếu driver yêu cầu */
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: Cannot adjust number of descriptors (ret: %d)\n", ret);
    }

    /* 5. Cấu hình RX Queue */
    ret = rte_eth_rx_queue_setup(port, 0, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: RX queue setup failed for port %u (ret: %d)\n", port, ret);
    }

    /* 6. Cấu hình TX Queue */
    ret = rte_eth_tx_queue_setup(port, 0, nb_txd, rte_eth_dev_socket_id(port), NULL);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: TX queue setup failed for port %u (ret: %d)\n", port, ret);
    }

    /* 7. Khởi động Port */
    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: Cannot start port %u (ret: %d)\n", port, ret);
    }

    /* 8. Bật chế độ promiscuous */
    ret = rte_eth_promiscuous_enable(port);
    if (ret < 0) {
        printf("[DPDK_INIT] Warning: Promiscuous mode enable failed on port %u\n", port);
    }

    /* In địa chỉ MAC của cổng */
    struct rte_ether_addr mac_addr;
    ret = rte_eth_macaddr_get(port, &mac_addr);
    if (ret == 0) {
        printf("[DPDK_INIT] Port %u MAC Address: " RTE_ETHER_ADDR_PRT_FMT "\n",
               port, RTE_ETHER_ADDR_BYTES(&mac_addr));
    }

    printf("[DPDK_INIT] Port %u initialized and started successfully.\n", port);
    return mbuf_pool;
}

void cleanup_dpdk_subsystem(uint16_t port_id)
{
    printf("[DPDK_INIT] Stopping and closing port %u...\n", port_id);
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_eal_cleanup();
    printf("[DPDK_INIT] DPDK cleanup finished.\n");
}

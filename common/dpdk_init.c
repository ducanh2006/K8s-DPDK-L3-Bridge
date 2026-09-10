#include "dpdk_init.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_ether.h>

struct rte_mempool *init_dpdk_subsystem(int argc, char **argv, uint16_t *nb_ports, int *eal_consumed)
{
    /* 1. Khởi tạo DPDK EAL */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: rte_eal_init failed (ret: %d)\n", ret);
    }
    if (eal_consumed) {
        *eal_consumed = ret;
    }

    /* 2. Kiểm tra các port có sẵn */
    uint16_t total_ports = rte_eth_dev_count_avail();
    if (total_ports == 0) {
        rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: No Ethernet ports available! Please check --vdev arguments.\n");
    }

    printf("[DPDK_INIT] Detected %u available Ethernet port(s).\n", total_ports);

    /* 3. Tạo Mempool dùng chung cho tất cả các port */
    const char *pool_name = "MBUF_POOL_SHARED";
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
    printf("[DPDK_INIT] Created shared mbuf pool '%s' (capacity: %u) successfully\n",
           pool_name, NUM_MBUFS);

    /* 4. Cấu hình từng port */
    uint16_t configured_count = 0;
    for (uint16_t port = 0; port < total_ports; port++) {
        struct rte_eth_dev_info dev_info;
        ret = rte_eth_dev_info_get(port, &dev_info);
        if (ret != 0) {
            fprintf(stderr, "[DPDK_INIT] Error getting device info for port %u\n", port);
            continue;
        }

        uint16_t rx_q = (dev_info.max_rx_queues > 0) ? 1 : 0;
        uint16_t tx_q = (dev_info.max_tx_queues > 0) ? 1 : 0;

        struct rte_eth_conf port_conf;
        memset(&port_conf, 0, sizeof(port_conf));

        ret = rte_eth_dev_configure(port, rx_q, tx_q, &port_conf);
        if (ret < 0) {
            rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: Cannot configure port %u (ret: %d)\n", port, ret);
        }

        uint16_t nb_rxd = RX_RING_SIZE;
        uint16_t nb_txd = TX_RING_SIZE;
        rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);

        /* Setup RX Queue nếu hỗ trợ */
        if (rx_q > 0) {
            ret = rte_eth_rx_queue_setup(port, 0, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
            if (ret < 0) {
                rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: RX queue setup failed for port %u (ret: %d)\n", port, ret);
            }
        }

        /* Setup TX Queue nếu hỗ trợ */
        if (tx_q > 0) {
            ret = rte_eth_tx_queue_setup(port, 0, nb_txd, rte_eth_dev_socket_id(port), NULL);
            if (ret < 0) {
                rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: TX queue setup failed for port %u (ret: %d)\n", port, ret);
            }
        }

        /* Khởi động Port */
        ret = rte_eth_dev_start(port);
        if (ret < 0) {
            rte_exit(EXIT_FAILURE, "[DPDK_INIT] Error: Cannot start port %u (ret: %d)\n", port, ret);
        }

        rte_eth_promiscuous_enable(port);

        struct rte_ether_addr mac_addr;
        ret = rte_eth_macaddr_get(port, &mac_addr);
        if (ret == 0) {
            printf("[DPDK_INIT] Port %u (Driver: %s, RX_Q: %u, TX_Q: %u) MAC: " RTE_ETHER_ADDR_PRT_FMT "\n",
                   port, dev_info.driver_name ? dev_info.driver_name : "unknown",
                   rx_q, tx_q, RTE_ETHER_ADDR_BYTES(&mac_addr));
        }

        configured_count++;
    }

    if (nb_ports) {
        *nb_ports = configured_count;
    }

    printf("[DPDK_INIT] Configured and started %u port(s) successfully.\n", configured_count);
    return mbuf_pool;
}

void cleanup_dpdk_subsystem(uint16_t nb_ports)
{
    printf("[DPDK_INIT] Stopping and cleaning up %u port(s)...\n", nb_ports);
    for (uint16_t port = 0; port < nb_ports; port++) {
        rte_eth_dev_stop(port);
        rte_eth_dev_close(port);
    }
    rte_eal_cleanup();
    printf("[DPDK_INIT] DPDK cleanup finished.\n");
}

#ifndef HW_STATS_H
#define HW_STATS_H

#include <stdint.h>
#include <rte_ethdev.h>

/**
 * In thống kê phần cứng / PMD NIC port (imissed, ierrors, oerrors)
 */
void print_port_hw_stats(uint16_t port_id, const char *tag);

#endif /* HW_STATS_H */

#include "hw_stats.h"

#include <stdio.h>
#include <inttypes.h>

void print_port_hw_stats(uint16_t port_id, const char *tag)
{
    struct rte_eth_stats stats;
    if (rte_eth_stats_get(port_id, &stats) == 0) {
        printf("%s [HW Stats Port %u] in: %" PRIu64 ", out: %" PRIu64 ", imissed: %" PRIu64 ", ierrors: %" PRIu64 ", oerrors: %" PRIu64 "\n",
               tag ? tag : "", port_id, stats.ipackets, stats.opackets, stats.imissed, stats.ierrors, stats.oerrors);
        fflush(stdout);
    }
}

#include "group_stats.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

void group_stats_init(struct group_stats_tracker *gs, const char *name)
{
    (void)name;
    if (!gs) return;
    memset(gs, 0, sizeof(*gs));
}

void group_stats_record(struct group_stats_tracker *gs, struct rte_mbuf *m)
{
    if (unlikely(!gs || !m)) return;

    /* Guard: đủ dài để đọc Ethernet header */
    if (unlikely(m->pkt_len < sizeof(struct rte_ether_hdr))) {
        gs->non_ip_pkts++;
        return;
    }

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    uint16_t eth_type = rte_be_to_cpu_16(eth->ether_type);

    /* Bỏ qua gói tin không phải IPv4 */
    if (unlikely(eth_type != RTE_ETHER_TYPE_IPV4)) {
        gs->non_ip_pkts++;
        return;
    }

    /* Guard: đủ dài để đọc IPv4 header cố định */
    if (unlikely(m->pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr))) {
        gs->non_ip_pkts++;
        return;
    }

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((uint8_t *)eth + sizeof(struct rte_ether_hdr));
    uint8_t proto = ip->next_proto_id;
    rte_be32_t dst_addr = ip->dst_addr;
    rte_be32_t src_addr = ip->src_addr;

    rte_be16_t dst_port = 0;
    rte_be16_t src_port = 0;

    size_t ip_hdr_len = (ip->version_ihl & RTE_IPV4_HDR_IHL_MASK) * RTE_IPV4_IHL_MULTIPLIER;
    if (unlikely(ip_hdr_len < sizeof(struct rte_ipv4_hdr) ||
                 m->pkt_len < sizeof(struct rte_ether_hdr) + ip_hdr_len)) {
        gs->non_ip_pkts++;
        return;
    }

    if (proto == IPPROTO_TCP) {
        /* Guard: đủ dài để đọc TCP ports */
        if (unlikely(m->pkt_len < sizeof(struct rte_ether_hdr) + ip_hdr_len + 4)) {
            gs->non_ip_pkts++;
            return;
        }
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + ip_hdr_len);
        dst_port = tcp->dst_port;
        src_port = tcp->src_port;
    } else if (proto == IPPROTO_UDP) {
        /* Guard: đủ dài để đọc UDP ports */
        if (unlikely(m->pkt_len < sizeof(struct rte_ether_hdr) + ip_hdr_len + sizeof(struct rte_udp_hdr))) {
            gs->non_ip_pkts++;
            return;
        }
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + ip_hdr_len);
        dst_port = udp->dst_port;
        src_port = udp->src_port;
    }

    /* Đối sánh lần lượt theo thứ tự priority giảm dần (First-match wins) */
    for (int i = 0; i < NUM_FILTER_RULES; i++) {
        const struct filter_rule *r = &FILTER_RULES[i];

        if (r->proto != 0 && proto != r->proto) {
            continue;
        }
        if (r->dst_mask != 0 && (dst_addr & r->dst_mask) != r->dst_net) {
            continue;
        }
        if (r->src_mask != 0 && (src_addr & r->src_mask) != r->src_net) {
            continue;
        }
        if (r->dst_port != 0 && dst_port != r->dst_port) {
            continue;
        }
        if (r->src_port != 0 && src_port != r->src_port) {
            continue;
        }

        /* Gói tin khớp rule -> cộng dồn vào group tương ứng */
        enum group_id gid = r->group_id;
        uint32_t pkt_len = m->pkt_len;

        gs->groups[gid].period_pkts++;
        gs->groups[gid].period_bytes += pkt_len;
        gs->groups[gid].total_pkts++;
        return;
    }

    /* Không khớp rule nào (không nên xảy ra vì có default catch-all) */
    gs->non_ip_pkts++;
}

void group_stats_print_table(struct group_stats_tracker *gs, const char *tag, double elapsed_sec)
{
    if (!gs) return;
    const char *pfx = tag ? tag : "[GRP]";

    printf("\n%s ==================== L3/L4 GROUP STATISTICS (SSOT) ====================\n", pfx);
    printf("%s %-20s | %-4s | %-7s | %12s | %14s | %12s\n",
           pfx, "Group Name", "Prio", "Action", "Period Pkts", "Throughput", "Total Pkts");
    printf("%s ---------------------+------+---------+--------------+----------------+-------------\n", pfx);

    for (int i = 0; i < NUM_GROUPS; i++) {
        const struct group_info *g = &GROUP_INFOS[i];
        uint64_t pkts = gs->groups[i].period_pkts;
        uint64_t bytes = gs->groups[i].period_bytes;
        uint64_t total = gs->groups[i].total_pkts;

        double mbps = 0.0;
        if (elapsed_sec > 0.0) {
            mbps = ((double)bytes * 8.0) / (elapsed_sec * 1e6);
        }

        printf("%s %-20s | %4u | %-7s | %12" PRIu64 " | %10.3f Mbps | %12" PRIu64 "\n",
               pfx, g->name, g->priority, g->action_str, pkts, mbps, total);
    }
    printf("%s ========================================================================\n\n", pfx);
    fflush(stdout);
}

void group_stats_reset_period(struct group_stats_tracker *gs)
{
    if (!gs) return;
    for (int i = 0; i < NUM_GROUPS; i++) {
        gs->groups[i].period_pkts = 0;
        gs->groups[i].period_bytes = 0;
    }
}

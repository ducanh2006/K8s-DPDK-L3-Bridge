#include "flow_tracker.h"
#include "pkt_utils.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <inttypes.h>

static inline uint32_t hash_5tuple(const struct flow_key *k)
{
    uint32_t hash = 2166136261u;
    hash ^= (uint32_t)k->src_ip;
    hash *= 16777619u;
    hash ^= (uint32_t)k->dst_ip;
    hash *= 16777619u;
    hash ^= ((uint32_t)k->src_port << 16) | (uint32_t)k->dst_port;
    hash *= 16777619u;
    hash ^= (uint32_t)k->proto;
    hash *= 16777619u;
    return hash;
}

static inline bool keys_equal(const struct flow_key *a, const struct flow_key *b)
{
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->src_port == b->src_port &&
            a->dst_port == b->dst_port &&
            a->proto == b->proto);
}

void flow_tracker_init(struct flow_tracker *ft, const char *name)
{
    memset(ft, 0, sizeof(*ft));
    if (name) {
        snprintf(ft->name, sizeof(ft->name), "%s", name);
    } else {
        snprintf(ft->name, sizeof(ft->name), "FlowTracker");
    }
}

void flow_tracker_record(struct flow_tracker *ft, struct rte_mbuf *m)
{
    if (!ft || !m) return;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    uint16_t eth_type = rte_be_to_cpu_16(eth->ether_type);

    /* Bỏ qua gói không phải IPv4 để giữ tốc độ cao */
    if (eth_type != RTE_ETHER_TYPE_IPV4) {
        return;
    }

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((uint8_t *)eth + sizeof(struct rte_ether_hdr));
    uint16_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

    struct flow_key key;
    key.src_ip = ip->src_addr;
    key.dst_ip = ip->dst_addr;
    key.proto = ip->next_proto_id;
    key.src_port = 0;
    key.dst_port = 0;

    if (ip->next_proto_id == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + ip_hdr_len);
        key.src_port = tcp->src_port;
        key.dst_port = tcp->dst_port;
    } else if (ip->next_proto_id == IPPROTO_UDP) {
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + ip_hdr_len);
        key.src_port = udp->src_port;
        key.dst_port = udp->dst_port;
    }

    uint32_t hash = hash_5tuple(&key);
    uint32_t idx = hash & FLOW_HASH_MASK;
    uint64_t now = get_current_time_ns();

    /* Linear probing tối đa 16 bước */
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t cur = (idx + i) & FLOW_HASH_MASK;
        struct flow_entry *entry = &ft->entries[cur];

        if (!entry->is_active) {
            /* Slot trống, tạo entry mới */
            entry->key = key;
            entry->is_active = true;
            entry->total_pkts = 1;
            entry->total_bytes = m->pkt_len;
            entry->period_pkts = 1;
            entry->period_bytes = m->pkt_len;
            entry->last_seen_ns = now;
            ft->active_flows++;
            ft->total_tracked_pkts++;
            ft->total_tracked_bytes += m->pkt_len;
            return;
        }

        if (keys_equal(&entry->key, &key)) {
            /* Khớp luồng đã có, cập nhật counter */
            entry->total_pkts++;
            entry->total_bytes += m->pkt_len;
            entry->period_pkts++;
            entry->period_bytes += m->pkt_len;
            entry->last_seen_ns = now;
            ft->total_tracked_pkts++;
            ft->total_tracked_bytes += m->pkt_len;
            return;
        }
    }

    /* Nếu bảng đầy hoặc va chạm vượt 16, ghi đè entry cũ nhất trong 16 slot này */
    uint32_t oldest_idx = idx;
    uint64_t oldest_time = ft->entries[idx].last_seen_ns;
    for (uint32_t i = 1; i < 16; i++) {
        uint32_t cur = (idx + i) & FLOW_HASH_MASK;
        if (ft->entries[cur].last_seen_ns < oldest_time) {
            oldest_time = ft->entries[cur].last_seen_ns;
            oldest_idx = cur;
        }
    }
    struct flow_entry *replace = &ft->entries[oldest_idx];
    replace->key = key;
    replace->total_pkts = 1;
    replace->total_bytes = m->pkt_len;
    replace->period_pkts = 1;
    replace->period_bytes = m->pkt_len;
    replace->last_seen_ns = now;
    ft->total_tracked_pkts++;
    ft->total_tracked_bytes += m->pkt_len;
}

static const char *proto_to_str(uint8_t proto)
{
    switch (proto) {
    case IPPROTO_TCP:  return "TCP ";
    case IPPROTO_UDP:  return "UDP ";
    case IPPROTO_ICMP: return "ICMP";
    default:           return "OTH ";
    }
}

void flow_tracker_print_top(struct flow_tracker *ft, int top_n, const char *tag)
{
    if (!ft || top_n <= 0) return;

    /* Tìm top N flows có số gói period_pkts lớn nhất (hoặc total_pkts nếu period rỗng) */
    int indices[16];
    if (top_n > 16) top_n = 16;
    for (int i = 0; i < top_n; i++) indices[i] = -1;

    for (uint32_t i = 0; i < FLOW_TABLE_SIZE; i++) {
        if (!ft->entries[i].is_active) continue;
        uint64_t count = ft->entries[i].period_pkts ? ft->entries[i].period_pkts : ft->entries[i].total_pkts;
        if (count == 0) continue;

        for (int j = 0; j < top_n; j++) {
            if (indices[j] == -1 || count > (ft->entries[indices[j]].period_pkts ? ft->entries[indices[j]].period_pkts : ft->entries[indices[j]].total_pkts)) {
                for (int k = top_n - 1; k > j; k--) {
                    indices[k] = indices[k - 1];
                }
                indices[j] = (int)i;
                break;
            }
        }
    }

    printf("\n%s ---------- Top Active IP:Port Traffic Flows (5s Period) ----------\n", tag ? tag : "");
    printf("%s %-4s | %-21s -> %-21s | %-10s | %-9s\n",
           tag ? tag : "", "Prot", "Source IP:Port", "Dest IP:Port", "Period Pkt", "Throughput");
    printf("%s ----------------------------------------------------------------------\n", tag ? tag : "");

    int displayed = 0;
    for (int i = 0; i < top_n; i++) {
        if (indices[i] == -1) break;
        struct flow_entry *e = &ft->entries[indices[i]];

        char src_ip_str[INET_ADDRSTRLEN];
        char dst_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &e->key.src_ip, src_ip_str, sizeof(src_ip_str));
        inet_ntop(AF_INET, &e->key.dst_ip, dst_ip_str, sizeof(dst_ip_str));

        char src_buf[32], dst_buf[32];
        snprintf(src_buf, sizeof(src_buf), "%s:%u", src_ip_str, rte_be_to_cpu_16(e->key.src_port));
        snprintf(dst_buf, sizeof(dst_buf), "%s:%u", dst_ip_str, rte_be_to_cpu_16(e->key.dst_port));

        double mbps = ((double)e->period_bytes * 8.0) / (5.0 * 1e6);
        printf("%s %-4s | %-21s -> %-21s | %-10" PRIu64 " | %6.2f Mbps\n",
               tag ? tag : "",
               proto_to_str(e->key.proto),
               src_buf, dst_buf,
               e->period_pkts, mbps);
        displayed++;
    }

    if (displayed == 0) {
        printf("%s (No active traffic recorded in this period)\n", tag ? tag : "");
    }
    printf("%s ----------------------------------------------------------------------\n\n", tag ? tag : "");
    fflush(stdout);
}

void flow_tracker_reset_period(struct flow_tracker *ft)
{
    if (!ft) return;
    for (uint32_t i = 0; i < FLOW_TABLE_SIZE; i++) {
        if (ft->entries[i].is_active) {
            ft->entries[i].period_pkts = 0;
            ft->entries[i].period_bytes = 0;
        }
    }
}

void print_port_hw_stats(uint16_t port_id, const char *tag)
{
    struct rte_eth_stats stats;
    if (rte_eth_stats_get(port_id, &stats) == 0) {
        printf("%s [HW Stats Port %u] in: %" PRIu64 ", out: %" PRIu64 ", imissed: %" PRIu64 ", ierrors: %" PRIu64 ", oerrors: %" PRIu64 "\n",
               tag ? tag : "", port_id, stats.ipackets, stats.opackets, stats.imissed, stats.ierrors, stats.oerrors);
        fflush(stdout);
    }
}

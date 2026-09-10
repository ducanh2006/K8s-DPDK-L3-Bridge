#include "l3_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <rte_lpm.h>
#include <rte_malloc.h>
#include <rte_byteorder.h>

#define MAX_L3_RULES 1024
#define NUMBER_TBL8S 256

struct l3_table {
    struct rte_lpm *lpm;
    char name[64];
    l3_action_t default_action;
};

struct l3_table *l3_table_init(const char *name, uint32_t socket_id)
{
    struct l3_table *table = rte_zmalloc(NULL, sizeof(struct l3_table), RTE_CACHE_LINE_SIZE);
    if (!table) {
        fprintf(stderr, "[L3_TABLE] Error: Failed to allocate memory for l3_table\n");
        return NULL;
    }

    snprintf(table->name, sizeof(table->name), "%s", name);
    table->default_action = L3_ACTION_DROP;

    struct rte_lpm_config config;
    memset(&config, 0, sizeof(config));
    config.max_rules = MAX_L3_RULES;
    config.number_tbl8s = NUMBER_TBL8S;
    config.flags = 0;

    table->lpm = rte_lpm_create(name, socket_id, &config);
    if (!table->lpm) {
        fprintf(stderr, "[L3_TABLE] Error: Failed to create DPDK LPM table '%s'\n", name);
        rte_free(table);
        return NULL;
    }

    printf("[L3_TABLE] Initialized LPM table '%s' successfully\n", name);
    return table;
}

int l3_table_add_route(struct l3_table *table, const char *ip_cidr, l3_action_t action)
{
    if (!table || !table->lpm || !ip_cidr) {
        return -1;
    }

    char cidr_buf[64];
    snprintf(cidr_buf, sizeof(cidr_buf), "%s", ip_cidr);

    char *slash = strchr(cidr_buf, '/');
    uint8_t depth = 32;
    if (slash) {
        *slash = '\0';
        depth = (uint8_t)atoi(slash + 1);
        if (depth > 32) {
            fprintf(stderr, "[L3_TABLE] Invalid prefix depth: %u in %s\n", depth, ip_cidr);
            return -1;
        }
    }

    struct in_addr in_addr;
    if (inet_pton(AF_INET, cidr_buf, &in_addr) != 1) {
        fprintf(stderr, "[L3_TABLE] Invalid IP address: %s\n", cidr_buf);
        return -1;
    }

    /* DPDK LPM yêu cầu 1 <= depth <= 32. Nếu depth = 0 (0.0.0.0/0), ta lưu vào default_action */
    if (depth == 0) {
        table->default_action = action;
        printf("[L3_TABLE] Added Default Route: 0.0.0.0/0 -> %s\n",
               (action == L3_ACTION_FORWARD) ? "FORWARD" : "DROP");
        return 0;
    }

    /* DPDK LPM sử dụng địa chỉ IP theo host byte order */
    uint32_t host_ip = rte_be_to_cpu_32(in_addr.s_addr);

    int ret = rte_lpm_add(table->lpm, host_ip, depth, (uint32_t)action);
    if (ret < 0) {
        fprintf(stderr, "[L3_TABLE] Failed to add rule %s -> %s (code: %d)\n",
                ip_cidr, (action == L3_ACTION_FORWARD) ? "FORWARD" : "DROP", ret);
        return ret;
    }

    printf("[L3_TABLE] Added Rule: %-18s -> %s\n",
           ip_cidr, (action == L3_ACTION_FORWARD) ? "FORWARD" : "DROP");
    return 0;
}

int l3_table_load_file(struct l3_table *table, const char *conf_path)
{
    if (!table || !conf_path) return -1;

    FILE *fp = fopen(conf_path, "r");
    if (!fp) {
        fprintf(stderr, "[L3_TABLE] Warning: Cannot open config file '%s'\n", conf_path);
        return -1;
    }

    char line[256];
    int loaded = 0;

    printf("[L3_TABLE] Loading routes from '%s'...\n", conf_path);

    while (fgets(line, sizeof(line), fp)) {
        /* Bỏ khoảng trắng đầu dòng */
        char *p = line;
        while (isspace((unsigned char)*p)) p++;

        /* Bỏ dòng trống hoặc comment */
        if (*p == '\0' || *p == '#') continue;

        /* Cắt newline ở cuối */
        char *end = p + strlen(p) - 1;
        while (end > p && isspace((unsigned char)*end)) {
            *end = '\0';
            end--;
        }

        /* Phân tích: <CIDR> <ACTION> */
        char cidr[64] = {0};
        char action_str[32] = {0};
        if (sscanf(p, "%63s %31s", cidr, action_str) == 2) {
            l3_action_t act = L3_ACTION_DROP;
            if (strcasecmp(action_str, "FORWARD") == 0) {
                act = L3_ACTION_FORWARD;
            } else if (strcasecmp(action_str, "DROP") == 0) {
                act = L3_ACTION_DROP;
            } else {
                fprintf(stderr, "[L3_TABLE] Unknown action '%s' in line: %s\n", action_str, p);
                continue;
            }

            if (l3_table_add_route(table, cidr, act) == 0) {
                loaded++;
            }
        }
    }

    fclose(fp);
    printf("[L3_TABLE] Successfully loaded %d rules from '%s'\n", loaded, conf_path);
    return loaded;
}

l3_action_t l3_table_lookup(struct l3_table *table, rte_be32_t dst_ip)
{
    if (unlikely(!table || !table->lpm)) {
        return L3_ACTION_DROP;
    }

    uint32_t host_ip = rte_be_to_cpu_32(dst_ip);
    uint32_t next_hop = 0;

    int ret = rte_lpm_lookup(table->lpm, host_ip, &next_hop);
    if (ret == 0) {
        return (l3_action_t)next_hop;
    }

    /* Nếu không khớp bất kỳ dải cụ thể nào, trả về default_action */
    return table->default_action;
}

void l3_table_free(struct l3_table *table)
{
    if (!table) return;
    if (table->lpm) {
        rte_lpm_free(table->lpm);
    }
    rte_free(table);
}

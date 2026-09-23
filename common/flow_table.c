#include "flow_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

/* Hàm phụ trợ cắt bỏ khoảng trắng ở đầu và cuối chuỗi */
static char *trim_whitespace(char *str)
{
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/* Chuyển đổi chuỗi CIDR (ví dụ "31.13.64.0/18", "74.125.0.1", "*") sang Network Byte Order */
static int parse_cidr(const char *cidr_str, rte_be32_t *out_net, rte_be32_t *out_mask)
{
    if (!cidr_str || !out_net || !out_mask) return -1;

    if (strcmp(cidr_str, "*") == 0 || strcmp(cidr_str, "0.0.0.0/0") == 0) {
        *out_net = 0;
        *out_mask = 0;
        return 0;
    }

    char buf[64];
    strncpy(buf, cidr_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *slash = strchr(buf, '/');
    int depth = 32;
    if (slash) {
        *slash = '\0';
        depth = atoi(slash + 1);
        if (depth < 0 || depth > 32) {
            return -1;
        }
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, buf, &addr) != 1) {
        return -1;
    }

    uint32_t mask_host = 0;
    if (depth > 0) {
        if (depth == 32) {
            mask_host = 0xFFFFFFFFU;
        } else {
            mask_host = (0xFFFFFFFFU << (32 - depth)) & 0xFFFFFFFFU;
        }
    }

    rte_be32_t mask_be = rte_cpu_to_be_32(mask_host);
    *out_mask = mask_be;
    *out_net = addr.s_addr & mask_be;
    return 0;
}

/* Chuyển đổi giao thức sang số chuẩn IP */
static int parse_proto(const char *proto_str, uint8_t *out_proto)
{
    if (!proto_str || !out_proto) return -1;

    if (strcmp(proto_str, "*") == 0 || strcmp(proto_str, "0") == 0) {
        *out_proto = 0;
    } else if (strcasecmp(proto_str, "tcp") == 0) {
        *out_proto = IPPROTO_TCP;
    } else if (strcasecmp(proto_str, "udp") == 0) {
        *out_proto = IPPROTO_UDP;
    } else if (strcasecmp(proto_str, "icmp") == 0) {
        *out_proto = IPPROTO_ICMP;
    } else {
        *out_proto = (uint8_t)atoi(proto_str);
    }
    return 0;
}

/* Chuyển đổi Port sang Network Byte Order */
static int parse_port(const char *port_str, rte_be16_t *out_port)
{
    if (!port_str || !out_port) return -1;

    if (strcmp(port_str, "*") == 0) {
        *out_port = 0;
        return 0;
    }

    int port = atoi(port_str);
    if (port < 0 || port > 65535) {
        return -1;
    }
    *out_port = rte_cpu_to_be_16((uint16_t)port);
    return 0;
}

/* Hàm so sánh ưu tiên giảm dần cho qsort */
static int compare_rules_desc_priority(const void *a, const void *b)
{
    const struct flow_rule *ra = (const struct flow_rule *)a;
    const struct flow_rule *rb = (const struct flow_rule *)b;

    if (ra->priority > rb->priority) return -1;
    if (ra->priority < rb->priority) return 1;
    return 0;
}

int flow_table_load(struct flow_table *ft, const char *conf_path)
{
    if (!ft || !conf_path) return -1;
    memset(ft, 0, sizeof(*ft));

    FILE *fp = fopen(conf_path, "r");
    if (!fp) {
        fprintf(stderr, "[FlowTable] LỖI: Không thể mở file cấu hình '%s'\n", conf_path);
        return -1;
    }

    char line[1024];
    size_t count_groups = 0;
    size_t count_rules = 0;
    int section = 0; /* 0: None, 1: Groups, 2: Filters */

    /* =========================================================================
     * PASS 1: Đếm chính xác số lượng groups và rules để cấp phát bộ nhớ đúng 1 lần
     * ========================================================================= */
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_whitespace(line);
        if (*p == '\0' || *p == '#') continue;

        if (strstr(p, "[GROUPS_SECTION]")) {
            section = 1;
            continue;
        } else if (strstr(p, "[FILTERS_SECTION]")) {
            section = 2;
            continue;
        }

        if (section == 1) {
            count_groups++;
        } else if (section == 2) {
            count_rules++;
        }
    }

    if (count_groups == 0) {
        fprintf(stderr, "[FlowTable] LỖI: Không tìm thấy Group nào trong '%s'\n", conf_path);
        fclose(fp);
        return -1;
    }

    ft->groups = (struct flow_group *)calloc(count_groups, sizeof(struct flow_group));
    if (count_rules > 0) {
        ft->rules = (struct flow_rule *)calloc(count_rules, sizeof(struct flow_rule));
    }
    if (!ft->groups || (count_rules > 0 && !ft->rules)) {
        fprintf(stderr, "[FlowTable] LỖI: Không thể cấp phát bộ nhớ cho flow_table\n");
        flow_table_free(ft);
        fclose(fp);
        return -1;
    }

    /* =========================================================================
     * PASS 2: Nạp dữ liệu và kiểm tra tính hợp lệ
     * ========================================================================= */
    rewind(fp);
    section = 0;
    size_t cur_group = 0;
    size_t cur_rule = 0;
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char *p = trim_whitespace(line);
        if (*p == '\0' || *p == '#') continue;

        if (strstr(p, "[GROUPS_SECTION]")) {
            section = 1;
            continue;
        } else if (strstr(p, "[FILTERS_SECTION]")) {
            section = 2;
            continue;
        }

        if (section == 1) {
            /* Cú pháp: [Group_Name],[Priority],[Action] */
            char *saveptr = NULL;
            char *name = strtok_r(p, ",", &saveptr);
            char *prio_str = strtok_r(NULL, ",", &saveptr);
            char *act_str = strtok_r(NULL, ",", &saveptr);

            if (!name || !prio_str || !act_str) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: Cú pháp Group không hợp lệ: '%s'\n", line_num, line);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }

            name = trim_whitespace(name);
            prio_str = trim_whitespace(prio_str);
            act_str = trim_whitespace(act_str);

            /* Kiểm tra trùng lặp tên Group */
            for (size_t i = 0; i < cur_group; i++) {
                if (strcmp(ft->groups[i].name, name) == 0) {
                    fprintf(stderr, "[FlowTable] LỖI dòng %d: Trùng lặp tên Group '%s'\n", line_num, name);
                    flow_table_free(ft);
                    fclose(fp);
                    return -1;
                }
            }

            struct flow_group *g = &ft->groups[cur_group++];
            strncpy(g->name, name, sizeof(g->name) - 1);
            g->priority = (uint32_t)strtoul(prio_str, NULL, 10);
            strncpy(g->action, act_str, sizeof(g->action) - 1);
            g->is_drop = (strcasecmp(act_str, "DROP") == 0);

        } else if (section == 2) {
            /* Cú pháp: [Filter_Name],[Group],[Proto],[SrcIP/Mask],[DstIP/Mask],[SrcPort],[DstPort] */
            char *saveptr = NULL;
            char *f_name = strtok_r(p, ",", &saveptr);
            char *f_grp = strtok_r(NULL, ",", &saveptr);
            char *f_proto = strtok_r(NULL, ",", &saveptr);
            char *f_src_ip = strtok_r(NULL, ",", &saveptr);
            char *f_dst_ip = strtok_r(NULL, ",", &saveptr);
            char *f_src_port = strtok_r(NULL, ",", &saveptr);
            char *f_dst_port = strtok_r(NULL, ",", &saveptr);

            if (!f_name || !f_grp || !f_proto || !f_src_ip || !f_dst_ip || !f_src_port || !f_dst_port) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: Cú pháp Filter không hợp lệ: '%s'\n", line_num, line);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }

            f_name = trim_whitespace(f_name);
            f_grp = trim_whitespace(f_grp);
            f_proto = trim_whitespace(f_proto);
            f_src_ip = trim_whitespace(f_src_ip);
            f_dst_ip = trim_whitespace(f_dst_ip);
            f_src_port = trim_whitespace(f_src_port);
            f_dst_port = trim_whitespace(f_dst_port);

            /* Tìm group index tương ứng */
            int found_idx = -1;
            for (size_t i = 0; i < cur_group; i++) {
                if (strcmp(ft->groups[i].name, f_grp) == 0) {
                    found_idx = (int)i;
                    break;
                }
            }
            if (found_idx < 0) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: Filter '%s' tham chiếu Group không tồn tại '%s'\n",
                        line_num, f_name, f_grp);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }

            struct flow_rule *r = &ft->rules[cur_rule++];
            strncpy(r->rule_name, f_name, sizeof(r->rule_name) - 1);
            r->group_idx = (uint32_t)found_idx;
            r->priority = ft->groups[found_idx].priority;

            if (parse_proto(f_proto, &r->proto) != 0) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: Protocol không hợp lệ '%s'\n", line_num, f_proto);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }

            if (parse_cidr(f_src_ip, &r->src_net, &r->src_mask) != 0) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: SrcIP không hợp lệ '%s'\n", line_num, f_src_ip);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }

            if (parse_cidr(f_dst_ip, &r->dst_net, &r->dst_mask) != 0) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: DstIP không hợp lệ '%s'\n", line_num, f_dst_ip);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }

            if (parse_port(f_src_port, &r->src_port) != 0) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: SrcPort không hợp lệ '%s'\n", line_num, f_src_port);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }

            if (parse_port(f_dst_port, &r->dst_port) != 0) {
                fprintf(stderr, "[FlowTable] LỖI dòng %d: DstPort không hợp lệ '%s'\n", line_num, f_dst_port);
                flow_table_free(ft);
                fclose(fp);
                return -1;
            }
        }
    }

    fclose(fp);

    ft->num_groups = cur_group;
    ft->num_rules = cur_rule;

    /* Sắp xếp các luật theo thứ tự priority giảm dần (First-match wins) */
    if (ft->num_rules > 1) {
        qsort(ft->rules, ft->num_rules, sizeof(struct flow_rule), compare_rules_desc_priority);
    }

    printf("[FlowTable] Nạp thành công %zu groups và %zu rules từ '%s'\n",
           ft->num_groups, ft->num_rules, conf_path);
    return 0;
}

void flow_table_free(struct flow_table *ft)
{
    if (!ft) return;
    if (ft->groups) {
        free(ft->groups);
        ft->groups = NULL;
    }
    if (ft->rules) {
        free(ft->rules);
        ft->rules = NULL;
    }
    ft->num_groups = 0;
    ft->num_rules = 0;
}

void flow_table_dump(const struct flow_table *ft)
{
    if (!ft) return;
    printf("\n--- FLOW TABLE DUMP: %zu Groups, %zu Rules ---\n", ft->num_groups, ft->num_rules);
    printf("[Groups]\n");
    for (size_t i = 0; i < ft->num_groups; i++) {
        const struct flow_group *g = &ft->groups[i];
        printf("  [%zu] %-24s Prio:%-2u Action:%-7s (is_drop=%d)\n",
               i, g->name, g->priority, g->action, g->is_drop);
    }
    printf("[Rules - Sorted by Priority Descending]\n");
    for (size_t i = 0; i < ft->num_rules; i++) {
        const struct flow_rule *r = &ft->rules[i];
        char src_ip_str[INET_ADDRSTRLEN] = "*";
        char dst_ip_str[INET_ADDRSTRLEN] = "*";
        if (r->src_mask != 0) {
            struct in_addr a = { .s_addr = r->src_net };
            inet_ntop(AF_INET, &a, src_ip_str, sizeof(src_ip_str));
        }
        if (r->dst_mask != 0) {
            struct in_addr a = { .s_addr = r->dst_net };
            inet_ntop(AF_INET, &a, dst_ip_str, sizeof(dst_ip_str));
        }
        printf("  [%zu] %-20s -> Group:%-2u (%-20s) Prio:%-2u Proto:%-2u Src:%s Dst:%s Ports:%u->%u\n",
               i, r->rule_name, r->group_idx, ft->groups[r->group_idx].name,
               r->priority, r->proto, src_ip_str, dst_ip_str,
               rte_be_to_cpu_16(r->src_port), rte_be_to_cpu_16(r->dst_port));
    }
    printf("----------------------------------------------\n\n");
}

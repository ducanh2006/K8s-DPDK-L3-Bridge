#!/usr/bin/env bash
# ==============================================================================
# Script: ovs-setup.sh
# Mục đích: Tạo bridge OVS-DPDK (br-dpdk), tạo 2 cổng vhost-user và cấu hình OpenFlow
# Yêu cầu: Chạy với quyền root: sudo bash manifests/ovs-setup.sh
# ==============================================================================

set -e

if [ "$(id -u)" -ne 0 ]; then
    echo -e "\033[1;31m[LỖI] Script này bắt buộc phải chạy với quyền root: sudo bash $0\033[0m"
    exit 1
fi

BRIDGE="br-dpdk"
PORT0="vhost-user-0"
PORT1="vhost-user-1"
SOCK_DIR="/var/run/openvswitch"

echo -e "\033[1;34m=====================================================\033[0m"
echo -e "\033[1;34m   CẤU HÌNH SWITCH ẢO OVS-DPDK & VHOST-USER PORTS    \033[0m"
echo -e "\033[1;34m=====================================================\033[0m"

mkdir -p "$SOCK_DIR"
chmod 777 "$SOCK_DIR"

# 1. Tạo bridge br-dpdk với datapath netdev
echo "[1/3] Tạo bridge $BRIDGE (datapath_type=netdev)..."
ovs-vsctl --may-exist add-br $BRIDGE -- set bridge $BRIDGE datapath_type=netdev

# 2. Tạo 2 cổng vhost-user-client kết nối với Pods
echo "[2/3] Tạo 2 cổng $PORT0 và $PORT1..."
ovs-vsctl --may-exist add-port $BRIDGE $PORT0 -- \
    set Interface $PORT0 type=dpdkvhostuserclient options:vhost-server-path="$SOCK_DIR/$PORT0"

ovs-vsctl --may-exist add-port $BRIDGE $PORT1 -- \
    set Interface $PORT1 type=dpdkvhostuserclient options:vhost-server-path="$SOCK_DIR/$PORT1"

# 3. Cấu hình bảng luật định tuyến L3 trên OVS-DPDK (Tự động đọc từ manifests/ovs_flows.conf)
echo "[3/3] Nạp OpenFlow L3/L4 Routing & Filtering Table vào switch ảo $BRIDGE..."
ovs-ofctl del-flows $BRIDGE

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE=""
if [ -f "$SCRIPT_DIR/ovs_flows.conf" ]; then
    CONF_FILE="$SCRIPT_DIR/ovs_flows.conf"
elif [ -f "/manifests/ovs_flows.conf" ]; then
    CONF_FILE="/manifests/ovs_flows.conf"
fi

if [ -n "$CONF_FILE" ] && [ -f "$CONF_FILE" ]; then
    echo "  -> Đang đọc cấu hình từ: $CONF_FILE"
    declare -A group_prio
    declare -A group_act
    section=""
    loaded=0

    while IFS= read -r raw_line || [ -n "$raw_line" ]; do
        line="$(echo "$raw_line" | sed 's/#.*//; s/^[[:space:]]*//; s/[[:space:]]*$//')"
        [ -z "$line" ] && continue

        if [[ "$raw_line" =~ \[GROUPS_SECTION\] ]]; then
            section="GROUPS"
            continue
        elif [[ "$raw_line" =~ \[FILTERS_SECTION\] ]]; then
            section="FILTERS"
            continue
        fi

        if [ "$section" = "GROUPS" ]; then
            IFS=',' read -r g_name g_prio g_act <<< "$line"
            g_name="$(echo "$g_name" | xargs)"
            g_prio="$(echo "$g_prio" | xargs)"
            g_act="$(echo "$g_act" | xargs)"
            group_prio["$g_name"]="$g_prio"
            group_act["$g_name"]="$g_act"
        elif [ "$section" = "FILTERS" ]; then
            IFS=',' read -r f_name f_grp f_proto f_src_ip f_dst_ip f_src_port f_dst_port <<< "$line"
            f_name="$(echo "$f_name" | xargs)"
            f_grp="$(echo "$f_grp" | xargs)"
            f_proto="$(echo "$f_proto" | xargs | tr '[:upper:]' '[:lower:]')"
            f_src_ip="$(echo "$f_src_ip" | xargs)"
            f_dst_ip="$(echo "$f_dst_ip" | xargs)"
            f_src_port="$(echo "$f_src_port" | xargs)"
            f_dst_port="$(echo "$f_dst_port" | xargs)"

            prio="${group_prio[$f_grp]:-100}"
            act="${group_act[$f_grp]:-FORWARD}"

            flow_match="in_port=$PORT0"
            if [ "$f_proto" = "tcp" ]; then
                flow_match="$flow_match,tcp"
            elif [ "$f_proto" = "udp" ]; then
                flow_match="$flow_match,udp"
            else
                flow_match="$flow_match,ip"
            fi

            [ "$f_src_ip" != "*" ] && flow_match="$flow_match,nw_src=$f_src_ip"
            [ "$f_dst_ip" != "*" ] && flow_match="$flow_match,nw_dst=$f_dst_ip"
            [ "$f_src_port" != "*" ] && flow_match="$flow_match,tp_src=$f_src_port"
            [ "$f_dst_port" != "*" ] && flow_match="$flow_match,tp_dst=$f_dst_port"

            if [ "$act" = "FORWARD" ]; then
                flow_act="output:$PORT1"
            else
                flow_act="drop"
            fi

            echo "  -> Nạp Rule [$f_name] ($f_grp): prio=$prio, match=($flow_match) -> $flow_act"
            ovs-ofctl add-flow $BRIDGE "priority=$prio,$flow_match,actions=$flow_act"
            loaded=$((loaded + 1))
        fi
    done < "$CONF_FILE"
    echo "  -> Đã nạp thành công $loaded rules từ file cấu hình."
else
    echo "  -> [Cảnh báo] Không tìm thấy file ovs_flows.conf, áp dụng luật mặc định."
    ovs-ofctl add-flow $BRIDGE "priority=50,ip,in_port=$PORT0,actions=output:$PORT1"
fi

# Chiều về từ Pod 1 sang Pod 0 và cách ly ARP
echo "  -> [Priority 10/1] Nạp luật Return Path (Pod 1 -> Pod 0) và ARP Isolation..."
ovs-ofctl add-flow $BRIDGE "priority=10,in_port=$PORT1,actions=output:$PORT0"
ovs-ofctl add-flow $BRIDGE "priority=1,arp,actions=drop"

echo -e "\n====================================================="
echo -e "\033[1;32m[1] Cấu hình Switch ảo OVS-DPDK (ovs-vsctl show):\033[0m"
ovs-vsctl show

echo -e "\n====================================================="
echo -e "\033[1;32m[2] BẢNG ROUTE CỦA VSWITCH (OpenFlow 1.3 dump-flows):\033[0m"
ovs-ofctl -O OpenFlow13 dump-flows $BRIDGE

echo -e "\n====================================================="
echo -e "\033[1;32m[3] Thống kê cổng mạng vSwitch (dump-ports):\033[0m"
ovs-ofctl dump-ports $BRIDGE

echo -e "\n====================================================="
echo -e "\033[1;32m[4] Fast-Path Datapath Kernel/Netdev (dpctl show & flows):\033[0m"
ovs-appctl dpctl/show || true
ovs-appctl dpctl/dump-flows | head -20 || true
echo -e "=====================================================\n"

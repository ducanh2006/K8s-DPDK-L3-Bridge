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

# 3. Cấu hình bảng luật định tuyến L3 trên OVS-DPDK (Theo manifests/ovs_flows.conf)
echo "[3/3] Nạp OpenFlow L3 Routing Table vào switch ảo $BRIDGE..."
ovs-ofctl del-flows $BRIDGE

# DROP — priority cao nhất (200), áp dụng cho lưu lượng đi từ Pod 0
echo "  -> [Priority 200] Nạp luật DROP cho Meta (157.240.0.0/16) và AWS (96.127.0.0/16)..."
ovs-ofctl add-flow $BRIDGE "priority=200,ip,in_port=$PORT0,nw_dst=157.240.0.0/16,actions=drop"
ovs-ofctl add-flow $BRIDGE "priority=200,ip,in_port=$PORT0,nw_dst=96.127.0.0/16,actions=drop"

# FORWARD Google (Priority 100)
echo "  -> [Priority 100] Nạp luật FORWARD cho Google (172.217.0.0/16, 142.250.0.0/16)..."
ovs-ofctl add-flow $BRIDGE "priority=100,ip,in_port=$PORT0,nw_dst=172.217.0.0/16,actions=output:$PORT1"
ovs-ofctl add-flow $BRIDGE "priority=100,ip,in_port=$PORT0,nw_dst=142.250.0.0/16,actions=output:$PORT1"

# Default L3 + chiều về + cách ly ARP (để không nhiễu số liệu đếm gói L3)
echo "  -> [Priority 50/10/1] Nạp luật Default Forward, Reverse Path và ARP Drop..."
ovs-ofctl add-flow $BRIDGE "priority=50,ip,in_port=$PORT0,actions=output:$PORT1"
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

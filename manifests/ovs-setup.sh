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

# 3. Cấu hình OpenFlow rules chuyển tiếp hai chiều giữa Port 0 và Port 1
echo "[3/3] Nạp OpenFlow rules chuyển tiếp gói tin hai chiều..."
ovs-ofctl del-flows $BRIDGE
ovs-ofctl add-flow $BRIDGE "in_port=$PORT0, actions=output:$PORT1"
ovs-ofctl add-flow $BRIDGE "in_port=$PORT1, actions=output:$PORT0"

echo -e "\n====================================================="
echo -e "\033[1;32mTrạng thái Switch ảo OVS-DPDK:\033[0m"
ovs-vsctl show
echo -e "====================================================="
ovs-ofctl dump-flows $BRIDGE
echo -e "=====================================================\n"

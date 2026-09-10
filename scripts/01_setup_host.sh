#!/usr/bin/env bash
# ==============================================================================
# Script: 01_setup_host.sh
# Mục đích: Cài đặt và khởi tạo Open vSwitch với DPDK Datapath (OVS-DPDK)
# Yêu cầu: Chạy với quyền root: sudo bash scripts/01_setup_host.sh
# ==============================================================================

set -e

if [ "$(id -u)" -ne 0 ]; then
    echo -e "\033[1;31m[LỖI] Script này bắt buộc phải chạy với quyền root: sudo bash $0\033[0m"
    exit 1
fi

echo -e "\033[1;34m=====================================================\033[0m"
echo -e "\033[1;34m   CÀI ĐẶT VÀ KHỞI TẠO DỊCH VỤ OVS-DPDK              \033[0m"
echo -e "\033[1;34m=====================================================\033[0m"

# 1. Cài đặt openvswitch-switch-dpdk
if ! dpkg -s openvswitch-switch-dpdk >/dev/null 2>&1; then
    echo "[1/3] Đang cài đặt openvswitch-switch-dpdk từ kho apt..."
    apt-get update -qq
    apt-get install -y -qq openvswitch-switch-dpdk
else
    echo "[1/3] Gói openvswitch-switch-dpdk đã được cài đặt."
fi

# Chuyển đổi alternative sang ovs-vswitchd-dpdk nếu cần
update-alternatives --set ovs-vswitchd /usr/lib/openvswitch-switch-dpdk/ovs-vswitchd-dpdk 2>/dev/null || true

# 2. Cấu hình OVS kích hoạt DPDK
echo "[2/3] Cấu hình OVS kích hoạt DPDK và cấp bộ nhớ Hugepages..."
systemctl start openvswitch-switch || true

ovs-vsctl --no-wait set Open_vSwitch . other_config:dpdk-init=true
ovs-vsctl --no-wait set Open_vSwitch . other_config:dpdk-socket-mem="1024"
ovs-vsctl --no-wait set Open_vSwitch . other_config:dpdk-lcore-mask="0x1"

# 3. Khởi động lại dịch vụ OVS
echo "[3/3] Khởi động lại dịch vụ openvswitch-switch..."
systemctl restart openvswitch-switch

# Kiểm tra trạng thái DPDK trong OVS
DPDK_INIT_STATUS=$(ovs-vsctl get Open_vSwitch . dpdk_initialized 2>/dev/null || echo "false")
echo -e "\n====================================================="
echo -e "OVS DPDK Initialized Status: \033[1;32m$DPDK_INIT_STATUS\033[0m"
ovs-vswitchd --version | head -n 2
echo -e "=====================================================\n"

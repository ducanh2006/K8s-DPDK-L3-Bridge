#!/usr/bin/env bash
# ==============================================================================
# Script: 07_stop_k8s.sh
# Mục đích: Dừng toàn bộ các Pods DPDK và OVS, dọn dẹp sạch sẽ tài nguyên hệ thống
# ==============================================================================

set -e

echo -e "\033[1;33m=====================================================\033[0m"
echo -e "\033[1;33m   DỪNG TOÀN BỘ CỤM K8S DPDK & SWITCH ẢO OVS-DPDK    \033[0m"
echo -e "\033[1;33m=====================================================\033[0m"

echo -e "\n[1/2] Đang xóa các Pods (dpdk-pod0, dpdk-pod1, ovs-dpdk)..."
kubectl delete pod dpdk-pod0 dpdk-pod1 ovs-dpdk --ignore-not-found=true --grace-period=0 --force 2>/dev/null || true

echo -e "\n[2/2] Dọn dẹp các socket vhost-user tồn đọng..."
rm -f /var/run/openvswitch/vhost-user-* 2>/dev/null || sudo rm -f /var/run/openvswitch/vhost-user-* 2>/dev/null || true

echo -e "\n\033[1;32m=====================================================\033[0m"
echo -e "\033[1;32m[THÀNH CÔNG] Đã dừng và giải phóng toàn bộ tài nguyên!\033[0m"
echo -e "Để khởi động chạy lại bất cứ lúc nào: bash scripts/05_deploy_k8s.sh"
echo -e "=====================================================\n"

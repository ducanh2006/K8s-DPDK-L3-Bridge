#!/usr/bin/env bash
# ==============================================================================
# Script: 05_deploy_k8s.sh
# Mục đích: Triển khai 2 Pod DPDK (dpdk-pod1 trước, sau đó dpdk-pod0) lên K8s
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$ROOT_DIR"

echo -e "\033[1;34m[1/4] Xóa các Pod cũ nếu đang chạy...\033[0m"
kubectl delete pod dpdk-pod0 dpdk-pod1 ovs-dpdk --ignore-not-found=true --grace-period=0 --force 2>/dev/null || true

# Tắt service OVS ngoài host nếu đang chạy để tránh xung đột
if systemctl is-active --quiet openvswitch-switch 2>/dev/null; then
    echo -e "\033[1;33m[Lưu ý] Đang tắt openvswitch-switch trên máy host để chạy OVS trong Pod...\033[0m"
    sudo systemctl stop openvswitch-switch || true
fi

echo -e "\n\033[1;34m[2/4] Triển khai dpdk-pod1 (Traffic Sink) trước...\033[0m"
kubectl apply -f manifests/pod1.yaml

sleep 2

echo -e "\n\033[1;34m[3/4] Triển khai dpdk-pod0 (PCAP Replayer & L3 Router)...\033[0m"
kubectl apply -f manifests/pod0.yaml

sleep 2

echo -e "\n\033[1;34m[4/4] Triển khai ovs-dpdk (Switch ảo OVS trong Pod)...\033[0m"
kubectl apply -f manifests/ovs-pod.yaml

echo -e "\n\033[1;33mĐang kiểm tra trạng thái toàn bộ Pods:\033[0m"
kubectl get pods -l app=dpdk-l3-bridge -o wide

echo -e "\n\033[1;32m[THÀNH CÔNG] Đã apply xong 3 Pods (ovs-dpdk, dpdk-pod0, dpdk-pod1). Dùng 'k9s' hoặc 'kubectl get pods' để quan sát.\033[0m"

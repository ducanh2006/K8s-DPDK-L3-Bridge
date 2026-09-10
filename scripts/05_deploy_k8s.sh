#!/usr/bin/env bash
# ==============================================================================
# Script: 05_deploy_k8s.sh
# Mục đích: Triển khai 2 Pod DPDK (dpdk-pod1 trước, sau đó dpdk-pod0) lên K8s
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$ROOT_DIR"

echo -e "\033[1;34m[1/3] Xóa các Pod cũ nếu đang chạy...\033[0m"
kubectl delete pod dpdk-pod0 dpdk-pod1 --ignore-not-found=true --grace-period=0 --force 2>/dev/null || true

echo -e "\n\033[1;34m[2/3] Triển khai dpdk-pod1 (Responder) trước...\033[0m"
kubectl apply -f manifests/pod1.yaml

sleep 2

echo -e "\n\033[1;34m[3/3] Triển khai dpdk-pod0 (Forwarder / Sender)...\033[0m"
kubectl apply -f manifests/pod0.yaml

echo -e "\n\033[1;33mĐang kiểm tra trạng thái Pods:\033[0m"
kubectl get pods -l app=dpdk-l3-bridge -o wide

echo -e "\n\033[1;32m[THÀNH CÔNG] Đã apply xong manifests. Dùng lệnh 'kubectl get pods' hoặc 'k9s' để quan sát.\033[0m"

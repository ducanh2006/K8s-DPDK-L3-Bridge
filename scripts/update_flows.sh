#!/usr/bin/env bash
# ==============================================================================
# Script: update_flows.sh
# Mục đích: Tự động đồng bộ bảng luật từ manifests/ovs_flows.conf lên K8s ConfigMap,
#           nạp lại flows tức thì cho OVS-DPDK và khởi động lại Pods để áp dụng.
# Sử dụng:  bash scripts/update_flows.sh
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$ROOT_DIR"

CONF_FILE="manifests/ovs_flows.conf"

if [ ! -f "$CONF_FILE" ]; then
    echo -e "\033[1;31m[LỖI] Không tìm thấy file $CONF_FILE!\033[0m"
    exit 1
fi

echo -e "\033[1;34m=====================================================\033[0m"
echo -e "\033[1;34m   CẬP NHẬT BẢNG LUẬT SSOT TỰ ĐỘNG TỪ ovs_flows.conf \033[0m"
echo -e "\033[1;34m=====================================================\033[0m"

# 1. Tự động sinh ConfigMap trực tiếp từ ovs_flows.conf (Không cần file YAML trung gian)
echo -e "\n\033[1;34m[1/3] Đang đồng bộ $CONF_FILE lên Kubernetes ConfigMap 'ovs-flows'...\033[0m"
kubectl create configmap ovs-flows \
    --from-file=ovs_flows.conf="$CONF_FILE" \
    --dry-run=client -o yaml | kubectl apply -f -
echo "[OK] Đã cập nhật ConfigMap thành công."

# 2. Nạp lại Flow vào Switch ảo OVS-DPDK nếu đang chạy
if kubectl get pod ovs-dpdk &>/dev/null && [ "$(kubectl get pod ovs-dpdk -o jsonpath='{.status.phase}' 2>/dev/null)" = "Running" ]; then
    echo -e "\n\033[1;34m[2/3] Nạp lại OpenFlow rules vào switch ảo OVS-DPDK (Hot-Reload)... \033[0m"
    kubectl exec ovs-dpdk -- bash /manifests/ovs-setup.sh
    echo "[OK] OVS-DPDK đã nạp xong toàn bộ luật mới."
else
    echo -e "\n\033[1;33m[2/3] Pod ovs-dpdk chưa chạy, bỏ qua bước nạp trực tiếp vào OVS.\033[0m"
fi

# 3. Khởi động lại 2 Pods để nạp luật mới vào Runtime Flow Parser
echo -e "\n\033[1;34m[3/3] Khởi động lại dpdk-pod0 và dpdk-pod1 để cập nhật bộ đếm thống kê...\033[0m"
kubectl delete pod dpdk-pod0 dpdk-pod1 --ignore-not-found=true --grace-period=0 --force 2>/dev/null || true
kubectl apply -f manifests/pod1.yaml
sleep 1
kubectl apply -f manifests/pod0.yaml

echo -e "\n\033[1;32m=====================================================\033[0m"
echo -e "\033[1;32m[THÀNH CÔNG] Đã cập nhật xong toàn bộ hệ thống!\033[0m"
echo -e "\033[1;32mDùng 'bash scripts/06_verify_traffic.sh' để kiểm tra kết quả.\033[0m"
echo -e "\033[1;32m=====================================================\033[0m"

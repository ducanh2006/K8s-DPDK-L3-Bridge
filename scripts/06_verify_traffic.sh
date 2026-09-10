#!/usr/bin/env bash
# ==============================================================================
# Script: 06_verify_traffic.sh
# Mục đích: Kiểm tra và hiển thị log trao đổi gói tin hai chiều giữa Pod 0 và Pod 1
# ==============================================================================

set -e

echo -e "\033[1;34m=====================================================\033[0m"
echo -e "\033[1;34m      KIỂM TRA GIAO TIẾP HAI CHIỀU (OVS-DPDK)        \033[0m"
echo -e "\033[1;34m=====================================================\033[0m"

echo -e "\n\033[1;33m>>> [1/2] Log từ Pod 1 (Responder) - Lắng nghe & Bắn ACK:\033[0m"
kubectl logs dpdk-pod1 --tail=20 2>/dev/null || echo "Chưa có log từ dpdk-pod1"

echo -e "\n\033[1;33m>>> [2/2] Log từ Pod 0 (Forwarder) - Phát gói & Nhận ACK:\033[0m"
kubectl logs dpdk-pod0 --tail=20 2>/dev/null || echo "Chưa có log từ dpdk-pod0"

echo -e "\n\033[1;32m=====================================================\033[0m"
echo -e "Để theo dõi log thời gian thực:"
echo -e "  - Xem Pod 0 : kubectl logs -f dpdk-pod0"
echo -e "  - Xem Pod 1 : kubectl logs -f dpdk-pod1"
echo -e "  - Dùng K9s  : gõ lệnh 'k9s', chọn pod và nhấn 'l'"
echo -e "=====================================================\n"

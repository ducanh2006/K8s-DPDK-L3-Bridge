#!/usr/bin/env bash
# ==============================================================================
# Script: 06_verify_traffic.sh
# Mục đích: Kiểm tra và hiển thị log thông lượng PCAP & chuyển mạch L3 qua OVS-DPDK
# ==============================================================================

set -e

echo -e "\033[1;34m=====================================================\033[0m"
echo -e "\033[1;34m   KIỂM TRA HIỆU NĂNG L3 FORWARDING & THÔNG LƯỢNG    \033[0m"
echo -e "\033[1;34m=====================================================\033[0m"

echo -e "\n\033[1;33m>>> [1/2] Log từ Pod 0 (PCAP Replayer & L3 Classifier):\033[0m"
kubectl logs dpdk-pod0 --tail=15 2>/dev/null || echo "Chưa có log từ dpdk-pod0"

echo -e "\n\033[1;33m>>> [2/2] Log từ Pod 1 (High-Speed Traffic Sink & Inspector):\033[0m"
kubectl logs dpdk-pod1 --tail=15 2>/dev/null || echo "Chưa có log từ dpdk-pod1"

echo -e "\n\033[1;32m=====================================================\033[0m"
echo -e "Để theo dõi log thời gian thực:"
echo -e "  - Xem Pod 0 (Replayer & Drop stats): kubectl logs -f dpdk-pod0"
echo -e "  - Xem Pod 1 (Sink & Throughput):     kubectl logs -f dpdk-pod1"
echo -e "  - Dùng K9s:                          gõ lệnh 'k9s', chọn pod và nhấn 'l'"
echo -e "=====================================================\n"

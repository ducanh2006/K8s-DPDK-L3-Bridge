#!/usr/bin/env bash
# ==============================================================================
# Script: 06_verify_traffic.sh
# Mục đích: Báo cáo kết quả kiểm tra hệ thống theo đúng yêu cầu của Mentor:
#   [Khối 1] Bảng Route & Flow Counters của vSwitch (OVS-DPDK)
#   [Khối 2] Thống kê Port & Datapath Fast-Path (dump-ports & dpctl)
#   [Khối 3] Thống kê DPDK App (Throughput pps/Mbps + Top IP:Port Flows)
#   [Khối 4] Bằng chứng đối chứng toàn diện End-to-End (Proof E2E)
# ==============================================================================

set -e

BRIDGE="br-dpdk"

echo -e "\033[1;34m======================================================================\033[0m"
echo -e "\033[1;34m        K8s DPDK L3 BRIDGE: HỆ THỐNG KIỂM TRA & XÁC THỰC E2E           \033[0m"
echo -e "\033[1;34m======================================================================\033[0m"

# Hàm helper gọi lệnh OVS thông minh (ưu tiên qua kubectl exec nếu ovs-dpdk pod đang chạy)
run_ovs() {
    if kubectl get pod ovs-dpdk &>/dev/null && [ "$(kubectl get pod ovs-dpdk -o jsonpath='{.status.phase}' 2>/dev/null)" = "Running" ]; then
        kubectl exec ovs-dpdk -- "$@"
    elif command -v "$1" &>/dev/null; then
        sudo "$@"
    else
        echo "(Không tìm thấy $1)"
    fi
}

# ------------------------------------------------------------------------------
# KHỐI 1: BẢNG ROUTE CỦA VSWITCH (Theo yêu cầu: "Show cho a bảng route của vswitch")
# ------------------------------------------------------------------------------
echo -e "\n\033[1;33m[KHỐI 1] BẢNG ROUTE & L3 FLOW COUNTERS TRÊN VIRTUAL SWITCH (OVS-DPDK):\033[0m"
echo -e "Lệnh: ovs-ofctl -O OpenFlow13 dump-flows $BRIDGE"
echo -e "----------------------------------------------------------------------"
run_ovs ovs-ofctl -O OpenFlow13 dump-flows $BRIDGE || true

# ------------------------------------------------------------------------------
# KHỐI 2: THỐNG KÊ CỔNG VSWITCH & FAST-PATH DATAPATH
# ------------------------------------------------------------------------------
echo -e "\n\033[1;33m[KHỐI 2] THỐNG KÊ CỔNG MẠNG & DATAPATH OVS-DPDK:\033[0m"
echo -e "----------------------------------------------------------------------"
echo -e ">> Thống kê cổng mạng (Port counters):"
run_ovs ovs-ofctl dump-ports $BRIDGE || true
echo -e "\n>> Trạng thái Datapath Fast-Path (dpctl/show):"
run_ovs ovs-appctl dpctl/show 2>/dev/null || true

# ------------------------------------------------------------------------------
# KHỐI 3: THỐNG KÊ DPDK APP (Theo yêu cầu: "stats của dpdk app, thêm ip/port")
# ------------------------------------------------------------------------------
echo -e "\n\033[1;33m[KHỐI 3] THỐNG KÊ DPDK APP (Throughput + Top IP:Port Flows):\033[0m"
echo -e "----------------------------------------------------------------------"
echo -e ">> [Pod 0 - Forwarder/Streamer] 20 dòng log mới nhất:"
kubectl logs dpdk-pod0 --tail=20 2>/dev/null || echo "(Chưa có log từ dpdk-pod0)"

echo -e "\n>> [Pod 1 - Sink & Inspector] 20 dòng log mới nhất:"
kubectl logs dpdk-pod1 --tail=20 2>/dev/null || echo "(Chưa có log từ dpdk-pod1)"

# ------------------------------------------------------------------------------
# KHỐI 4: ĐỐI CHỨNG VÀ CHỨNG MINH HỆ THỐNG TOÀN DIỆN (Proof E2E)
# ------------------------------------------------------------------------------
echo -e "\n\033[1;32m======================================================================\033[0m"
echo -e "\033[1;32m   [KHỐI 4] BẰNG CHỨNG ĐỐI CHỨNG TOÀN DIỆN END-TO-END (PROOF E2E)     \033[0m"
echo -e "\033[1;32m======================================================================\033[0m"
cat << 'EOF'
  CÔNG THỨC KIỂM CHỨNG TOÀN VẸN HỆ THỐNG (DÙNG ĐỂ CHỤP VÀ ĐƯA VÀO SLIDE):
  1. Pod 0 TX  ≈ OVS Forward (Google + Default) + OVS Drop (Meta + AWS)
  2. OVS Forward ≈ Pod 1 RX
  3. Phân tích Flow IP:Port:
     - Tại Pod 0: Xuất hiện đầy đủ cả luồng Google, Meta, AWS gửi đi.
     - Tại vSwitch: n_packets của luật Drop (157.240.0.0/16, 96.127.0.0/16) tăng liên tục.
     - Tại Pod 1: CHỈ CÒN luồng Google và Default, HOÀN TOÀN KHÔNG CÓ Meta/AWS.
     => Bằng chứng vSwitch đã thực thi định tuyến L3 và hủy gói chính xác 100%!
EOF
echo -e "======================================================================\n"

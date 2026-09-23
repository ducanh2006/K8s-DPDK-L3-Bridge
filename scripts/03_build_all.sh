#!/usr/bin/env bash
# ==============================================================================
# Script: 03_build_all.sh
# Mục đích: Biên dịch cả thư viện common, pod0_app và pod1_app
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$ROOT_DIR"

echo -e "\033[1;34m[0/4] Sinh bảng group_stats_table.h từ manifests/ovs_flows.conf...\033[0m"
bash scripts/gen_group_table.sh

echo -e "\n\033[1;34m[1/4] Cấu hình và biên dịch ứng dụng bằng CMake...\033[0m"
cmake -B build
cmake --build build

echo -e "\n\033[1;34m[2/4] Chuẩn bị thư viện runtime DPDK (build/dpdk-libs)...\033[0m"
mkdir -p build/dpdk-libs
cp third_party/dpdk-24.11/build/lib/*.so* build/dpdk-libs/ 2>/dev/null || true
cp third_party/dpdk-24.11/build/drivers/*.so* build/dpdk-libs/ 2>/dev/null || true

echo -e "\n\033[1;34m[3/3] Kiểm tra binary kết quả:\033[0m"
ls -lh pod0-forwarder/pod0_app pod1-responder/pod1_app

echo -e "\n\033[1;32m[THÀNH CÔNG] Toàn bộ binary đã được biên dịch xong!\033[0m"

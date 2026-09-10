#!/usr/bin/env bash
# ==============================================================================
# Script: 04_build_docker.sh
# Mục đích: Đóng gói 2 Docker image pod0:latest và pod1:latest từ Root context
#           và nạp vào K3s image store
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$ROOT_DIR"

echo -e "\033[1;34m[1/3] Đảm bảo binary và thư viện DPDK đã được build...\033[0m"
bash scripts/03_build_all.sh

echo -e "\n\033[1;34m[2/3] Build Docker image pod0:latest...\033[0m"
docker build -f pod0-forwarder/Dockerfile -t pod0:latest .

echo -e "\n\033[1;34m[3/3] Build Docker image pod1:latest...\033[0m"
docker build -f pod1-responder/Dockerfile -t pod1:latest .

# Nếu đang chạy K3s, import trực tiếp vào K3s container runtime
if command -v k3s &>/dev/null && systemctl is-active --quiet k3s; then
    echo -e "\n\033[1;33mĐang nạp image vào K3s containerd store...\033[0m"
    docker save pod0:latest | sudo k3s ctr images import -
    docker save pod1:latest | sudo k3s ctr images import -
    echo "[OK] Đã import image vào K3s."
fi

echo -e "\n\033[1;32m[THÀNH CÔNG] Đã build xong 2 images: pod0:latest và pod1:latest\033[0m"

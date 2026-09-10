#!/usr/bin/env bash
# ==============================================================================
# Script: 00_setup_app_env.sh
# Mục đích: Thiết lập môi trường chạy tại /home/app (tận dụng phân vùng 97GB SSD)
#           - Cài đặt Docker và cấu hình data-root về /home/app/docker-data
#           - Cài đặt K3s (Kubernetes lightweight) với data-dir=/home/app/k8s-data
#           - Cài đặt K9s CLI dashboard vào /home/app/bin
#           - Cấp phát 1024 trang Hugepages 2MB (2GB RAM) cho DPDK
# Yêu cầu: Chạy với quyền root: sudo bash scripts/00_setup_app_env.sh
# ==============================================================================

set -e

if [ "$(id -u)" -ne 0 ]; then
    echo -e "\033[1;31m[LỖI] Script này bắt buộc phải chạy với quyền root: sudo bash $0\033[0m"
    exit 1
fi

ACTUAL_USER="${SUDO_USER:-$USER}"
USER_HOME=$(eval echo "~$ACTUAL_USER")

echo -e "\033[1;34m=====================================================\033[0m"
echo -e "\033[1;34m   BẮT ĐẦU THIẾT LẬP MÔI TRƯỜNG TẠI /home/app        \033[0m"
echo -e "\033[1;34m   User: $ACTUAL_USER | Home: $USER_HOME             \033[0m"
echo -e "\033[1;34m=====================================================\033[0m"

# 1. Tạo cấu trúc thư mục /home/app
echo -e "\n\033[1;33m[1/5] Khởi tạo các thư mục lưu trữ tại /home/app...\033[0m"
mkdir -p /home/app/docker-data
mkdir -p /home/app/k8s-data
mkdir -p /home/app/bin

chmod 755 /home/app
chmod 755 /home/app/bin
echo "[OK] Thư mục /home/app đã sẵn sàng."

# 2. Cài đặt và cấu hình Docker
echo -e "\n\033[1;33m[2/5] Cài đặt & cấu hình Docker (data-root -> /home/app/docker-data)...\033[0m"
if ! command -v docker &> /dev/null; then
    echo "Đang cài đặt docker.io từ kho apt..."
    apt-get update -qq
    apt-get install -y -qq docker.io
fi

# Cấu hình /etc/docker/daemon.json
mkdir -p /etc/docker
cat <<EOF > /etc/docker/daemon.json
{
  "data-root": "/home/app/docker-data"
}
EOF

# Thêm user vào nhóm docker để không cần gõ sudo khi dùng docker
usermod -aG docker "$ACTUAL_USER" || true
systemctl restart docker || true
echo "[OK] Docker đã được cấu hình lưu trữ tại /home/app/docker-data"

# 3. Cài đặt K3s (Kubernetes lightweight)
echo -e "\n\033[1;33m[3/5] Cài đặt K3s với data-dir=/home/app/k8s-data...\033[0m"
if ! command -v k3s &> /dev/null; then
    curl -sfL https://get.k3s.io | INSTALL_K3S_EXEC="--data-dir=/home/app/k8s-data --write-kubeconfig-mode=644" sh -
else
    echo "K3s đã được cài đặt, đang kiểm tra dịch vụ..."
    systemctl restart k3s || true
fi

# Thiết lập KUBECONFIG cho user hiện tại
mkdir -p "$USER_HOME/.kube"
cp /etc/rancher/k3s/k3s.yaml "$USER_HOME/.kube/config" || true
chown -R "$ACTUAL_USER:$ACTUAL_USER" "$USER_HOME/.kube" || true
chmod 600 "$USER_HOME/.kube/config" || true
echo "[OK] K3s đã khởi động thành công và liên kết kubeconfig cho user $ACTUAL_USER"

# 4. Cài đặt K9s CLI Dashboard
echo -e "\n\033[1;33m[4/5] Cài đặt K9s CLI Dashboard vào /home/app/bin...\033[0m"
if ! command -v k9s &> /dev/null && [ ! -f /home/app/bin/k9s ]; then
    echo "Đang tải K9s release mới nhất..."
    K9S_TAR="/tmp/k9s_Linux_amd64.tar.gz"
    curl -sL https://github.com/derailed/k9s/releases/latest/download/k9s_Linux_amd64.tar.gz -o "$K9S_TAR"
    tar -xzf "$K9S_TAR" -C /home/app/bin/ k9s
    rm -f "$K9S_TAR"
    chmod +x /home/app/bin/k9s
    ln -sf /home/app/bin/k9s /usr/local/bin/k9s
fi
echo "[OK] K9s CLI đã sẵn sàng (gõ 'k9s' trong terminal để mở)"

# 5. Cấp phát Hugepages cho DPDK
echo -e "\n\033[1;33m[5/5] Cấp phát 1024 trang Hugepages 2MB (2GB RAM) cho DPDK...\033[0m"
echo 1024 > /proc/sys/vm/nr_hugepages
echo "vm.nr_hugepages = 1024" > /etc/sysctl.d/99-hugepages.conf
sysctl --system > /dev/null 2>&1 || true

mkdir -p /dev/hugepages
mount -t hugetlbfs nodev /dev/hugepages 2>/dev/null || true
echo "[OK] Hugepages hiện tại:"
grep -i hugepages /proc/meminfo | head -n 3

echo -e "\n\033[1;32m=====================================================\033[0m"
echo -e "\033[1;32m   HOÀN TẤT THIẾT LẬP MÔI TRƯỜNG THÀNH CÔNG!         \033[0m"
echo -e "\033[1;32m=====================================================\033[0m"
echo -e "Kiểm tra nhanh:\n"
echo -e "  1. K3s Nodes : kubectl get nodes"
echo -e "  2. Docker    : docker info | grep 'Docker Root Dir'"
echo -e "  3. K9s       : k9s"
echo -e "  4. Dung lượng: df -h /home /"
echo -e "=====================================================\n"

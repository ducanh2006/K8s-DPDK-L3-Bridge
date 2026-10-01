# Tóm Tắt Kiến Trúc & Cơ Chế Hoạt Động Của Dự Án K8s-DPDK-L3-Bridge

---

## 1. Giới Thiệu & Mục Đích Dự Án

Dự án **K8s-DPDK-L3-Bridge** là hệ thống mạng hiệu năng cao (*High-Performance Networking*) chạy trong môi trường **Kubernetes**. Hệ thống kết hợp bộ công cụ **DPDK (Data Plane Development Kit)** và switch ảo **OVS-DPDK (Open vSwitch chạy trên nền DPDK)** nhằm thực hiện việc phát luồng, phân loại, chuyển mạch và lọc gói tin Layer 3/Layer 4 với độ trễ cực thấp (*microsecond level*) và thông lượng cao (*hàng triệu gói tin mỗi giây - Mpps*).

---

## 2. Kiến Trúc 3 Pods & Vai Trò Các Thành Phần

Hệ thống được thiết kế theo mô hình 3 Pods độc lập nhưng liên kết chặt chẽ:

```text
+---------------------------------------------------------------------------------------+
|                 Kubernetes ConfigMap: ovs-flows (ovs_flows.conf - SSOT)               |
+---------------------------------------------------------------------------------------+
                    │                                                  │
       mount /app/ovs_flows.conf                          mount /app/ovs_flows.conf
                    ▼                                                  ▼
+---------------------------------------+          +---------------------------------------+
|            dpdk-pod0                  |          |               dpdk-pod1               |
|  (PCAP Streamer & Passthrough)        |          |       (High-Speed Traffic Sink)       |
|                                       |          |                                       |
| - Port 0: net_pcap (đọc file mẫu)     |          | - Port 0: virtio-user0 (nhận từ OVS)  |
| - Port 1: virtio-user0 (đẩy sang OVS) |          | - Phân loại & đếm gói theo 8 nhóm     |
| - Thống kê Offered Load (8 nhóm SSOT) |          | - Kiểm chứng: 3 nhóm DROP = 0 gói,    |
+---------------------------------------+          |               5 nhóm FORWARD đầy đủ   |
                    │                              +---------------------------------------+
        vhost-user-0 (virtio-user)                                     ▲
                    ▼                                                  │ vhost-user-1
+----------------------------------------------------------------------│---------------+
|                                       ovs-dpdk                                       |
|                          (Switch ảo OVS chạy trong Pod)                              |
|                               Bridge: br-dpdk (netdev)                               |
|                                                                                      |
|  - Cổng Ingress: vhost-user-0 (dpdkvhostuserclient)                                  |
|  - Cổng Egress:  vhost-user-1 (dpdkvhostuserclient)                                  |
|  - Bảng OpenFlow L3/L4 Rules (8 nhóm):                                               |
|      * DROP   : facebook, aws, udp_other                                             |
|      * FORWARD: youtube, http, https, dns, default                                   |
+--------------------------------------------------------------------------------------+
```

1. **`dpdk-pod0` (PCAP Streamer & Passthrough):**
   - Đọc dữ liệu gói tin thực tế từ file PCAP (`balanced_traffic_sample.pcap`) qua PMD ảo `net_pcap`.
   - Phân loại gói tin theo 8 nhóm luật SSOT để thống kê lưu lượng đầu vào (*Offered Load*).
   - Đẩy toàn bộ gói tin sang cổng `virtio-user0` tới OVS-DPDK mà không thực hiện DROP tại Pod (chế độ transparent passthrough).
2. **`ovs-dpdk` (L3/L4 Switch & Router):**
   - Chạy tiến trình OVS với datapath chế độ người dùng `netdev` (DPDK Userspace Datapath).
   - Nạp các luật OpenFlow từ file cấu hình `ovs_flows.conf` (phân chia độ ưu tiên *priority* từ 8 xuống 1).
   - Quyết định DROP (hủy gói) hoặc FORWARD (đẩy sang cổng `vhost-user-1`).
3. **`dpdk-pod1` (Traffic Sink & Flow Inspector):**
   - Lắng nghe trên cổng `virtio-user0` kết nối từ OVS.
   - Bóc tách gói tin nhận được, đối chứng lại với 8 nhóm luật SSOT.
   - Xác thực tính chính xác: các luồng DROP tuyệt đối không lọt vào Pod 1, chỉ còn 5 luồng FORWARD.

---

## 3. Cơ Chế Giao Tiếp: Dự Án Dùng Unix Domain Socket Đúng Không?

**Câu trả lời:** **Đúng một phần.** Giao tiếp trong dự án được tách biệt rõ ràng thành 2 tầng (*Control Plane* và *Data Plane*):

### 3.1. Kênh Điều Khiển (Control Plane) → Dùng Unix Domain Socket
- **Vị trí socket:** `/var/run/openvswitch/vhost-user-0` và `/var/run/openvswitch/vhost-user-1`.
- **Cơ chế mount:** Được chia sẻ giữa Host và các Pod thông qua Kubernetes `hostPath` volume mount (`/var/run/openvswitch`).
- **Mô hình kết nối:**
  - **Pod 0 & Pod 1 đóng vai trò Server:** Tham số khởi tạo DPDK chỉ định `server=1`:
    ```bash
    --vdev=virtio_user0,path=/var/run/openvswitch/vhost-user-0,server=1
    ```
    Pod tạo và lắng nghe trên file Unix Domain Socket tương ứng.
  - **OVS-DPDK đóng vai trò Client:**
    ```bash
    ovs-vsctl add-port br-dpdk vhost-user-0 -- \
        set Interface vhost-user-0 type=dpdkvhostuserclient options:vhost-server-path="/var/run/openvswitch/vhost-user-0"
    ```
- **Nhiệm vụ của Unix Domain Socket:**
  - Bắt tay ban đầu (*Handshake*), đồng bộ trạng thái kết nối.
  - Đàm phán các tính năng ảo hóa VirtIO (*Feature negotiation*).
  - Trao đổi cấu hình hàng đợi (*Virtqueues*).
  - **Gửi quyền truy cập (File Descriptors - FDs) của vùng nhớ HugePages** từ tiến trình này sang tiến trình khác qua socket message (`SCM_RIGHTS`).

### 3.2. Luồng Dữ Liệu Gói Tin (Data Plane) → Dùng Bộ Nhớ Chia Sẻ (Shared Memory / HugePages)
- Khi quá trình bắt tay qua Unix Domain Socket hoàn tất, **gói tin KHÔNG truyền qua socket**. Nếu truyền hàng triệu gói tin qua socket thông thường (`read()`/`write()`), hệ thống sẽ bị thắt cổ chai do chi phí chuyển ngữ cảnh (*context switch*) và sao chép bộ nhớ trong nhân Linux (*kernel copy*).
- Thay vào đó, toàn bộ dữ liệu gói tin được truyền theo cơ chế **Zero-copy Inter-Process Communication (IPC)**:
  - Cả Pod 0, Pod 1 và OVS-DPDK đều ánh xạ (*mmap*) chung vào vùng nhớ **HugePages (2MB)** tại `/dev/hugepages`.
  - Gói tin được đưa vào cấu trúc vòng đệm **VirtIO vring** (bao gồm Descriptor Table, Available Ring, Used Ring) nằm trực tiếp trên vùng HugePages này.
  - Bên gửi chỉ cần đặt con trỏ mbuf vào ring và báo hiệu; bên nhận đọc trực tiếp từ vùng nhớ mà không qua bất kỳ lần sao chép bộ nhớ trung gian nào.

---

## 4. Các Công Nghệ & Kỹ Thuật Cốt Lõi Được Sử Dụng

| Kỹ thuật / Công nghệ | Vai trò trong dự án |
| :--- | :--- |
| **DPDK (Data Plane Development Kit)** | Bỏ qua kernel (*Kernel Bypass*), sử dụng Polling Mode Driver (PMD) để nhận/phát gói với độ trễ tối thiểu. |
| **OVS-DPDK (`datapath_type=netdev`)** | Chạy toàn bộ luồng chuyển mạch và bảng OpenFlow trong Userspace thay vì Kernel Module truyền thống. |
| **vhost-user / virtio-user** | Giao thức chuẩn hóa giao tiếp mạng hiệu năng cao giữa các tiến trình Userspace thông qua bộ nhớ chia sẻ. |
| **HugePages (2MB)** | Giảm thiểu số lượng bảng phân trang bộ nhớ, triệt tiêu hiện tượng *TLB Miss* khi xử lý lưu lượng mạng cực lớn. |
| **Single Source of Truth (SSOT)** | File cấu hình `ovs_flows.conf` định nghĩa 8 nhóm luật duy nhất được chia sẻ qua K8s ConfigMap cho cả 3 thành phần (Pod 0, OVS, Pod 1). |
| **PCAP Replay (`net_pcap`)** | Giả lập nguồn phát lưu lượng thực tế, cho phép tái hiện các kịch bản kiểm thử mạng lặp đi lặp lại một cách chuẩn xác. |

---

## 5. Quy Trình Vòng Đời Của Một Gói Tin (Packet Life Cycle)

1. **Khởi tạo:**
   - Pod 0 và Pod 1 khởi động, tạo Unix Domain Socket `/var/run/openvswitch/vhost-user-X`.
   - OVS-DPDK khởi động, kết nối tới 2 socket này, tiến hành bắt tay và cấu hình vùng nhớ chung (HugePages).
   - OVS nạp bảng luật OpenFlow từ `ovs_flows.conf`.
2. **Thu nhận & Phát luồng (Pod 0):**
   - DPDK gọi hàm `rte_eth_rx_burst()` kéo chùm gói tin từ file PCAP qua cổng `net_pcap`.
   - Pod 0 phân tích header L3/L4, cập nhật số liệu vào nhóm thống kê tương ứng.
   - Pod 0 gọi `rte_eth_tx_burst()` đẩy toàn bộ sang cổng `virtio_user0` (vào vùng nhớ HugePages chung với OVS).
3. **Chuyển mạch & Lọc gói (OVS-DPDK):**
   - OVS-DPDK đọc gói từ cổng `vhost-user-0`.
   - Đối chiếu trường header (IP nguồn, IP đích, giao thức, cổng L4) với bảng luật OpenFlow theo thứ tự ưu tiên:
     - **Khớp nhóm DROP (Priority 8, 7, 2 - Facebook, AWS, UDP):** Thực thi lệnh `actions=drop`, giải phóng gói tin.
     - **Khớp nhóm FORWARD (Priority 6, 5, 4, 3, 1 - YouTube, HTTP, HTTPS, DNS, Default):** Thực thi lệnh `actions=output:vhost-user-1`, đẩy gói sang vòng đệm của cổng `vhost-user-1`.
4. **Nhận & Kiểm tra (Pod 1):**
   - Pod 1 gọi `rte_eth_rx_burst()` trên cổng `virtio_user0` để lấy các gói tin mà OVS đã forward.
   - Tiến hành kiểm tra và in thống kê định kỳ (mỗi 1 giây và tổng kết mỗi 20 giây):
     - Xác nhận tỷ lệ throughput (pps, Mbps).
     - Đối chứng số lượng gói nhận được với số liệu phát ban đầu từ Pod 0 để kiểm tra tính toàn vẹn của hệ thống.

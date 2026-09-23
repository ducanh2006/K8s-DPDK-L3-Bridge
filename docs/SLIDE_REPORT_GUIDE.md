# Hướng Dẫn Soạn Slide Thuyết Trình Báo Cáo Dự Án
## Đề tài: High-Performance L3 Bridge trên Kubernetes với DPDK & OVS-DPDK

Tài liệu này được biên soạn bám sát theo 3 yêu cầu cốt lõi từ Mentor:
1. **Mô tả hệ thống (System Architecture & Topology)**
2. **Cách làm & Kỹ thuật triển khai (Implementation & Technology Stack)**
3. **Kết quả kiểm thử & Bằng chứng số liệu (Test Results, Routing Table & DPDK Stats)**

---

# SLIDE 1: TIÊU ĐỀ BÁO CÁO
- **Tên đề tài**: Cầu nối mạng Layer 3 hiệu năng cao trên Kubernetes sử dụng DPDK và Open vSwitch (OVS-DPDK)
- **Người thực hiện**: [Tên của bạn]
- **Người hướng dẫn (Mentor)**: [Tên Mentor]
- **Thời gian**: Tháng 09/2026

---

# PHẦN 1: MÔ TẢ HỆ THỐNG (SYSTEM OVERVIEW)

### SLIDE 2: Bài toán & Kiến trúc Tổng quan (Architecture Diagram)
* **Mục tiêu bài toán**: Xây dựng cầu nối mạng chuyển mạch gói tin tốc độ cao (Kernel-Bypass) giữa các Container trong Kubernetes, thực thi chính sách định tuyến và tường lửa L3 trực tiếp trên switch ảo.
* **Mô hình kiến trúc 3 tầng**:
  ```text
  ┌───────────────────────────────────────────────────────────────────────────┐
  │                           KUBERNETES NODE                                 │
  │                                                                           │
  │  ┌───────────────────────┐                     ┌───────────────────────┐  │
  │  │        Pod 0          │                     │        Pod 1          │  │
  │  │   (DPDK Forwarder)    │                     │  (Traffic Inspector)  │  │
  │  │  - net_pcap PMD       │                     │  - Flow Analyzer      │  │
  │  │  - Flow Tracker       │                     │  - Top IP:Port Stats  │  │
  │  └──────────┬────────────┘                     └──────────▲────────────┘  │
  │             │ virtio-user-0                               │ virtio-user-1 │
  │             ▼                                             │               │
  │  ┌────────────────────────────────────────────────────────┴────────────┐  │
  │  │                Open vSwitch với DPDK Datapath (OVS-DPDK)            │  │
  │  │  - Cổng vhost-user-client (Zero-Copy IPC qua 2MB Hugepages)         │  │
  │  │  - Bảng OpenFlow L3 Routing: Match IP CIDR -> FORWARD hoặc DROP     │  │
  │  └─────────────────────────────────────────────────────────────────────┘  │
  └───────────────────────────────────────────────────────────────────────────┘
  ```

### SLIDE 3: Vai trò & Trách nhiệm các thành phần
1. **Pod 0 (Traffic Generator & Passthrough Streamer)**:
   - Đọc dữ liệu mẫu từ file PCAP thực tế bằng PMD `net_pcap` (`infinite_rx=1`).
   - Phân tích 5-tuple và truyền tải toàn bộ lưu lượng qua socket `virtio_user0` sang OVS-DPDK.
2. **Virtual Switch (OVS-DPDK L3 Router)**:
   - Đóng vai trò hạt nhân định tuyến L3 cho toàn cụm.
   - Tiếp nhận luồng gói từ Pod 0, tra cứu bảng OpenFlow Flow Table để đưa ra quyết định chuyển mạch (`actions=output:vhost-user-1`) hoặc hủy gói bảo mật (`actions=drop`).
3. **Pod 1 (Traffic Sink & Inspector)**:
   - Nhận lưu lượng đã qua xử lý từ OVS qua cổng `virtio_user0`.
   - Phân tích protocol (TCP/UDP/ICMP), tổng hợp thống kê Top IP:Port để chứng minh OVS đã lọc chính xác.

---

# PHẦN 2: CÁCH LÀM & KỸ THUẬT TRIỂN KHAI (METHODOLOGY)

### SLIDE 4: Công nghệ & Kỹ thuật Tăng tốc Phần cứng (Hardware Acceleration)
* **DPDK Poll Mode Driver (PMD)**: Loại bỏ hoàn toàn ngắt phần cứng (interrupts) và kernel context switch, ứng dụng chiếm trọn CPU core để kéo burst 64 gói/lần với độ trễ cực thấp.
* **Cơ chế IPC Zero-Copy qua vhost-user**:
  - Giao tiếp giữa Pod và OVS không đi qua TCP/IP stack của Linux kernel.
  - Sử dụng chung vùng nhớ chia sẻ **Hugepages 2MB** (`/dev/hugepages`).
* **Tiến hóa kiến trúc (Storytelling so sánh)**:
  - *Giai đoạn 1 (Thử nghiệm)*: Định tuyến cục bộ tại Pod bằng thư viện DPDK LPM.
  - *Giai đoạn 2 (Chuẩn hóa Telco/NFV)*: Tách rời Data Plane và Control Plane, chuyển toàn bộ chính sách L3 Routing sang Switch ảo OVS-DPDK để quản lý tập trung và tăng hiệu năng chuyển mạch.

### SLIDE 5: Thiết kế Bảng Luật OpenFlow L3 (OVS Routing Table)
Hệ thống sử dụng bảng luật phân cấp theo độ ưu tiên (`priority`) chặt chẽ:

| Priority | Điều kiện khớp (Matching Criteria) | Hành động (Action) | Mục đích / Giải thích |
| :---: | :--- | :---: | :--- |
| **200** | `in_port=vhost-user-0, nw_dst=157.240.0.0/16` | **DROP** | Chặn toàn bộ gói tin đến Meta / Facebook tại vSwitch |
| **200** | `in_port=vhost-user-0, nw_dst=96.127.0.0/16` | **DROP** | Chặn toàn bộ gói tin đến dải test Amazon AWS |
| **100** | `in_port=vhost-user-0, nw_dst=172.217.0.0/16` | **FORWARD** | Chuyển tiếp tốc độ cao các dịch vụ Google sang Pod 1 |
| **100** | `in_port=vhost-user-0, nw_dst=142.250.0.0/16` | **FORWARD** | Chuyển tiếp Google Cloud & APIs sang Pod 1 |
| **50** | `in_port=vhost-user-0, ip` | **FORWARD** | Chuyển tiếp mặc định các dải IP thông thường khác |
| **10** | `in_port=vhost-user-1` | **FORWARD** | Kênh phản hồi từ Pod 1 về lại Pod 0 |
| **1** | `arp` | **DROP** | Cách ly gói ARP broadcast để tránh sai lệch thống kê L3 |

### SLIDE 6: Module Flow Tracker IP:Port Zero-Allocation
* **Vấn đề**: Trong luồng xử lý hàng triệu gói tin mỗi giây, việc cấp phát động `malloc()` cho mỗi gói sẽ làm sụt giảm thông lượng nghiêm trọng.
* **Giải pháp**: Thiết kế bảng băm tĩnh cố định (1024 entries), trích xuất trực tiếp IPv4 header và TCP/UDP header ngay trên mbuf không qua sao chép.
* **Thống kê 2 chu kỳ**:
  - Chu kỳ 1 giây: Cập nhật Throughput (pps, Mbps) và lỗi hàng đợi phần cứng (`imissed`).
  - Chu kỳ 20 giây: Xuất bảng Top-5 luồng hoạt động mạnh nhất theo cặp `Src IP:Port -> Dst IP:Port` (Mbps hiển thị 3 chữ số thập phân).

---

# PHẦN 3: KẾT QUẢ TEST & CHỨNG MINH (E2E TEST RESULTS)

### SLIDE 7: Show Bảng Route & Flow Counters của vSwitch
*(Chụp màn hình kết quả chạy lệnh: `sudo ovs-ofctl -O OpenFlow13 dump-flows br-dpdk`)*

**Bảng số liệu đối chứng mẫu**:
```text
cookie=0x0, duration=120.5s, table=0, n_packets=82150, n_bytes=78920100, priority=200,ip,in_port="vhost-user-0",nw_dst=157.240.0.0/16 actions=drop
cookie=0x0, duration=120.5s, table=0, n_packets=31200, n_bytes=29952000, priority=200,ip,in_port="vhost-user-0",nw_dst=96.127.0.0/16 actions=drop
cookie=0x0, duration=120.5s, table=0, n_packets=245000, n_bytes=235200000, priority=100,ip,in_port="vhost-user-0",nw_dst=172.217.0.0/16 actions=output:"vhost-user-1"
cookie=0x0, duration=120.5s, table=0, n_packets=198000, n_bytes=190080000, priority=100,ip,in_port="vhost-user-0",nw_dst=142.250.0.0/16 actions=output:"vhost-user-1"
```
* **Nhận xét quan trọng**:
  - Hai luật Priority 200 có số gói `n_packets` tăng liên tục $\rightarrow$ Chứng minh vSwitch đã trực tiếp Drop hơn 113,000 gói tin Meta/AWS.
  - Các luật Priority 100 chuyển tiếp thành công hàng trăm ngàn gói tin Google sang cổng của Pod 1.

### SLIDE 8: Thống kê DPDK App với Chi tiết IP và Port
*(Chụp màn hình terminal log của Pod 0 và Pod 1 từ lệnh `bash scripts/06_verify_traffic.sh`)*

**Tại Pod 0 (Forwarder - Phát lưu lượng)**:
```text
[Pod0-TX] ---------- Top Active IP:Port Traffic Flows (5s Period) ----------
[Pod0-TX] Prot | Source IP:Port        -> Dest IP:Port          | Period Pkt | Throughput
[Pod0-TX] TCP  | 192.168.1.10:52410    -> 172.217.182.112:443   | 45,120     | 43.32 Mbps
[Pod0-TX] TCP  | 192.168.1.10:52412    -> 157.240.4.224:443     | 32,500     | 31.20 Mbps
[Pod0-TX] TCP  | 192.168.1.10:52414    -> 142.250.14.95:443     | 28,400     | 27.26 Mbps
[Pod0-TX] UDP  | 192.168.1.10:53000    -> 8.8.8.8:53            | 12,300     | 11.81 Mbps
```

**Tại Pod 1 (Sink - Nhận và đối soát lưu lượng)**:
```text
[Pod1-RX] ---------- Top Active IP:Port Traffic Flows (5s Period) ----------
[Pod1-RX] Prot | Source IP:Port        -> Dest IP:Port          | Period Pkt | Throughput
[Pod1-RX] TCP  | 192.168.1.10:52410    -> 172.217.182.112:443   | 45,120     | 43.32 Mbps
[Pod1-RX] TCP  | 192.168.1.10:52414    -> 142.250.14.95:443     | 28,400     | 27.26 Mbps
[Pod1-RX] UDP  | 192.168.1.10:53000    -> 8.8.8.8:53            | 12,300     | 11.81 Mbps
(HOÀN TOÀN KHÔNG CÓ sự xuất hiện của IP 157.240.x.x hay 96.127.x.x!)
```

### SLIDE 9: Bằng chứng Đối chứng Toàn diện (Proof E2E)
* **Khẳng định tính đúng đắn qua công thức bảo toàn gói tin**:
  $$\text{Tổng gói Pod 0 TX} = \text{Gói OVS Drop} + \text{Gói OVS Forward} = \text{Gói OVS Drop} + \text{Tổng gói Pod 1 RX}$$
* **Chỉ số hiệu năng (KPIs)**:
  - Thông lượng trung bình: Đạt xấp xỉ tốc độ bão hòa đường truyền PCAP/vhost-user (> 100,000 pps).
  - Tỷ lệ mất gói ngoài ý muốn: $0\%$ (`imissed = 0`, `oerrors = 0`).
  - Độ chính xác lọc gói: $100\%$ các dải IP bị cấm bị drop sạch sẽ tại switch ảo.

---

# SLIDE 10: KẾT LUẬN & HƯỚNG PHÁT TRIỂN
1. **Kết quả đạt được**:
   - Triển khai thành công kiến trúc chuyển mạch L3 OVS-DPDK trên nền Kubernetes.
   - Thỏa mãn toàn bộ yêu cầu kỹ thuật của Mentor: Show bảng route của vSwitch, show thống kê DPDK app chi tiết theo IP/Port.
   - Đảm bảo hiệu năng cao nhờ tận dụng tối đa DPDK PMD, Hugepages và vhost-user.
2. **Hướng mở rộng**:
   - Tích hợp động bộ điều khiển SDN (SDN Controller như Ryu / OpenDaylight) để đẩy flow tự động qua giao thức OpenFlow.
   - Thử nghiệm trên phần cứng NIC vật lý hỗ trợ SR-IOV.

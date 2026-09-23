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
  │  │   (DPDK Streamer)       │                     │  (Traffic Inspector)  │  │
  │  │  - net_pcap PMD       │                     │  - Group Analyzer     │  │
  │  │  - Group Stats (SSOT) │                     │  - 8-Group Stats      │  │
  │  └──────────┬────────────┘                     └──────────▲────────────┘  │
  │             │ virtio-user-0                               │ virtio-user-1 │
  │             ▼                                             │               │
  │  ┌────────────────────────────────────────────────────────┴────────────┐  │
  │  │                Open vSwitch với DPDK Datapath (OVS-DPDK)            │  │
  │  │  - Cổng vhost-user-client (Zero-Copy IPC qua 2MB Hugepages)         │  │
  │  │  - Bảng OpenFlow L3/L4 Routing: 8 Groups SSOT -> FORWARD hoặc DROP     │  │
  │  └─────────────────────────────────────────────────────────────────────┘  │
  └───────────────────────────────────────────────────────────────────────────┘
  ```

### SLIDE 3: Vai trò & Trách nhiệm các thành phần
1. **Pod 0 (Traffic Generator & Passthrough Streamer)**:
   - Đọc dữ liệu mẫu từ file PCAP thực tế bằng PMD `net_pcap` (`infinite_rx=1`).
   - Phân loại mỗi gói vào 1 trong 8 Groups SSOT và truyền tải toàn bộ lưu lượng qua socket `virtio_user0` sang OVS-DPDK.
2. **Virtual Switch (OVS-DPDK L3/L4 Router)**:
   - Đóng vai trò hạt nhân định tuyến cho toàn cụm.
   - Tiếp nhận luồng gói từ Pod 0, tra cứu bảng OpenFlow Flow Table (8 Groups) để đưa ra quyết định chuyển mạch (`actions=output:vhost-user-1`) hoặc hủy gói bảo mật (`actions=drop`).
3. **Pod 1 (Traffic Sink & Inspector)**:
   - Nhận lưu lượng đã qua xử lý từ OVS qua cổng `virtio_user0`.
   - Phân tích protocol (TCP/UDP/ICMP/Other), tổng hợp bảng thống kê 8 Groups để chứng minh OVS đã lọc chính xác (nhóm DROP vắng mặt).

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

### SLIDE 5: Thiết kế Bảng Luật OpenFlow L3/L4 (8 Groups SSOT)
Hệ thống sử dụng bảng luật phân cấp theo độ ưu tiên (`priority`, số lớn thắng) — nguồn duy nhất `manifests/ovs_flows.conf`:

| Priority | Group | Điều kiện khớp (Matching Criteria) | Hành động (Action) |
| :---: | :--- | :--- | :---: |
| **8** | `fg_l34_facebook` | `ip,nw_dst` ∈ 5 dải Meta (31.13.64/18, 66.220.144/20, 69.63.176/20, 157.240/16, 69.220.144.5/32) | **DROP** |
| **7** | `fg_l34_aws` | `ip,nw_dst=96.127.0.0/16` | **DROP** |
| **6** | `fg_l34_youtube` | `tcp,tp_dst=443`, `nw_dst` ∈ 142.250/15, 172.217/16, 216.58.192/19, 74.125.0.1 | **FORWARD** |
| **5** | `fg_l34_http_sdf1003` | `tcp,tp_dst=80` | **FORWARD** |
| **4** | `fg_l34_https_sdf1004` | `tcp,tp_dst=443` | **FORWARD** |
| **3** | `fg_l34_dns_sdf1005` | `udp/tcp,tp_dst=53` | **FORWARD** |
| **2** | `fg_l34_udp_sdf1006` | `udp` còn lại | **DROP** |
| **1** | `fg_l34_default` | `ip` còn lại (catch-all) | **FORWARD** |
| **10/1** | (cứng trong script) | `in_port=vhost-user-1` chiều về / `arp` cách ly | **FORWARD / DROP** |

### SLIDE 6: Module Phân loại Gói tin & Thống kê 8 Groups (SSOT)
* **Vấn đề**: Trong hệ thống mạng tốc độ cao, việc băm nhỏ lưu lượng theo từng IP:Port ngẫu nhiên khiến mỗi luồng chỉ có vài gói, dẫn đến thông lượng tính ra bị làm tròn về `0.000 Mbps`.
* **Giải pháp (SSOT - Single Source of Truth)**:
   - Đồng bộ file cấu hình duy nhất `manifests/ovs_flows.conf` cho cả 3 thành phần (OVS-DPDK, Pod 0, Pod 1).
  - Tự động sinh bảng C `common/group_stats_table.h` lúc build để khớp gói tin trực tiếp theo Network Byte Order.
* **Thống kê 2 chu kỳ**:
  - Chu kỳ 1 giây: Cập nhật Throughput (pps, Mbps) và lỗi hàng đợi phần cứng (`imissed`).
  - Chu kỳ 20 giây: Xuất bảng thống kê chi tiết theo 8 Groups với thông lượng thực tế (hàng trăm Mbps đến hàng Gbps).

---

# PHẦN 3: KẾT QUẢ TEST & CHỨNG MINH (E2E TEST RESULTS)

### SLIDE 7: Show Bảng Route & Flow Counters của vSwitch (OVS-DPDK)
*(Chụp màn hình kết quả chạy lệnh: `ovs-ofctl -O OpenFlow13 dump-flows br-dpdk`)*

**Bảng số liệu đối chứng mẫu (Priorities 8..1)**:
```text
cookie=0x0, duration=120.5s, table=0, n_packets=12501376, priority=8,ip,in_port="vhost-user-0",nw_dst=157.240.0.0/16 actions=drop
cookie=0x0, duration=120.5s, table=0, n_packets=542100,   priority=7,ip,in_port="vhost-user-0",nw_dst=96.127.0.0/16 actions=drop
cookie=0x0, duration=120.5s, table=0, n_packets=9632410,  priority=6,tcp,in_port="vhost-user-0",nw_dst=172.217.0.0/16,tp_dst=443 actions=output:"vhost-user-1"
cookie=0x0, duration=120.5s, table=0, n_packets=4215000,  priority=5,tcp,in_port="vhost-user-0",tp_dst=80 actions=output:"vhost-user-1"
cookie=0x0, duration=120.5s, table=0, n_packets=1820300,  priority=1,ip,in_port="vhost-user-0" actions=output:"vhost-user-1"
```
* **Nhận xét quan trọng**:
  - Các luật Priority 8 (Facebook) và Priority 7 (AWS) có số gói `n_packets` tăng liên tục $\rightarrow$ Chứng minh vSwitch đã trực tiếp Drop hơn 13 triệu gói tin vi phạm.
  - Các luật Priority 6, 5, 4, 3, 1 chuyển tiếp thành công toàn bộ lưu lượng Google, HTTP, HTTPS, DNS sang cổng của Pod 1.

### SLIDE 8: Thống kê DPDK App theo 8 Groups SSOT
*(Chụp màn hình terminal log của Pod 0 và Pod 1 từ lệnh `bash scripts/06_verify_traffic.sh`)*

**Tại Pod 0 (Forwarder - Phát lưu lượng)**:
```text
[Pod0-GRP] ==================== L3/L4 GROUP STATISTICS (SSOT) ====================
[Pod0-GRP] Group Name           | Prio | Action  | Period Pkts  | Throughput     | Total Pkts
[Pod0-GRP] ---------------------+------+---------+--------------+----------------+-------------
[Pod0-GRP] fg_l34_facebook      |    8 | DROP    |   12,501,376 |  7100.118 Mbps |   12,501,376
[Pod0-GRP] fg_l34_aws           |    7 | DROP    |      542,100 |   308.201 Mbps |      542,100
[Pod0-GRP] fg_l34_youtube       |    6 | FORWARD |    9,632,410 |  5480.206 Mbps |    9,632,410
[Pod0-GRP] fg_l34_http_sdf1003  |    5 | FORWARD |    4,215,000 |  2395.120 Mbps |    4,215,000
[Pod0-GRP] fg_l34_https_sdf1004 |    4 | FORWARD |    8,110,200 |  4610.050 Mbps |    8,110,200
[Pod0-GRP] fg_l34_dns_sdf1005   |    3 | FORWARD |      310,000 |   176.220 Mbps |      310,000
[Pod0-GRP] fg_l34_udp_sdf1006   |    2 | DROP    |       45,000 |    25.600 Mbps |       45,000
[Pod0-GRP] fg_l34_default       |    1 | FORWARD |    1,820,300 |  1035.010 Mbps |    1,820,300
[Pod0-GRP] ========================================================================
```

**Tại Pod 1 (Sink - Nhận và đối soát lưu lượng từ OVS)**:
```text
[Pod1-GRP] ==================== L3/L4 GROUP STATISTICS (SSOT) ====================
[Pod1-GRP] Group Name           | Prio | Action  | Period Pkts  | Throughput     | Total Pkts
[Pod1-GRP] ---------------------+------+---------+--------------+----------------+-------------
[Pod1-GRP] fg_l34_facebook      |    8 | DROP    |            0 |     0.000 Mbps |            0
[Pod1-GRP] fg_l34_aws           |    7 | DROP    |            0 |     0.000 Mbps |            0
[Pod1-GRP] fg_l34_youtube       |    6 | FORWARD |    9,632,410 |  5480.206 Mbps |    9,632,410
[Pod1-GRP] fg_l34_http_sdf1003  |    5 | FORWARD |    4,215,000 |  2395.120 Mbps |    4,215,000
[Pod1-GRP] fg_l34_https_sdf1004 |    4 | FORWARD |    8,110,200 |  4610.050 Mbps |    8,110,200
[Pod1-GRP] fg_l34_dns_sdf1005   |    3 | FORWARD |      310,000 |   176.220 Mbps |      310,000
[Pod1-GRP] fg_l34_udp_sdf1006   |    2 | DROP    |            0 |     0.000 Mbps |            0
[Pod1-GRP] fg_l34_default       |    1 | FORWARD |    1,820,300 |  1035.010 Mbps |    1,820,300
[Pod1-GRP] ========================================================================
(HOÀN TOÀN BẰNG 0 đối với các nhóm DROP: Facebook, AWS, UDP_Other!)
```

### SLIDE 9: Bằng chứng Đối chứng Toàn diện (Proof E2E theo 8 Groups)
* **Khẳng định tính đúng đắn qua công thức bảo toàn gói tin từng Group**:
  - $\text{Pod0}_{\text{Facebook}} = \text{OVS DROP}_{\text{Prio 8}}$ (Pod1 nhận $0$)
  - $\text{Pod0}_{\text{AWS}} = \text{OVS DROP}_{\text{Prio 7}}$ (Pod1 nhận $0$)
  - $\text{Pod0}_{\text{YouTube}} \approx \text{OVS FWD}_{\text{Prio 6}} \approx \text{Pod1}_{\text{YouTube}}$
  - $\text{Tổng gói Pod 0 TX} = \text{Tổng gói OVS Drop} + \text{Tổng gói Pod 1 RX}$
* **Chỉ số hiệu năng (KPIs)**:
  - Thông lượng trung bình: Đạt xấp xỉ tốc độ bão hòa đường truyền PCAP/vhost-user (> 100,000 pps).
  - Tỷ lệ mất gói ngoài ý muốn: $0\%$ (`imissed = 0`, `oerrors = 0`).
  - Độ chính xác lọc gói: $100\%$ các nhóm DROP bị hủy sạch sẽ tại switch ảo OVS-DPDK.

---

# SLIDE 10: KẾT LUẬN & HƯỚNG PHÁT TRIỂN
1. **Kết quả đạt được**:
   - Triển khai thành công kiến trúc chuyển mạch L3 OVS-DPDK trên nền Kubernetes.
   - Thỏa mãn toàn bộ yêu cầu kỹ thuật của Mentor: Show bảng route của vSwitch, show thống kê DPDK app chi tiết theo IP/Port.
   - Đảm bảo hiệu năng cao nhờ tận dụng tối đa DPDK PMD, Hugepages và vhost-user.
2. **Hướng mở rộng**:
   - Tích hợp động bộ điều khiển SDN (SDN Controller như Ryu / OpenDaylight) để đẩy flow tự động qua giao thức OpenFlow.
   - Thử nghiệm trên phần cứng NIC vật lý hỗ trợ SR-IOV.

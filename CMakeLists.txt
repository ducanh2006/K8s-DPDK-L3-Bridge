cmake_minimum_required(VERSION 3.16)
project(k8s_dpdk_l3_bridge C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Tự động xuất file build/compile_commands.json cho VSCode (Clangd) đọc gợi ý code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Cấu hình cờ tối ưu hóa và kiến trúc CPU tương thích với DPDK intrinsics
add_compile_options(-O3 -g -Wall -Wextra -march=native)

# 1. Tìm thư viện DPDK trong third_party hoặc hệ thống
find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/meson-uninstalled:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(DPDK REQUIRED libdpdk)

# 2. Thư mục include và library
include_directories(
    ${CMAKE_SOURCE_DIR}/common
    ${DPDK_INCLUDE_DIRS}
)

link_directories(
    ${DPDK_LIBRARY_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/lib
    ${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/drivers
)

# 3. Cấu hình rpath để binary tự tìm thấy shared libraries mà không cần biến môi trường
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH
    "${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/lib"
    "${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/drivers"
    "/app/dpdk-libs"
)

# Linker flags nhúng tĩnh các driver PMD và mempool ring vào binary
set(DRIVER_LDFLAGS
    "-Wl,--whole-archive"
    "${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/drivers/librte_net_pcap.a"
    "${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/drivers/librte_net_virtio.a"
    "${CMAKE_SOURCE_DIR}/third_party/dpdk-24.11/build/drivers/librte_mempool_ring.a"
    "-Wl,--no-whole-archive"
)
set(EXTRA_LIBS rte_net rte_ethdev rte_kvargs rte_bus_vdev rte_bus_pci pthread numa dl m pcap)

# 4. Thư viện dùng chung common
add_library(common STATIC
    common/dpdk_init.c
    common/l3_table.c
    common/flow_tracker.c
)
target_include_directories(common PUBLIC ${CMAKE_SOURCE_DIR}/common ${DPDK_INCLUDE_DIRS})
target_link_libraries(common PUBLIC ${DPDK_LIBRARIES} ${DRIVER_LDFLAGS} ${EXTRA_LIBS})

# 5. Ứng dụng Pod 0: Forwarder / Sender
add_executable(pod0_app pod0-forwarder/main.c)
set_target_properties(pod0_app PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/pod0-forwarder")
target_link_libraries(pod0_app PRIVATE common ${DPDK_LIBRARIES} ${DRIVER_LDFLAGS} ${EXTRA_LIBS})

# 6. Ứng dụng Pod 1: Responder / Echo-ACK
add_executable(pod1_app pod1-responder/main.c)
set_target_properties(pod1_app PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/pod1-responder")
target_link_libraries(pod1_app PRIVATE common ${DPDK_LIBRARIES} ${DRIVER_LDFLAGS} ${EXTRA_LIBS})

# 7. Chương trình Unit Test offline
add_executable(test_l3_table tests/test_l3_table.c)
target_link_libraries(test_l3_table PRIVATE common ${DPDK_LIBRARIES} ${DRIVER_LDFLAGS} ${EXTRA_LIBS})

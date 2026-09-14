FROM ubuntu:24.04

# Cài đặt các thư viện phụ thuộc runtime tối thiểu
RUN apt-get update && apt-get install -y --no-install-recommends \
    libnuma1 \
    libpcap0.8 \
    iproute2 \
    net-tools \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy thư viện DPDK và binary ứng dụng Pod 0
COPY build/dpdk-libs/ /app/dpdk-libs/
COPY pod0-forwarder/pod0_app /app/pod0_app
COPY pod0-forwarder/routes.conf /app/routes.conf

RUN chmod -R 755 /app

ENV LD_LIBRARY_PATH=/app/dpdk-libs

ENTRYPOINT ["/app/pod0_app"]

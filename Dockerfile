FROM ubuntu:22.04

# Türkiye mirror'ı kullan + universe/backports kaldır (17.5 MB gereksiz indirme engellenir)
RUN sed -i 's|http://archive.ubuntu.com|http://tr.archive.ubuntu.com|g' /etc/apt/sources.list && \
    sed -i 's|http://security.ubuntu.com|http://tr.archive.ubuntu.com|g' /etc/apt/sources.list && \
    sed -i '/universe/d' /etc/apt/sources.list && \
    sed -i '/backports/d' /etc/apt/sources.list

# Install dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    make \
    iproute2 \
    iputils-ping \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Set up libiec61850 (copy from local build context - no internet needed)
# Linux VM'de: cp -r /path/to/libiec61850 /media/sf_test1/P1_docker/windows/linux_files/
COPY libiec61850/ /root/libiec61850/
WORKDIR /root/libiec61850
RUN make

# Copy the project files
WORKDIR /app
COPY . /app

# Build the project (all: isletim bilesenleri, attacker: saldiri araclari)
RUN make clean && make all && make attacker

# Default command (can be overridden by docker-compose)
CMD ["tail", "-f", "/dev/null"]

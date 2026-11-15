#!/bin/bash
cd ../..
CONFIG_FILE="../network_config.json"
ENV_FILE=".env"
if [ ! -f "$CONFIG_FILE" ]; then exit 1; fi

# Detect default parent network interface
NIC=$(ip route | grep default | awk '{print $5}' | head -1)
if [ -z "$NIC" ]; then
    NIC="enp0s3"
fi

# Detect VM's IP address dynamically
VM_IP=$(ip -o -4 addr show dev "$NIC" | awk '{print $4}' | cut -d/ -f1 | head -1)
if [ -z "$VM_IP" ]; then
    VM_IP=$(jq -r '.WINDOWS_IP' "$CONFIG_FILE")
fi
echo "Detected VM IP address: $VM_IP"

# Update WINDOWS_IP in network_config.json dynamically if writable
if jq --arg ip "$VM_IP" '.WINDOWS_IP = $ip' "$CONFIG_FILE" > temp_config.json 2>/dev/null; then
    mv temp_config.json "$CONFIG_FILE" 2>/dev/null || rm temp_config.json
fi

# Copy network_config.json and simulation files to build context
cp "$CONFIG_FILE" .
cp ../streamer.py . 2>/dev/null || true
cp ../digital_substation.py . 2>/dev/null || true
cp ../web_app.py . 2>/dev/null || true
cp -r ../templates . 2>/dev/null || true


# Ensure host VLAN 10, VLAN 20 and VLAN 30 interfaces exist for Docker macvlan
if ! ip link show "$NIC.10" &>/dev/null; then
    echo "Creating host VLAN 10 interface: $NIC.10"
    sudo ip link add link "$NIC" name "$NIC.10" type vlan id 10
    sudo ip link set dev "$NIC.10" up
    sudo ip addr add 10.10.10.1/24 dev "$NIC.10" 2>/dev/null || true
    sudo ip link set "$NIC.10" promisc on
fi
if ! ip link show "$NIC.20" &>/dev/null; then
    echo "Creating host VLAN 20 interface: $NIC.20"
    sudo ip link add link "$NIC" name "$NIC.20" type vlan id 20
    sudo ip link set dev "$NIC.20" up
    sudo ip addr add 10.10.20.1/24 dev "$NIC.20" 2>/dev/null || true
    sudo ip link set "$NIC.20" promisc on
fi
if ! ip link show "$NIC.30" &>/dev/null; then
    echo "Creating host VLAN 30 interface: $NIC.30"
    sudo ip link add link "$NIC" name "$NIC.30" type vlan id 30
    sudo ip link set dev "$NIC.30" up
    sudo ip addr add 10.10.30.1/24 dev "$NIC.30" 2>/dev/null || true
    sudo ip link set "$NIC.30" promisc on
fi

# Write environment variables to .env
SIM_IP=$(jq -r '.LINUX_SIM_IP_VLAN30' "$CONFIG_FILE")
if [ -z "$SIM_IP" ] || [ "$SIM_IP" = "null" ]; then
    SIM_IP="10.10.30.216"
fi
echo "PARENT_INTERFACE=$NIC" > "$ENV_FILE"
echo "WINDOWS_IP=$SIM_IP" >> "$ENV_FILE"

echo "LINUX_1_IP_VLAN10=$(jq -r '.LINUX_1_IP_VLAN10' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_1_IP_VLAN20=$(jq -r '.LINUX_1_IP_VLAN20' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_1_IP_VLAN30=$(jq -r '.LINUX_1_IP_VLAN30' "$CONFIG_FILE")" >> "$ENV_FILE"

echo "LINUX_2_IP_VLAN10=$(jq -r '.LINUX_2_IP_VLAN10' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_2_IP_VLAN20=$(jq -r '.LINUX_2_IP_VLAN20' "$CONFIG_FILE")" >> "$ENV_FILE"

echo "LINUX_3_IP_VLAN10=$(jq -r '.LINUX_3_IP_VLAN10' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_3_IP_VLAN20=$(jq -r '.LINUX_3_IP_VLAN20' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_3_IP_VLAN30=$(jq -r '.LINUX_3_IP_VLAN30' "$CONFIG_FILE")" >> "$ENV_FILE"

echo "LINUX_SCADA_IP_VLAN10=$(jq -r '.LINUX_SCADA_IP_VLAN10' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_SCADA_IP_VLAN20=$(jq -r '.LINUX_SCADA_IP_VLAN20' "$CONFIG_FILE")" >> "$ENV_FILE"

echo "LINUX_CTRL_IP_VLAN10=$(jq -r '.LINUX_CTRL_IP_VLAN10' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_CTRL_IP_VLAN20=$(jq -r '.LINUX_CTRL_IP_VLAN20' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_CTRL_IP_VLAN30=$(jq -r '.LINUX_CTRL_IP_VLAN30' "$CONFIG_FILE")" >> "$ENV_FILE"

echo "LINUX_SIM_IP_VLAN10=$(jq -r '.LINUX_SIM_IP_VLAN10' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_SIM_IP_VLAN20=$(jq -r '.LINUX_SIM_IP_VLAN20' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "LINUX_SIM_IP_VLAN30=$(jq -r '.LINUX_SIM_IP_VLAN30' "$CONFIG_FILE")" >> "$ENV_FILE"

echo "UDP_PORT=$(jq -r '.UDP_PORT' "$CONFIG_FILE")" >> "$ENV_FILE"
echo "CONTROL_PORT=$(jq -r '.CONTROL_PORT' "$CONFIG_FILE")" >> "$ENV_FILE"

if docker compose version &> /dev/null; then
    docker compose up -d --build control_ied
else
    docker-compose up -d --build control_ied
fi

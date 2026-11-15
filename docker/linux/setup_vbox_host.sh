#!/bin/bash

echo -e "\e[36m================================================\e[0m"
echo -e "\e[36m=== SETUP MACVLAN SIBLING INTERFACES FOR VM ===\e[0m"
echo -e "\e[36m================================================\e[0m"

# 1. Detect active physical network interface
NIC=$(ip route | grep default | awk '{print $5}' | head -1)
if [ -z "$NIC" ]; then
    NIC="enp0s3"
fi

echo "Active interface: $NIC"

# 2. Ensure parent VLAN interfaces exist
if ! ip link show "$NIC.10" &>/dev/null; then
    echo "VLAN 10 interface does not exist. Creating it..."
    sudo ip link add link "$NIC" name "$NIC.10" type vlan id 10
    sudo ip link set dev "$NIC.10" up
fi

if ! ip link show "$NIC.20" &>/dev/null; then
    echo "VLAN 20 interface does not exist. Creating it..."
    sudo ip link add link "$NIC" name "$NIC.20" type vlan id 20
    sudo ip link set dev "$NIC.20" up
fi

# 3. Create macvlan sibling interface for Process Bus (VLAN 10)
if ! ip link show "mac-proc" &>/dev/null; then
    echo "Creating macvlan sibling 'mac-proc' on $NIC.10..."
    sudo ip link add link "$NIC.10" name "mac-proc" type macvlan mode bridge
    sudo ip addr add 10.10.10.99/24 dev "mac-proc"
    sudo ip link set dev "mac-proc" up
    sudo ip link set "mac-proc" promisc on
else
    echo "mac-proc already exists."
    # Ensure IP is set correctly
    sudo ip addr flush dev "mac-proc"
    sudo ip addr add 10.10.10.99/24 dev "mac-proc"
fi

# 4. Create macvlan sibling interface for Station Bus (VLAN 20)
if ! ip link show "mac-stat" &>/dev/null; then
    echo "Creating macvlan sibling 'mac-stat' on $NIC.20..."
    sudo ip link add link "$NIC.20" name "mac-stat" type macvlan mode bridge
    sudo ip addr add 10.10.20.99/24 dev "mac-stat"
    sudo ip link set dev "mac-stat" up
    sudo ip link set "mac-stat" promisc on
else
    echo "mac-stat already exists."
    # Ensure IP is set correctly
    sudo ip addr flush dev "mac-stat"
    sudo ip addr add 10.10.20.99/24 dev "mac-stat"
fi

# 5. Enable IP forwarding and routing rules
echo "Enabling IP forwarding and iptables rules..."
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -I FORWARD 1 -j ACCEPT

echo -e "\e[32m================================================\e[0m"
echo -e "\e[32m=== SETUP COMPLETED SUCCESSFULLY! ===\e[0m"
echo -e "\e[32m  Process Bus VM IP  : 10.10.10.99 (mac-proc)\e[0m"
echo -e "\e[32m  Station Bus VM IP  : 10.10.20.99 (mac-stat)\e[0m"
echo -e "\e[32m================================================\e[0m"

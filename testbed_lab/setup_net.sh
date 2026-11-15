#!/bin/bash
# Sanal testbed agi — WSL/Linux uzerinde 7 ayri "cihaz" (network namespace)
# Kullanim: sudo bash setup_net.sh
# Not: docker-compose.yml'deki macvlan/VLAN yapisinin namespace karsiligidir.
set -e
BR=brt0

for node in mu prot bcu scada ctrl atk sim; do
  ip netns del ns_$node 2>/dev/null || true
done
ip link del $BR 2>/dev/null || true

ip link add $BR type bridge
ip link set $BR up
echo 0 > /sys/class/net/$BR/bridge/multicast_snooping 2>/dev/null || true
echo 0 > /sys/class/net/$BR/bridge/multicast_querier 2>/dev/null || true

setup_node() {
  local node=$1 v10=$2 v20=$3
  local ns=ns_$node
  ip netns add $ns
  ip link add veth_$node type veth peer name vethp_$node
  ip link set vethp_$node netns $ns
  ip link set veth_$node master $BR
  ip link set veth_$node up
  ip netns exec $ns ip link set lo up
  ip netns exec $ns ip link set vethp_$node name eth0
  ip netns exec $ns ip link set eth0 up
  ip netns exec $ns ip addr add 10.10.10.$v10/24 dev eth0
  if [ -n "$v20" ]; then
    ip netns exec $ns ip addr add 10.10.20.$v20/24 dev eth0
  fi
}

setup_node mu    211 211   # Merging Unit
setup_node prot  212 212   # Protection IED
setup_node bcu   213 213   # Breaker IED
setup_node scada 214 214   # SCADA
setup_node ctrl  215 215   # Control IED
setup_node sim   216 216   # Güç sistemi simülatörü (streamer.py yerine)
setup_node atk   217 ""    # Saldırgan

echo "=== ag hazir ==="
for node in mu prot bcu scada ctrl sim atk; do
  printf '%-6s ' "$node"
  ip netns exec ns_$node ip -4 -br addr show eth0 | awk '{print $3}'
done

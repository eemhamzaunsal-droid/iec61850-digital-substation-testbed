#!/bin/bash
# TAM SENARYO: normal isletim -> gercek ariza -> sahte trip (saldiri)
# Kullanim:  sudo bash run_scenario.sh
# Onkosul :  ../Makefile ile derlenmis binary'ler ve setup_net.sh
# Cikti   :  /tmp/testbed_cap/{normal,fault,attack}.pcap + logs/
set -u
LAB="$(cd "$(dirname "$0")" && pwd)"
W="$(cd "$LAB/.." && pwd)"          # linux_files dizini
OUT=/tmp/testbed_cap
mkdir -p "$OUT" "$W/logs"
rm -f "$OUT"/*.pcap "$W"/logs/*.log

bash "$LAB/setup_net.sh" > /dev/null 2>&1 || { echo "Ag kurulamadi (root gerekli)"; exit 1; }

run_node() {
  local node=$1; shift
  ip netns exec ns_$node bash -c "cd $W && exec $* >> $W/logs/$node.log 2>&1" &
  echo $! > "$W/logs/$node.pid"
}
stop_all() {
  for p in "$W"/logs/*.pid; do kill "$(cat "$p")" 2>/dev/null; done
  sleep 0.3
  pkill -f protection_ied >/dev/null 2>&1
  pkill -f breaker_ied    >/dev/null 2>&1
  pkill -f control_ied    >/dev/null 2>&1
  pkill -f mu_publisher   >/dev/null 2>&1
  pkill -f scada_monitor  >/dev/null 2>&1
  sleep 1
}
start_core() {
  run_node prot  ./protection_ied eth0 eth0
  run_node bcu   env WINDOWS_IP=10.10.30.216 ./breaker_ied eth0 eth0 eth0
  run_node mu    ./mu_publisher eth0 eth0 5005
  run_node ctrl  env PROT_IP=10.10.10.212 ./control_ied eth0 eth0
  sleep 3
}
cap_start() { tcpdump -i brt0 -s 0 -w "$1" -q 2>/dev/null & echo $! > /tmp/cap.pid; }
cap_stop()  { kill -INT "$(cat /tmp/cap.pid)" 2>/dev/null; sleep 0.7; }
meas()      { ip netns exec ns_sim python3 "$LAB/send_meas.py" 10.10.10.211 "$@"; }

echo "################ FAZ 1: NORMAL ISLETIM ################"
start_core
cap_start "$OUT/normal.pcap"
meas 120 8
cap_stop
stop_all

echo "################ FAZ 2: GERCEK ARIZA (i_f1 = 420 A) ################"
start_core
cap_start "$OUT/fault.pcap"
meas 120 2
meas 420 4
cap_stop
echo "--- Protection ---"; grep -E "ALARM|ARIZA" "$W/logs/prot.log" | tail -2
echo "--- BCU ---";        grep -E "ACILDI" "$W/logs/bcu.log" | tail -1
stop_all

echo "################ FAZ 3: SALDIRI (sahte trip, ariza YOK) ################"
start_core
cap_start "$OUT/attack.pcap"
( sleep 1.5; meas 120 4 ) &
run_node atk ./goose_attacker -i eth0 -m trip -c 3 -p 300
sleep 5
cap_stop
echo "--- Saldirgan ---"; tail -3 "$W/logs/atk.log"
echo "--- BCU ---";       grep -E "ACILDI" "$W/logs/bcu.log" | tail -1
echo "--- Protection (ariza yok) ---"; grep -E "NORMAL" "$W/logs/prot.log" | tail -1
stop_all

echo
echo "################ OZET ################"
for f in normal fault attack; do
  printf '%-8s  SV: %-4s  GOOSE: %-3s  MMS: %s\n' "$f" \
    "$(tshark -r $OUT/$f.pcap -Y 'sv' 2>/dev/null | wc -l)" \
    "$(tshark -r $OUT/$f.pcap -Y 'goose' 2>/dev/null | wc -l)" \
    "$(tshark -r $OUT/$f.pcap -Y 'mms' 2>/dev/null | wc -l)"
done
echo "pcap'ler: $OUT"

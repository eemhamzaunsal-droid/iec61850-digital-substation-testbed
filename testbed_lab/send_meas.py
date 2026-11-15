#!/usr/bin/env python3
"""streamer.py yerine gecen basit olcum gonderici (UDP JSON, MU formati).

Kullanim: python3 send_meas.py <MU_IP> <i_f1_amper> <sure_sn>
Ornek   : python3 send_meas.py 10.10.10.211 120 8     # normal yuk
          python3 send_meas.py 10.10.10.211 420 4     # asiri akim (ariza)
"""
import socket
import sys
import time

ip = sys.argv[1]
i_f1 = float(sys.argv[2])
dur = float(sys.argv[3])
port = 5005

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
t0 = time.time()
n = 0
while time.time() - t0 < dur:
    msg = ('{"timestamp": %.3f, "measurements": {"v_hv": 11000.0, "i_hv": 50.0, '
           '"i_lv": 45.0, "v_mv": 400.0, "i_f1": %.2f, "i_f2": 100.0, '
           '"p_f1": 1.0, "q_f1": 0.1, "p_f2": 1.0, "q_f2": 0.1}}') % (time.time(), i_f1)
    s.sendto(msg.encode(), (ip, port))
    n += 1
    time.sleep(0.02)  # ~50 Hz
print("gonderilen paket: %d (i_f1=%.1f A)" % (n, i_f1))

from flask import Flask, render_template
from flask_socketio import SocketIO
import threading
import os
import platform
import subprocess
import time
import json
import socket

app = Flask(__name__)
app.config['SECRET_KEY'] = 'scada-secret'
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='threading')

# Konfigurasyon
CONFIG_FILE = "network_config.json"
TELEMETRY_PORT = 5007

def get_config():
    try:
        with open(CONFIG_FILE, "r") as f:
            return json.load(f)
    except:
        return {"LINUX_1_IP": "192.168.1.15", "LINUX_2_IP": "192.168.1.16", "LINUX_3_IP": "192.168.1.17"}

config = get_config()

# Global Sistem Durumu
system_state = {
    "v_hv": 0, "i_hv": 0, "v_mv": 0, "i_f1": 0, "i_f2": 0,
    "v_b3": 0.0, "v_b4": 0.0, "i_3_4": 0.0,
    "p_f1": 0.0, "q_f1": 0.0, "p_f2": 0.0, "q_f2": 0.0,
    "loss_mw": 0.0, "efficiency": 100.0,
    "cos_phi_f1": 1.0, "cos_phi_f2": 1.0,
    "rtt": 0.0, "bfp_cd": 0.15,
    "sv_pps": 0, "goose_pps": 0, "mms_pps": 0,
    "breakers": {
        "F1": "CLOSED"
    },
    "machines": {
        "MU": {"status": "OFFLINE", "last_seen": 0, "details": {}},
        "PROT": {"status": "OFFLINE", "last_seen": 0, "details": {}},
        "BCU": {"status": "OFFLINE", "last_seen": 0, "details": {}}
    }
}

def telemetry_listener():
    """Linux makinelerinden gelen detayli durum verilerini dinler"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", TELEMETRY_PORT))
    print(f"📡 [TELEMETRY] Makinelerden veri bekleniyor (Port: {TELEMETRY_PORT})...")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            payload = json.loads(data.decode('utf-8'))
            m_id = payload.get("id")
            print(f"📡 [TELEMETRY] Received data from {m_id}") # Debug logu
            if m_id in system_state["machines"]:
                system_state["machines"][m_id]["status"] = "ONLINE"
                system_state["machines"][m_id]["last_seen"] = time.time()
                system_state["machines"][m_id]["details"] = payload.get("data", {})
                
                # Telemetri detaylarini ana sisteme düzleştirerek (flat) aktaralım
                tel_data = payload.get("data", {})
                for k, v in tel_data.items():
                    system_state[k] = v
                
                socketio.emit('telemetry_update', system_state["machines"])
                socketio.emit('update', system_state)
        except Exception as e:
            print(f"Telemetry error: {e}")

def offline_checker():
    """10 saniye veri gelmeyen makineyi OFFLINE yapar"""
    while True:
        now = time.time()
        changed = False
        for m_id in system_state["machines"]:
            if system_state["machines"][m_id]["last_seen"] > 0 and (now - system_state["machines"][m_id]["last_seen"] > 5):
                if system_state["machines"][m_id]["status"] != "OFFLINE":
                    system_state["machines"][m_id]["status"] = "OFFLINE"
                    changed = True
        if changed:
            socketio.emit('telemetry_update', system_state["machines"])
        time.sleep(2)

@app.route('/')
def index():
    return render_template('index.html')

# Thread'leri baslat (Import edildiginde de calismasi icin)
def start_background_tasks():
    if not hasattr(app, "_threads_started"):
        threading.Thread(target=telemetry_listener, daemon=True).start()
        threading.Thread(target=offline_checker, daemon=True).start()
        app._threads_started = True

@socketio.on('connect')
def handle_connect():
    start_background_tasks() # Ilk baglantida tasklari tetikle
    socketio.emit('telemetry_update', system_state["machines"])
    socketio.emit('update', system_state)

def update_state(data):
    """streamer.py'den gelen verileri alir"""
    global system_state
    system_state.update(data)
    socketio.emit('update', system_state)

if __name__ == '__main__':
    start_background_tasks()
    socketio.run(app, host='0.0.0.0', port=5000, debug=False, use_reloader=False)

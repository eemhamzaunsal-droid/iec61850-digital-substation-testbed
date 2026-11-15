import pandapower as pp
import socket
import time
import json
import random
import threading
import sys
import numpy as np
import datetime
from digital_substation import create_digital_substation_net
from web_app import app, update_state, socketio

# --- YAPILANDIRMA ---
CONFIG_FILE = "network_config.json"
import os

def load_config():
    try:
        with open(CONFIG_FILE, "r") as f:
            return json.load(f)
    except:
        return {"LINUX_1_IP_VLAN10": "10.10.10.211", "UDP_PORT": 5005, "CONTROL_PORT": 5006}

config = load_config()

def ts():
    """Mikrosaniye hassasiyetli zaman damgasi (IED loglariyla ayni bicim)."""
    return datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')

LINUX_1_IP = os.getenv("LINUX_1_IP_VLAN10") or config.get("LINUX_1_IP_VLAN10", config.get("LINUX_1_IP", "10.10.10.211"))
UDP_PORT = int(os.getenv("UDP_PORT")) if os.getenv("UDP_PORT") else config["UDP_PORT"]
CONTROL_PORT = int(os.getenv("CONTROL_PORT")) if os.getenv("CONTROL_PORT") else config["CONTROL_PORT"]

def breaker_control_listener(net):
    control_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control_sock.bind(("0.0.0.0", CONTROL_PORT))
    while True:
        data, addr = control_sock.recvfrom(1024)
        t_rx = ts()
        command = data.decode('utf-8').strip().upper()
        
        try:
            f1_idx = net.switch[net.switch.name == "CB1"].index[0]
            if "TRIP" in command and "F1" in command: 
                net.switch.at[f1_idx, "closed"] = False
                print(f"[{t_rx}] >> [KOMUT ALINDI] {command} (BCU, UDP {CONTROL_PORT}) -> CB1 ACIK")
            elif "CLOSE" in command and "F1" in command: 
                net.switch.at[f1_idx, "closed"] = True
                print(f"[{t_rx}] >> [KOMUT ALINDI] {command} (BCU, UDP {CONTROL_PORT}) -> CB1 KAPALI")
        except IndexError:
            pass

sim_mode = "NORMAL"

def keyboard_listener(net):
    global sim_mode
    print("\n" + "="*50)
    print("  KLAVYE KONTROLLERİ AKTİF (IEEE 14 BARA MODU):")
    print("  [1] - Aşırı Akım Tetikle (Bara 4 Yükü Artırılır)")
    print("  [2] - Kısa Devre Arızası Tetikle (Bara 4 Yükü Maksimum)")
    print("  [3] - Normal Çalışma / Sıfırla")
    print("="*50 + "\n")
    
    while True:
        try:
            line = sys.stdin.readline().strip()
            if not line:
                continue
            cmd = line[0]
            
            # CB1 index
            f1_idx = net.switch[net.switch.name == "CB1"].index[0]
            # Yuk 1 (Bara 4'teki yük)
            load1_idx = net.load[net.load.name == "Yuk 1"].index[0]
            
            if cmd == '1':
                sim_mode = "OVERCURRENT"
                # Akımı artırmak için yükü çok yükseltiyoruz
                net.load.at[load1_idx, "p_mw"] = 150.0
                net.load.at[load1_idx, "q_mvar"] = 50.0
                print("\n==================================================")
                print(f"[{ts()}] [KOMUT UYGULANDI] -> 1: AŞIRI AKIM aktif edildi.")
                print("==================================================\n")
            elif cmd == '2':
                sim_mode = "SHORT_CIRCUIT"
                net.load.at[load1_idx, "p_mw"] = 350.0
                net.load.at[load1_idx, "q_mvar"] = 150.0
                print("\n==================================================")
                print(f"[{ts()}] [KOMUT UYGULANDI] -> 2: KISA DEVRE aktif edildi.")
                print("==================================================\n")
            elif cmd == '3':
                sim_mode = "NORMAL"
                # Yükleri normale çek (IEEE 14 Bara 4'ün standart yükü ~ 47.8 MW, 3.9 MVAR'dır)
                net.load.at[load1_idx, "p_mw"] = 47.8
                net.load.at[load1_idx, "q_mvar"] = -3.9
                # Kesiciyi kapat
                net.switch.at[f1_idx, "closed"] = True
                print("\n==================================================")
                print(f"[{ts()}] [KOMUT UYGULANDI] -> 3: SİSTEM SIFIRLANDI.")
                print("==================================================\n")
        except Exception as e:
            print(f"Klavye komut hatası: {e}")

def main_simulation_loop():
    net = create_digital_substation_net()
    
    # Standart başlangıç yük değerini ayarlayalım
    load1_idx = net.load[net.load.name == "Yuk 1"].index[0]
    net.load.at[load1_idx, "p_mw"] = 47.8
    net.load.at[load1_idx, "q_mvar"] = -3.9

    threading.Thread(target=breaker_control_listener, args=(net,), daemon=True).start()
    threading.Thread(target=keyboard_listener, args=(net,), daemon=True).start()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    print("\n" + "="*50)
    print("      IEEE 14-BUS GÜÇ AKIŞI BAŞLADI")
    print("="*50)
    
    try:
        while True:
            # Mevcut yüklere ufak bir gürültü ekle (%0.5 random sapma)
            temp_p_mw = net.load.p_mw.copy()
            for i in range(len(net.load)):
                net.load.at[i, "p_mw"] = temp_p_mw.at[i] * (1 + random.uniform(-0.005, 0.005))
            
            cb1_closed = bool(net.switch.at[net.switch[net.switch.name == "CB1"].index[0], "closed"])
            f1_line_idx = net.line[net.line.name == "Fider 1 Hatti"].index[0]
            target_bus = net.line.at[f1_line_idx, "to_bus"]
            
            # Güç akışı
            solver_failed = False
            try:
                pp.runpp(net, consider_isolated_bus=True, suppress_errors=False, numba=False, max_iteration=100, init="flat")
            except Exception as e:
                print(f"[SOLVER ERROR] {e}")
                solver_failed = True

            if not solver_failed:
                # --- DOSYAYA OTOMATIK YAZDIRMA ---
                try:
                    import datetime
                    import os
                    log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "guc_akisi_raporu.txt")
                    with open(log_path, "w") as f:
                        f.write("=== IEEE 14-BUS ANLIK GUC AKISI RAPORU ===\n")
                        f.write(f"Son Guncelleme: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
                        f.write(f"Simulasyon Modu: {sim_mode}\n\n")
                        f.write("1. BARA SONUCLARI [vm_pu: Voltaj, va_degree: Aci, p_mw: Guc, q_mvar: Reaktif]\n")
                        f.write("-" * 75 + "\n")
                        f.write(net.res_bus[['vm_pu', 'va_degree', 'p_mw', 'q_mvar']].round(3).to_string())
                        f.write("\n\n2. HAT SONUCLARI [i_ka: Akim(kA), loading_percent: Yuklenme(%), pl_mw: Kayip]\n")
                        f.write("-" * 75 + "\n")
                        f.write(net.res_line[['i_ka', 'loading_percent', 'pl_mw', 'ql_mvar']].round(3).to_string())
                except Exception as log_e:
                    print(f"[RAPOR YAZMA HATASI] Dosya olusturulamadi: {log_e}")
                # --------------------------------------------------
            
            # Eski yük değerini gürültüsüz olarak geri yükleyelim
            net.load.p_mw = temp_p_mw
            
            # Değerleri oku ve NaN kontrolü yap (NaN ise 0 yap)
            def safe_val(val, factor=1.0):
                if np.isnan(val) or np.isinf(val): return 0.0
                return float(val * factor)

            if solver_failed:
                v_hv, i_hv, i_lv, v_mv, i_f1, i_f2 = 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
                v_b3, v_b4, i_3_4 = 0.0, 0.0, 0.0
                p_f1, q_f1, p_f2, q_f2 = 0.0, 0.0, 0.0, 0.0
            else:
                # v_hv'yi Bus 0 (Slack Bus) geriliminden al
                v_hv = safe_val(net.res_bus.vm_pu.at[0], 138000.0) 
                
                # Fider 1 (CT1) üzerinden geçen akım
                if cb1_closed:
                    # IEEE14'te akım değerleri kA cinsindedir. 
                    # 1000 ile çarparak Amper'e çeviriyoruz.
                    i_f1 = safe_val(net.res_line.i_ka.at[f1_line_idx], 1000.0)
                else:
                    i_f1 = 0.0

                # IEEE 14 Fider 1 üzerinden akan güç
                p_f1 = safe_val(net.res_line.p_from_mw.at[f1_line_idx])
                q_f1 = safe_val(net.res_line.q_from_mvar.at[f1_line_idx])
                
                # Web arayüzünü bozmamak için Fider 2 vb değişkenleri sabit 0.0 veriyoruz
                i_f2 = 0.0
                p_f2 = 0.0
                q_f2 = 0.0
                i_hv = 0.0
                i_lv = 0.0
                v_mv = safe_val(net.res_bus.vm_pu.at[target_bus], 18000.0) # Hedef baranın gerilimi
                v_b3 = safe_val(net.res_bus.vm_pu.at[2], 135000.0)
                v_b4 = safe_val(net.res_bus.vm_pu.at[3], 135000.0)
                i_3_4 = safe_val(net.res_line.i_ka.at[5], 1000.0)

            # Kesici durumlarını oku
            brk_f1 = "KAPALI" if cb1_closed else "ACIK"
            
            loss_mw = 0.0
            efficiency = 100.0
            
            s_f1 = np.sqrt(p_f1**2 + q_f1**2)
            cos_phi_f1 = p_f1 / s_f1 if s_f1 > 0 else 1.0
            cos_phi_f2 = 1.0

            # 1. Linux MU birimine SV gönder
            data_sv = {
                "timestamp": time.time(),
                "measurements": {
                    "v_hv": v_hv, "i_hv": i_hv, "i_lv": i_lv, "v_mv": v_mv, "i_f1": i_f1, "i_f2": i_f2,
                    "p_f1": p_f1, "q_f1": q_f1, "p_f2": p_f2, "q_f2": q_f2
                }
            }
            t_sv = ts()
            sock.sendto(json.dumps(data_sv).encode('utf-8'), (LINUX_1_IP, UDP_PORT))
            
            # 2. Web Dashbord Güncelle
            update_state({
                "v_hv": v_hv, "i_hv": i_hv, "i_lv": i_lv, "v_mv": v_mv, "i_f1": i_f1, "i_f2": i_f2,
                "v_b3": v_b3, "v_b4": v_b4, "i_3_4": i_3_4,
                "p_f1": p_f1, "q_f1": q_f1, "p_f2": p_f2, "q_f2": q_f2,
                "loss_mw": loss_mw, "efficiency": efficiency,
                "cos_phi_f1": cos_phi_f1, "cos_phi_f2": cos_phi_f2,
                "breakers": {"F1": brk_f1}
            })
            
            # 3. Temiz Terminal Çıktısı
            print(f"[{t_sv}] >> [{sim_mode}] F1_I: {i_f1:.1f}A | F1_P: {p_f1:.1f} MW | F1_CB: {brk_f1}")
            
            time.sleep(0.1)
            
    except KeyboardInterrupt:
        print("\n\n[INFO] Simülasyon durduruldu.")
    finally:
        sock.close()

if __name__ == "__main__":
    web_thread = threading.Thread(target=lambda: socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True))
    web_thread.daemon = True
    web_thread.start()
    main_simulation_loop()

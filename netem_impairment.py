#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""netem_impairment.py - Section 3.1.3 / Table 6 deney kosucusu.

Ne yapar
--------
Docker ile calisan testbed uzerinde, Tablo 6'daki bozulma matrisini sirayla uygular
(IMPARIMENT CONDITIONS), her kosuda Section 3.2'deki ariza senaryosunu tetikler ve
kaydedilen loglardan su dort buyuklugu cikarir:

  1) kaybolan SV cercevesi  (IED2_PROT'un gordugu SmpCnt dizisindeki bosluklar)
  2) GOOSE trip gecikmesi   (IED2_PROT 'TRIP SENT' -> IED3_BCU 'TRIP RECEIVED')
  3) trip mesaji ulasti mi  (evet/hayir)
  4) CB1 acildi mi          (IED3_BCU logu)

Cikti: results/impairment_matrix.csv  (Tablo 6'nin satirlari) ve
       results/summary.md            (kosu basina ortalama +- standart sapma)

On kosullar
-----------
  * Testbed docker compose ile ayakta (mu_publisher, protection_ied, breaker_ied,
    power_simulator konteynerleri "running").
  * Her IED konteynerinde NET_ADMIN var (docker-compose.yml'de mevcut) ve host
    cekirdeginde sch_netem modulu yuklu:  sudo modprobe sch_netem
  * Konteynerlerin VLAN10 arayuzu eth0 (macvlan).

Kullanim
--------
  python3 netem_impairment.py                 # tam matris, 10 tekrar
  python3 netem_impairment.py --trials 3 --only loss5,delay50
  python3 netem_impairment.py --manual        # '1' komutunu siz girin
  python3 netem_impairment.py --dry-run       # sadece komutlari yazdir

Not: '1' komutu streamer.py'de yuku 150 MW / 50 Mvar'a cikarir (OVERCURRENT);
'3' komutu sistemi 47.8 MW / -3.9 Mvar'a dondurur ve CB1'i kapatir.
"""
import argparse
import csv
import datetime as dt
import os
import re
import statistics
import subprocess
import sys
import time

IFACE = "eth0"          # VLAN10 process bus arayuzu
CTR = {
    "mu": "mu_publisher",
    "prot": "protection_ied",
    "bcu": "breaker_ied",
    "sim": "power_simulator",
}
SETTLE_S = 2.0          # reset sonrasi oturma
FAULT_S = 3.0           # ariza sonrasi bekleme

# (ad, [(konteyner, netem ifadesi), ...])
CONDITIONS = [
    ("baseline", []),
    ("loss1", [("mu", "loss 1%"), ("prot", "loss 1%")]),
    ("loss5", [("mu", "loss 5%"), ("prot", "loss 5%")]),
    ("loss10", [("mu", "loss 10%"), ("prot", "loss 10%")]),
    ("delay20", [("mu", "delay 20ms"), ("prot", "delay 20ms")]),
    ("delay50", [("mu", "delay 50ms"), ("prot", "delay 50ms")]),
    ("delay100", [("mu", "delay 100ms"), ("prot", "delay 100ms")]),
    ("reorder5", [("mu", "reorder 5% 50%")]),
    ("congestion", []),     # arka plan flood ayri baslatilir
]

TS_RE = re.compile(r"\[?(\d{4}-\d\d-\d\d[T ]\d\d:\d\d:\d\d\.\d+)\]?")
SMPCNT_RE = re.compile(r"SmpCnt[:=\s]+(\d+)", re.I)


def sh(cmd, check=False):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"komut basarisiz: {cmd}\n{r.stderr}")
    return r


def ensure_up():
    r = sh(f"docker inspect -f '{{{{.State.Running}}}}' {CTR['sim']}")
    if "true" not in r.stdout.lower():
        sys.exit("power_simulator calismiyor; once docker compose up yapin.")
    sh("docker exec mu_publisher tc qdisc show dev eth0", check=True)


def clear_netem():
    for c in (CTR["mu"], CTR["prot"], CTR["bcu"]):
        sh(f"docker exec {c} tc qdisc del dev {IFACE} root 2>/dev/null")


def apply_netem(spec_list):
    for key, expr in spec_list:
        cmd = f"docker exec {CTR[key]} tc qdisc replace dev {IFACE} root netem {expr}"
        r = sh(cmd)
        if r.returncode != 0:
            print(f"  !! netem uygulanamadi ({key}): {r.stderr.strip()}")


def send_cmd(key, manual=False):
    """power_simulator'in stdin'ine '1' / '3' gonderir."""
    if manual:
        input(f"  >> power_simulator konsolunda '{key}' tusuna basin, sonra Enter...")
        return
    p = subprocess.Popen(["docker", "attach", CTR["sim"]],
                         stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.4)
        p.stdin.write(f"{key}\n".encode())
        p.stdin.flush()
        time.sleep(0.3)
    finally:
        try:
            p.stdin.close()
        except Exception:
            pass
        p.wait(timeout=5)


def dump_logs(since_iso, outdir):
    os.makedirs(outdir, exist_ok=True)
    paths = {}
    for k in ("mu", "prot", "bcu", "sim"):
        r = sh(f"docker logs --timestamps --since {since_iso} {CTR[k]}")
        p = os.path.join(outdir, f"{k}.log")
        with open(p, "w", encoding="utf-8") as f:
            f.write(r.stdout + r.stderr)
        paths[k] = p
    return paths


def parse_ts(line):
    m = TS_RE.search(line)
    if not m:
        return None
    s = m.group(1).replace("T", " ")
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try:
            return dt.datetime.strptime(s, fmt)
        except ValueError:
            continue
    return None


def load(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def first_ts(lines, pred):
    for ln in lines:
        if pred(ln):
            t = parse_ts(ln)
            if t:
                return t
    return None


def sv_gaps(lines):
    seq = []
    for ln in lines:
        m = SMPCNT_RE.search(ln)
        if m:
            seq.append(int(m.group(1)))
    missed = 0
    for a, b in zip(seq, seq[1:]):
        if b > a + 1 and b > a:          # yeniden baslama (reset) sayilmaz
            missed += (b - a - 1)
    return missed, len(seq)


def metrics(paths):
    mu, prot, bcu = (load(paths["mu"]), load(paths["prot"]), load(paths["bcu"]))
    trip_sent = first_ts(prot, lambda l: ("TRIP" in l.upper() or "TRIP" in l)
                         and ("SENT" in l.upper() or "GONDER" in l.upper()
                              or "YAYIN" in l.upper()))
    trip_recv = first_ts(bcu, lambda l: "TRIP" in l.upper()
                         and ("RECEIVED" in l.upper() or "ALINDI" in l.upper()))
    cb_open = first_ts(bcu, lambda l: ("OPENED" in l.upper() or "ACILDI" in l.upper())
                       and "TRIP" not in l.upper()[:12])
    missed, total = sv_gaps(prot)
    if missed == 0:
        missed_mu, _ = sv_gaps(mu)
        missed = max(missed, missed_mu)
    lat = (trip_recv - trip_sent).total_seconds() * 1000.0 if (trip_sent and trip_recv) else None
    return {
        "sv_missed": missed,
        "sv_frames": total,
        "trip_latency_ms": round(lat, 3) if lat is not None else "",
        "trip_received": "yes" if trip_recv else "no",
        "cb1_opened": "yes" if cb_open else "no",
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=10)
    ap.add_argument("--only", default="")
    ap.add_argument("--manual", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--out", default="results")
    a = ap.parse_args()

    only = [s.strip() for s in a.only.split(",") if s.strip()]
    conds = [c for c in CONDITIONS if not only or c[0] in only]
    os.makedirs(a.out, exist_ok=True)

    if not a.dry_run:
        ensure_up()

    rows = []
    for name, spec in conds:
        for trial in range(1, a.trials + 1):
            tag = f"{name}_{trial:02d}"
            print(f"\n=== {tag} ===")
            if a.dry_run:
                for k, expr in spec:
                    print(f"  docker exec {CTR[k]} tc qdisc replace dev {IFACE} root netem {expr}")
                print(f"  docker attach {CTR['sim']}  ->  '3', '1'")
                continue

            clear_netem()
            send_cmd("3", a.manual)          # normal / reset
            time.sleep(SETTLE_S)
            apply_netem(spec)
            if name == "congestion":
                subprocess.Popen(
                    ["docker", "compose", "--profile", "attack", "run", "--rm",
                     "goose_attacker", "-m", "flood", "-c", "0", "-p", "5"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(1.0)

            since = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
            send_cmd("1", a.manual)          # ariza
            time.sleep(FAULT_S)

            rawdir = os.path.join(a.out, "raw", tag)
            paths = dump_logs(since, rawdir)
            m = metrics(paths)
            clear_netem()
            sh("docker rm -f goose_attacker 2>/dev/null")
            send_cmd("3", a.manual)          # toparla
            time.sleep(1.0)

            row = {"condition": name, "trial": trial, **m}
            rows.append(row)
            print("  ", row)

    if a.dry_run:
        return

    csv_path = os.path.join(a.out, "impairment_matrix.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nCSV: {csv_path}")

    # ozet
    md = ["| Impairment | Trials | SV frames missed | GOOSE trip latency (ms) | Trip received | CB1 opened |",
          "|---|---|---|---|---|---|"]
    for name, _ in conds:
        rs = [r for r in rows if r["condition"] == name]
        if not rs:
            continue
        lat = [r["trip_latency_ms"] for r in rs if r["trip_latency_ms"] != ""]
        lat_s = (f"{statistics.mean(lat):.2f} +- {statistics.stdev(lat):.2f}"
                 if len(lat) > 1 else (f"{lat[0]:.2f}" if lat else "n/a"))
        missed = "/".join(str(r["sv_missed"]) for r in rs)
        ok_t = sum(1 for r in rs if r["trip_received"] == "yes")
        ok_c = sum(1 for r in rs if r["cb1_opened"] == "yes")
        md.append(f"| {name} | {len(rs)} | {missed} | {lat_s} | {ok_t}/{len(rs)} | {ok_c}/{len(rs)} |")
    md_path = os.path.join(a.out, "summary.md")
    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))
    print(f"\nozet: {md_path}")


if __name__ == "__main__":
    main()

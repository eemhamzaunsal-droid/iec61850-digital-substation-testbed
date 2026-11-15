/*
 * ============================================================================
 * SCADA SİSTEMİ — Gerçek Dijital Trafo Merkezi Standardı
 * ============================================================================
 * ETH ARAYÜZ DAĞILIMI:
 *   Tek arayüz: VLAN20 (Station Bus) — TCP/IP üzerinden MMS
 *
 * GÖREVLER (IEC 61850 — MMS Client):
 *   OKUMA:
 *     - Breaker IED (IED3_BCU): CB1 açık/kapalı durumu, alarm, olay kayıtları
 *     - Protection IED (IED2_PROT): CT1 akımı, trip durumu, arıza raporları
 *
 *   YAZMA / KOMUT:
 *     - Breaker IED'ye: CB1 Aç [O] / Kapat [C] komutu (MMS Write)
 *
 *   HMI VE HISTORIAN:
 *     - ASCII tek hat şeması (Single Line Diagram) ekranda gösterilir
 *     - Tüm olaylar zaman damgalı scada_fault_history.log'a kaydedilir
 *
 * NOT: GOOSE döngüsüne girmez. Sadece MMS Client rolündedir.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "iec61850_client.h"
#include "hal_thread.h"

/* -------------------------------------------------------------------------
 * IED IP ADRESLERİ (VLAN20 — Station Bus)
 * ---------------------------------------------------------------------- */
static const char *PROT_IP;  /* Protection IED — IED2_PROT */
static const char *BCU_IP;   /* Breaker IED    — IED3_BCU  */

/* -------------------------------------------------------------------------
 * RENK KODLARI
 * ---------------------------------------------------------------------- */
#define RST   "\x1B[0m"
#define RED   "\x1B[31m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define CYN   "\x1B[36m"
#define WHT   "\x1B[37m"
#define BRED  "\x1B[1;31m"  /* Bold Red */
#define BGRN  "\x1B[1;32m"  /* Bold Green */
#define BYEL  "\x1B[1;33m"  /* Bold Yellow */

/* -------------------------------------------------------------------------
 * SCADA VERİ YAPISI
 * Only CB1 and CT1 — spec gereği
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Breaker IED → CB1 */
    bool  cb1_pos;          /* 1=KAPALI, 0=AÇIK */
    bool  cb1_interlock;    /* Kilitleme durumu */
    int   cb1_op_count;     /* Operasyon sayacı */
    bool  bcu_conn;         /* MMS bağlantı durumu */

    /* Protection IED → CT1 */
    float ct1_amp;          /* CT1 / Feeder_1 akımı */
    bool  prot_trip;        /* Trip aktif mi? */
    bool  bfp_active;       /* BFP devrede mi? */
    int   fault_count;      /* Toplam arıza sayısı */
    bool  prot_conn;        /* MMS bağlantı durumu */

    bool  last_prot_trip;   /* Önceki trip durumu (log için) */
    bool  last_bfp;         /* Önceki BFP durumu (log için) */
    bool  last_cb1_pos;     /* Önceki CB1 durumu (log için) */
} ScadaData;

static ScadaData data = {0};

/* -------------------------------------------------------------------------
 * HISTORIAN — Milisaniye hassasiyetli olay kaydı
 * ---------------------------------------------------------------------- */
static void log_event(const char *source, const char *event) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    FILE *f = fopen("scada_fault_history.log", "a");
    if (f) {
        fprintf(f, "[%s.%03ld] SOURCE:%s EVENT:%s\n",
                ts, tv.tv_usec / 1000, source, event);
        fclose(f);
    }
}

/* -------------------------------------------------------------------------
 * ASCII TEK HAT ŞEMASİ — HMI EKRANI
 * ---------------------------------------------------------------------- */
static void print_sld(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    /* Ekran temizle */
    printf("\033[H\033[J");

    /* ------------------------------------------------------------------ */
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║     SCADA — DİJİTAL TRAFO MERKEZİ KONTROL MERKEZİ          ║\n");
    printf("║  %s  ║\n", ts);
    printf("╠══════════════════════════════════════════════════════════════╣\n");

    /* Aktif alarmlar */
    if (data.prot_trip) {
        printf("║  %s!!! KRİTİK ALARM: PROTECTION IED TRIP AKTİF !!!%s           ║\n", BRED, RST);
    }
    if (data.bfp_active) {
        printf("║  %s!!! BFP DEVREDE: KESİCİ AÇILMADI — GENİŞLETİLMİŞ TRIP !!!%s ║\n", BRED, RST);
    }
    if (!data.bcu_conn || !data.prot_conn) {
        printf("║  %s!!! IED BAĞLANTISI KESİLDİ — İLETİŞİM HATASI !!!%s          ║\n", BRED, RST);
    }
    if (!data.prot_trip && !data.bfp_active && data.bcu_conn && data.prot_conn) {
        printf("║  %s✓ Sistem normal çalışıyor — Alarm yok.%s                     ║\n", BGRN, RST);
    }

    printf("╠══════════════════════════════════════════════════════════════╣\n");

    /* ------------------------------------------------------------------ */
    /* TEK HAT ŞEMASİ                                                      */
    /* ------------------------------------------------------------------ */
    printf("║                                                              ║\n");
    printf("║   ŞEBEKE                                                     ║\n");
    printf("║     │                                                        ║\n");
    printf("║   [TRAFO]                                                    ║\n");
    printf("║     │                                                        ║\n");
    printf("║     │  ANA BARA (MV)                                         ║\n");
    printf("║  ───┴──────────────────────────                              ║\n");
    printf("║     │                                                        ║\n");
    printf("║  [CB1 / Feeder_1]  %s%-8s%s  %s%-8s%s            ║\n",
           data.bcu_conn ? (data.cb1_pos ? BGRN : BRED) : YEL,
           data.bcu_conn ? (data.cb1_pos ? "■ KAPALI" : "○ AÇIK  ") : "? BAĞSIZ",
           RST,
           data.cb1_interlock ? BYEL : "",
           data.cb1_interlock ? "[KİLİTLİ]" : "",
           RST);
    printf("║     │                                                        ║\n");
    printf("║   [LOAD 1]                                                   ║\n");
    printf("║                                                              ║\n");

    /* ------------------------------------------------------------------ */
    /* ÖLÇÜM DEĞERLERİ                                                     */
    /* ------------------------------------------------------------------ */
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  PROTECTION IED — CT1 / Feeder_1 Ölçümleri                  ║\n");
    printf("║  Bağlantı: %s%-8s%s                                           ║\n",
           data.prot_conn ? BGRN : BRED,
           data.prot_conn ? "BAĞLI   " : "KESİLDİ",
           RST);
    printf("║  CT1 Akım  : %s%.2f A%s                                         ║\n",
           data.ct1_amp > 300.0f ? BRED : (data.ct1_amp > 200.0f ? BYEL : BGRN),
           data.ct1_amp, RST);
    printf("║  Trip Durumu: %s%-12s%s                                   ║\n",
           data.prot_trip ? BRED : BGRN,
           data.prot_trip ? "!! TRIP !!" : "NORMAL",
           RST);
    printf("║  BFP Durumu : %s%-12s%s                                   ║\n",
           data.bfp_active ? BRED : BGRN,
           data.bfp_active ? "DEVREDE" : "PASIF",
           RST);
    printf("║  Arıza Sayısı: %d                                            ║\n",
           data.fault_count);

    /* ------------------------------------------------------------------ */
    /* BREAKER IED — CB1 DETAYI                                            */
    /* ------------------------------------------------------------------ */
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  BREAKER IED — CB1 Durum Bilgisi                             ║\n");
    printf("║  Bağlantı    : %s%-8s%s                                      ║\n",
           data.bcu_conn ? BGRN : BRED,
           data.bcu_conn ? "BAĞLI   " : "KESİLDİ",
           RST);
    printf("║  CB1 Pozisyon: %s%-10s%s                                    ║\n",
           data.bcu_conn ? (data.cb1_pos ? BGRN : BRED) : YEL,
           data.bcu_conn ? (data.cb1_pos ? "■ KAPALI  " : "○ AÇIK    ") : "? BİLİNMİYOR",
           RST);
    printf("║  Kilitleme   : %s%-10s%s                                    ║\n",
           data.cb1_interlock ? BYEL : BGRN,
           data.cb1_interlock ? "BLOKE     " : "SERBEST   ",
           RST);
    printf("║  Op. Sayısı  : %d                                            ║\n",
           data.cb1_op_count);

    /* ------------------------------------------------------------------ */
    /* KOMUT MENÜSÜ                                                         */
    /* ------------------------------------------------------------------ */
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  OPERATÖR KOMUTLARI (Station Bus — MMS Write):               ║\n");
    printf("║   [O] + ENTER → CB1 Aç (MMS → Breaker IED → VLAN30 → CB1)  ║\n");
    printf("║   [C] + ENTER → CB1 Kapat                                   ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/* -------------------------------------------------------------------------
 * KLAVYE KOMUTU İŞLEYİCİ — MMS Write ile Breaker IED'ye komut
 * ---------------------------------------------------------------------- */
static void *input_handler(void *arg) {
    (void)arg;
    char line[16];
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) continue;
        char cmd = line[0];
        if (cmd == '\n' || cmd == '\r') continue;

        IedClientError err;
        IedConnection con = IedConnection_create();

        if (cmd == 'O' || cmd == 'o') {
            /* CB1 Aç — Breaker IED'ye MMS Write (VLAN20) */
            log_event("SCADA", "Manual Command: CB1 Open");
            IedConnection_connect(con, &err, BCU_IP, 102);
            if (err == IED_ERROR_OK) {
                MmsValue *val = MmsValue_newBoolean(0);
                IedConnection_writeObject(con, &err,
                    "IED3_BCU/LD1/XCBR1.Pos.stVal",
                    IEC61850_FC_SP, val);
                MmsValue_delete(val);
                printf("\n[SCADA] CB1 AC komutu gonderildi (MMS → IED3_BCU)\n");
                if (err != IED_ERROR_OK)
                    printf("[SCADA] UYARI: Yazma hatasi (err=%d)\n", err);
                IedConnection_close(con);
            } else {
                printf("\n[SCADA] HATA: Breaker IED'ye baglanamadi!\n");
            }

        } else if (cmd == 'C' || cmd == 'c') {
            /* CB1 Kapat — Breaker IED'ye MMS Write (VLAN20) */
            log_event("SCADA", "Manual Command: CB1 Close");
            IedConnection_connect(con, &err, BCU_IP, 102);
            if (err == IED_ERROR_OK) {
                MmsValue *val = MmsValue_newBoolean(1);
                IedConnection_writeObject(con, &err,
                    "IED3_BCU/LD1/XCBR1.Pos.stVal",
                    IEC61850_FC_SP, val);
                MmsValue_delete(val);
                printf("\n[SCADA] CB1 KAPAT komutu gonderildi (MMS → IED3_BCU)\n");
                if (err != IED_ERROR_OK)
                    printf("[SCADA] UYARI: Yazma hatasi (err=%d)\n", err);
                IedConnection_close(con);
            } else {
                printf("\n[SCADA] HATA: Breaker IED'ye baglanamadi!\n");
            }
        }

        IedConnection_destroy(con);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * ANA POLLING DÖNGÜSÜ — MMS Client, sadece BCU ve PROT
 * ---------------------------------------------------------------------- */
static void poll_ieds(void) {
    IedConnection prot_con = IedConnection_create();
    IedConnection bcu_con  = IedConnection_create();
    IedClientError err;

    /* Klavye thread başlat */
    Thread kbd = Thread_create((ThreadExecutionFunction)input_handler, NULL, 1);
    Thread_start(kbd);

    while (1) {
        /* ============================================================
         * BREAKER IED POLLING — CB1 Pozisyon + Kilitleme + Op Sayısı
         * ============================================================ */
        if (!data.bcu_conn) {
            IedConnection_connect(bcu_con, &err, BCU_IP, 102);
            if (err == IED_ERROR_OK) {
                data.bcu_conn = 1;
                log_event("BCU", "MMS Connected");
            }
        }

        if (data.bcu_conn) {
            /* CB1 Pozisyon */
            MmsValue *pos = IedConnection_readObject(bcu_con, &err,
                "IED3_BCU/LD1/XCBR1.Pos.stVal", IEC61850_FC_SP);
            if (err == IED_ERROR_OK && pos) {
                bool new_pos = MmsValue_getBoolean(pos);
                if (new_pos != data.last_cb1_pos) {
                    log_event("BCU", new_pos ? "CB1: KAPANDI (CLOSED)"
                                             : "CB1: ACILDI (OPEN)");
                    data.last_cb1_pos = new_pos;
                }
                data.cb1_pos = new_pos;
                MmsValue_delete(pos);

                /* Kilitleme durumu */
                MmsValue *ilk = IedConnection_readObject(bcu_con, &err,
                    "IED3_BCU/LD1/XCBR1.Interlock.stVal", IEC61850_FC_ST);
                if (err == IED_ERROR_OK && ilk) {
                    data.cb1_interlock = MmsValue_getBoolean(ilk);
                    MmsValue_delete(ilk);
                }

                /* Operasyon sayacı */
                MmsValue *opc = IedConnection_readObject(bcu_con, &err,
                    "IED3_BCU/LD1/XCBR1.OpCnt.stVal", IEC61850_FC_ST);
                if (err == IED_ERROR_OK && opc) {
                    data.cb1_op_count = MmsValue_toInt32(opc);
                    MmsValue_delete(opc);
                }

            } else {
                IedConnection_close(bcu_con);
                data.bcu_conn = 0;
                log_event("BCU", "ALARM: IED Connection Lost");
            }
        }

        /* ============================================================
         * PROTECTION IED POLLING — CT1 akım + Trip + BFP + Arıza
         * ============================================================ */
        if (!data.prot_conn) {
            IedConnection_connect(prot_con, &err, PROT_IP, 102);
            if (err == IED_ERROR_OK) {
                data.prot_conn = 1;
                log_event("PROT", "MMS Connected");
            }
        }

        if (data.prot_conn) {
            /* Trip durumu */
            MmsValue *trip = IedConnection_readObject(prot_con, &err,
                "IED2_PROT/LD1/PTRC1.Tr.stVal", IEC61850_FC_ST);
            if (err == IED_ERROR_OK && trip) {
                bool new_trip = MmsValue_getBoolean(trip);
                if (new_trip && !data.last_prot_trip)
                    log_event("PROT", "CRITICAL ALARM: Overcurrent TRIP (CT1 > 300A)");
                else if (!new_trip && data.last_prot_trip)
                    log_event("PROT", "Trip cleared — system normal");
                data.last_prot_trip = new_trip;
                data.prot_trip = new_trip;
                MmsValue_delete(trip);

                /* BFP durumu */
                MmsValue *bfp = IedConnection_readObject(prot_con, &err,
                    "IED2_PROT/LD1/PTRC1.BFP.stVal", IEC61850_FC_ST);
                if (err == IED_ERROR_OK && bfp) {
                    bool new_bfp = MmsValue_getBoolean(bfp);
                    if (new_bfp && !data.last_bfp)
                        log_event("PROT", "CRITICAL ALARM: BFP DEVREYE GIRDI");
                    data.last_bfp   = new_bfp;
                    data.bfp_active = new_bfp;
                    MmsValue_delete(bfp);
                }

                /* Arıza sayısı */
                MmsValue *fc = IedConnection_readObject(prot_con, &err,
                    "IED2_PROT/LD1/PTRC1.FaultCnt.stVal", IEC61850_FC_ST);
                if (err == IED_ERROR_OK && fc) {
                    data.fault_count = MmsValue_toInt32(fc);
                    MmsValue_delete(fc);
                }

                /* CT1 Akımı */
                MmsValue *amp = IedConnection_readObject(prot_con, &err,
                    "IED2_PROT/LD1/MMXU1.Amp.mag", IEC61850_FC_MX);
                if (err == IED_ERROR_OK && amp) {
                    data.ct1_amp = MmsValue_toFloat(amp);
                    MmsValue_delete(amp);
                }

            } else {
                IedConnection_close(prot_con);
                data.prot_conn = 0;
                log_event("PROT", "ALARM: IED Connection Lost");
            }
        }

        /* HMI ekranını güncelle */
        print_sld();

        Thread_sleep(500);
    }
}

/* -------------------------------------------------------------------------
 * ANA PROGRAM
 * ---------------------------------------------------------------------- */
int main(void) {
    setbuf(stdout, NULL);

    /* VLAN20 IP adresleri */
    PROT_IP = getenv("PROT_IP") ? getenv("PROT_IP") : "10.10.20.212";
    BCU_IP  = getenv("BCU_IP")  ? getenv("BCU_IP")  : "10.10.20.213";

    printf("SCADA başlatılıyor...\n");
    printf("  Protection IED : %s\n", PROT_IP);
    printf("  Breaker IED    : %s\n\n", BCU_IP);
    printf("  [O] + ENTER = CB1 Aç  |  [C] + ENTER = CB1 Kapat\n\n");

    log_event("SCADA", "System Started");
    poll_ieds();
    return 0;
}

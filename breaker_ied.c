/*
 * ============================================================================
 * BREAKER IED_1 (BCU) — Gerçek Dijital Trafo Merkezi Standardı
 * ============================================================================
 * ETH ARAYÜZ DAĞILIMI:
 *   eth0 (VLAN10 - Process Bus):
 *     - GOOSE Subscriber: Protection IED trip komutu (AppID: 0x8001)
 *     - GOOSE Subscriber: BFP genişletilmiş trip     (AppID: 0x8004)
 *     - GOOSE Subscriber: Control IED komutu         (AppID: 0x8008)
 *     - GOOSE Publisher : Kesici pozisyon geri bildirimi (AppID: 0x8003, <4ms)
 *
 *   eth1 (VLAN20 - Station Bus):
 *     - GOOSE Subscriber: Kilitleme/bara koruma (AppID: 0x8007)
 *     - GOOSE Publisher : Durum/kilitleme bilgisi (AppID: 0x8009)
 *     - MMS Server      : SCADA kesici durumu, açma/kapama komutu (Port 102)
 *
 *   eth2 (VLAN30 - Simulation Net):
 *     - UDP Client: Fiziksel CB1'i kontrol eder (streamer.py'ye TRIP_F1/CLOSE_F1)
 *     - Bu arayüz sadece güç sistemi (simülatör) ile konuşur.
 *
 * GÖREVLER:
 *   - Protection IED GOOSE Trip → CB1 açılır → UDP simülatöre → GOOSE geri bildirim
 *   - BFP GOOSE → komşu bay kesicisi açılır
 *   - SCADA MMS Write → kilitleme kontrolü → CB1 aç/kapat
 *   - Her değişimde Process Bus'a <4ms GOOSE pozisyon yayını
 * ============================================================================
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include "goose_receiver.h"
#include "goose_subscriber.h"
#include "goose_publisher.h"
#include "iec61850_server.h"
#include "hal_thread.h"
#include "hal_time.h"

/* -------------------------------------------------------------------------
 * AYARLAR
 * ---------------------------------------------------------------------- */
#define WINDOWS_DEFAULT_IP    "10.10.30.216"
#define WINDOWS_CTRL_PORT     5006

/* GOOSE AppID'leri */
#define APPID_PROT_TRIP       0x8001   /* Protection IED trip (PB) */
#define APPID_BCU_POS         0x8003   /* Breaker IED pozisyon (PB) */
#define APPID_BFP_EXT_TRIP    0x8004   /* BFP genişletilmiş trip (PB) */
#define APPID_PROT_STATUS_SB  0x8005   /* Protection IED durum (SB) */
#define APPID_CTRL_CMD        0x8008   /* Control IED komutu (PB) */
#define APPID_CTRL_INTERLOCK  0x8007   /* Control IED kilitleme (SB) */
#define APPID_BCU_STATUS_SB   0x8009   /* BCU durum (SB) */

/* GOOSE Multicast MAC'leri */
static const uint8_t MC_POS[6]       = {0x01,0x0c,0xcd,0x01,0x00,0x03};
static const uint8_t MC_BCU_SB[6]    = {0x01,0x0c,0xcd,0x01,0x00,0x09};

/* -------------------------------------------------------------------------
 * GLOBAL DURUM
 * ---------------------------------------------------------------------- */
static volatile int  running       = 1;
static volatile int  breaker_state = 1;  /* 1=KAPALI, 0=AÇIK */
static volatile int  interlock_ok  = 1;  /* 1=serbest, 0=bloke */
static int           op_count      = 0;

static const char   *iface_pb  = "eth0"; /* VLAN10 — Process Bus */
static const char   *iface_sb  = "eth1"; /* VLAN20 — Station Bus */
static const char   *iface_sim = "eth2"; /* VLAN30 — Simulation Net */
static const char   *windows_ip;

/* MMS */
static IedServer     iedServer     = NULL;
static DataAttribute *da_pos_stVal = NULL;
static DataAttribute *da_interlock = NULL;
static DataAttribute *da_op_count  = NULL;

/* GOOSE Publisher'lar */
static GoosePublisher pub_pos      = NULL; /* PB pozisyon */
static GoosePublisher pub_status   = NULL; /* SB durum */

/* -------------------------------------------------------------------------
 * SİNYAL İŞLEYİCİ
 * ---------------------------------------------------------------------- */
static void sig_handler(int s) { (void)s; running = 0; }

/* -------------------------------------------------------------------------
 * YARDIMCI: Zaman etiketi
 * ---------------------------------------------------------------------- */
static void get_ts(char *buf, size_t len) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char base[32]; strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", t);
    snprintf(buf, len, "%s.%03ld", base, tv.tv_usec / 1000);
}

static void log_latency_micro(const char *event_msg) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char base[64]; strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", t);
    FILE *f = fopen("logs/ied_latency.log", "a");
    if (f) {
        fprintf(f, "[%s.%06ld] %s\n", base, (long)tv.tv_usec, event_msg);
        fclose(f);
    }
}

static void log_e2e_micro(const char *event_msg) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char base[64]; strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", t);
    FILE *f = fopen("logs/end_to_end_analysis.log", "a");
    if (f) {
        fprintf(f, "[%s.%06ld] %s\n", base, (long)tv.tv_usec, event_msg);
        fclose(f);
    }
}

/* -------------------------------------------------------------------------
 * VLAN30 — SİMÜLATÖRE UDP KOMUTU GÖNDER (CB1 fiziksel kontrolü)
 * UDP dogrudan simulatorun IP adresine gonderilir.
 * SO_BINDTODEVICE kullanilmaz (-std=c99 uyumsuz).
 * ---------------------------------------------------------------------- */
static void send_to_simulator(const char *cmd) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family      = AF_INET;
    serv.sin_port        = htons(WINDOWS_CTRL_PORT);
    serv.sin_addr.s_addr = inet_addr(windows_ip);

    sendto(sock, cmd, strlen(cmd), 0, (struct sockaddr *)&serv, sizeof(serv));
    close(sock);

    char ts[64]; get_ts(ts, sizeof(ts));
    printf("[%s] [VLAN30-UDP] CB1 komutu gonderildi: %s -> %s:%d\n",
           ts, cmd, windows_ip, WINDOWS_CTRL_PORT);
}

/* -------------------------------------------------------------------------
 * PROCESS BUS — POZİSYON GOOSE YAYINLA (<4ms)
 * Protection IED bu paketi BFP zamanlayıcısı için bekler.
 * ---------------------------------------------------------------------- */
static void publish_pos_goose(void) {
    if (!pub_pos) return;
    LinkedList ds = LinkedList_create();
    LinkedList_add(ds, MmsValue_newBoolean(breaker_state));
    LinkedList_add(ds, MmsValue_newBoolean(interlock_ok));
    GoosePublisher_publish(pub_pos, ds);
    LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);

    char ts[64]; get_ts(ts, sizeof(ts));
    printf("[%s] [GOOSE-PB] Pozisyon yayinlandi: %s (AppID:0x%04X)\n",
           ts, breaker_state ? "KAPALI" : "ACIK", APPID_BCU_POS);
}

/* -------------------------------------------------------------------------
 * STATION BUS — DURUM GOOSE YAYINLA
 * ---------------------------------------------------------------------- */
static void publish_status_goose_sb(void) {
    if (!pub_status) return;
    LinkedList ds = LinkedList_create();
    LinkedList_add(ds, MmsValue_newBoolean(breaker_state));
    LinkedList_add(ds, MmsValue_newBoolean(interlock_ok));
    GoosePublisher_publish(pub_status, ds);
    LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);
}

/* -------------------------------------------------------------------------
 * MMS MODELİ GÜNCELLE
 * ---------------------------------------------------------------------- */
static void update_mms(void) {
    if (!iedServer) return;
    IedServer_lockDataModel(iedServer);
    MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_pos_stVal),  breaker_state);
    MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_interlock),  interlock_ok);
    MmsValue_setInt32  (IedServer_getAttributeValue(iedServer, da_op_count),   op_count);
    IedServer_unlockDataModel(iedServer);
}

/* -------------------------------------------------------------------------
 * KESİCİ DURUMU GÜNCELLE — Ana kontrol fonksiyonu
 * ---------------------------------------------------------------------- */
static void update_breaker(int new_state, const char *source) {
    if (new_state == breaker_state) return;

    /* Kilitleme kontrolü — sadece KAPATMA komutunda */
    if (new_state == 1 && !interlock_ok) {
        char ts[64]; get_ts(ts, sizeof(ts));
        printf("[%s] [KİLİTLEME] KAPAT komutu BLOKE edildi! (Kaynak:%s)\n",
               ts, source);
        return;
    }

    breaker_state = new_state;
    op_count++;

    char ts[64]; get_ts(ts, sizeof(ts));
    printf("[%s] [CB1] %s  (Kaynak:%s | Op#%d)\n",
           ts,
           new_state ? "KAPANDI (CLOSED)" : "ACILDI (OPEN)",
           source, op_count);

    /* 1. VLAN30: CB1 fiziksel kontrolü — simülatöre UDP */
    send_to_simulator(new_state ? "CLOSE_F1" : "TRIP_F1");

    /* 2. MMS modeli → SCADA raporu */
    update_mms();

    /* 3. Process Bus: Pozisyon GOOSE (<4ms) — Protection IED BFP için */
    log_latency_micro("BREAKER_IED: GOOSE POS SENT (CB STATUS CHANGED)");
    if (strcmp(source, "GOOSE_PROT_TRIP") == 0) {
        log_e2e_micro("5. BREAKER_IED: CB OPENED & GOOSE POS SENT");
    }
    publish_pos_goose();

    /* 4. Station Bus: Durum GOOSE */
    publish_status_goose_sb();
}

/* -------------------------------------------------------------------------
 * GOOSE DİNLEYİCİ — Protection IED Trip (Process Bus)
 * ---------------------------------------------------------------------- */
static void gooseListener_ProtTrip(GooseSubscriber sub, void *param) {
    (void)param;
    MmsValue *values = GooseSubscriber_getDataSetValues(sub);
    if (!values) return;
    MmsValue *trip_f1 = MmsValue_getElement(values, 0);
    if (!trip_f1) return;

    if (MmsValue_getBoolean(trip_f1)) {
        log_latency_micro("BREAKER_IED: GOOSE TRIP RECEIVED (OPENING CB)");
        log_e2e_micro("4. BREAKER_IED: GOOSE TRIP RECEIVED (PROCESSING...)");
       // Store receive timestamp (ms) for latency measurement
       {
           uint64_t recvTime = Hal_getTimeInMs();
           FILE *f = fopen("linux_files/logs/goose_trip_latency.log", "a");
           if (f){
               fprintf(f, "RECV %llu\n", (unsigned long long)recvTime);
               fclose(f);
           }
       }
        update_breaker(0, "GOOSE_PROT_TRIP");
    } else {
        char ts[64]; get_ts(ts, sizeof(ts));
        printf("[%s] [GOOSE-PB] Protection IED trip sinyali kaldirildi.\n", ts);
    }
}

/* -------------------------------------------------------------------------
 * GOOSE DİNLEYİCİ — BFP Genişletilmiş Trip (Process Bus)
 * ---------------------------------------------------------------------- */
static void gooseListener_BFPTrip(GooseSubscriber sub, void *param) {
    (void)param;
    MmsValue *values = GooseSubscriber_getDataSetValues(sub);
    if (!values) return;
    MmsValue *bfp_val = MmsValue_getElement(values, 0);
    if (bfp_val && MmsValue_getBoolean(bfp_val)) {
        char ts[64]; get_ts(ts, sizeof(ts));
        printf("[%s] [BFP] Genisletilmis trip alindi — Komsu bay CB aciliyor.\n", ts);
        update_breaker(0, "GOOSE_BFP_EXTENDED");
    }
}

/* -------------------------------------------------------------------------
 * GOOSE DİNLEYİCİ — Control IED Komutu (Process Bus)
 * ---------------------------------------------------------------------- */
static void gooseListener_CtrlCmd(GooseSubscriber sub, void *param) {
    (void)param;
    MmsValue *values = GooseSubscriber_getDataSetValues(sub);
    if (!values) return;
    MmsValue *cmd_val = MmsValue_getElement(values, 0);
    if (!cmd_val) return;

    int new_state = MmsValue_getBoolean(cmd_val);
    update_breaker(new_state, "GOOSE_CTRL_IED");
}

/* -------------------------------------------------------------------------
 * GOOSE DİNLEYİCİ — Control IED Kilitleme (Station Bus)
 * ---------------------------------------------------------------------- */
static void gooseListener_Interlock(GooseSubscriber sub, void *param) {
    (void)param;
    MmsValue *values = GooseSubscriber_getDataSetValues(sub);
    if (!values) return;
    MmsValue *block = MmsValue_getElement(values, 0);
    if (!block) return;

    int prev = interlock_ok;
    interlock_ok = !MmsValue_getBoolean(block);

    if (prev != interlock_ok) {
        char ts[64]; get_ts(ts, sizeof(ts));
        printf("[%s] [KİLİTLEME] %s\n", ts,
               interlock_ok ? "SERBEST" : "BLOKE");
        update_mms();
        publish_status_goose_sb();
    }
}

/* -------------------------------------------------------------------------
 * MMS SERVER KURULUMU — Station Bus (eth1 / VLAN20)
 * ---------------------------------------------------------------------- */
static void setup_mms_server(void) {
    IedModel      *model = IedModel_create("IED3_BCU");
    LogicalDevice *ldev  = LogicalDevice_create("LD1", model);
    LogicalNode   *xcbr  = LogicalNode_create("XCBR1", ldev);

    DataObject *pos = DataObject_create("Pos",      (ModelNode *)xcbr, 0);
    da_pos_stVal    = DataAttribute_create("stVal",  (ModelNode *)pos,
                          IEC61850_BOOLEAN, IEC61850_FC_SP, 0, 0, 0);

    DataObject *ilk = DataObject_create("Interlock", (ModelNode *)xcbr, 0);
    da_interlock    = DataAttribute_create("stVal",   (ModelNode *)ilk,
                          IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);

    DataObject *opc = DataObject_create("OpCnt",    (ModelNode *)xcbr, 0);
    da_op_count     = DataAttribute_create("stVal",  (ModelNode *)opc,
                          IEC61850_INT32,   IEC61850_FC_ST, 0, 0, 0);

    iedServer = IedServer_create(model);

    IedServer_lockDataModel(iedServer);
    MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_pos_stVal),  breaker_state);
    MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_interlock),  interlock_ok);
    MmsValue_setInt32  (IedServer_getAttributeValue(iedServer, da_op_count),   0);
    IedServer_unlockDataModel(iedServer);

    IedServer_start(iedServer, 102);
    printf("[MMS] BCU Server baslatildi (Port:102 | Model:IED3_BCU)\n");
    printf("      IED3_BCU/LD1/XCBR1.Pos.stVal       (CB1 pozisyonu — yazilabilir)\n");
    printf("      IED3_BCU/LD1/XCBR1.Interlock.stVal (Kilitleme durumu)\n");
    printf("      IED3_BCU/LD1/XCBR1.OpCnt.stVal     (Operasyon sayaci)\n");
}

/* -------------------------------------------------------------------------
 * ANA PROGRAM
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);

    windows_ip = getenv("WINDOWS_IP") ? getenv("WINDOWS_IP") : WINDOWS_DEFAULT_IP;

    if (argc > 1) iface_pb  = argv[1];  /* eth0 — VLAN10 */
    if (argc > 2) iface_sb  = argv[2];  /* eth1 — VLAN20 */
    if (argc > 3) iface_sim = argv[3];  /* eth2 — VLAN30 */

    char ts[64]; get_ts(ts, sizeof(ts));
    printf("\n[%s] === Breaker IED_1 (IED3_BCU) Baslatiliyor ===\n", ts);
    printf("  Process Bus  (VLAN10): %s\n", iface_pb);
    printf("  Station Bus  (VLAN20): %s\n", iface_sb);
    printf("  Sim Net      (VLAN30): %s\n", iface_sim);
    printf("  Simulatör    : %s:%d\n\n", windows_ip, WINDOWS_CTRL_PORT);

    /* ------------------------------------------------------------------ */
    /* 1. PROCESS BUS GOOSE PUBLISHER — Pozisyon geri bildirimi           */
    /* ------------------------------------------------------------------ */
    CommParameters pub_p;
    memset(&pub_p, 0, sizeof(pub_p));
    pub_p.appId = APPID_BCU_POS; pub_p.vlanId = 0; pub_p.vlanPriority = 4;
    memcpy(pub_p.dstAddress, MC_POS, 6);
    pub_pos = GoosePublisher_create(&pub_p, iface_pb);
    if (!pub_pos) { fprintf(stderr, "HATA: Pozisyon GOOSE publisher yaratılamadı!\n"); return 1; }
    GoosePublisher_setGoCbRef(pub_pos, "IED3_BCU/LLN0$GO$Pos");
    GoosePublisher_setDataSetRef(pub_pos, "IED3_BCU/LLN0$PosDataSet");
    printf("[GOOSE-PB] Pozisyon publisher hazir (AppID:0x%04X)\n", APPID_BCU_POS);

    /* ------------------------------------------------------------------ */
    /* 2. STATION BUS GOOSE PUBLISHER — Durum/kilitleme bilgisi           */
    /* ------------------------------------------------------------------ */
    CommParameters pub_s;
    memset(&pub_s, 0, sizeof(pub_s));
    pub_s.appId = APPID_BCU_STATUS_SB; pub_s.vlanId = 0; pub_s.vlanPriority = 4;
    memcpy(pub_s.dstAddress, MC_BCU_SB, 6);
    pub_status = GoosePublisher_create(&pub_s, iface_sb);
    if (!pub_status) { fprintf(stderr, "HATA: Status GOOSE publisher yaratılamadı!\n"); return 1; }
    GoosePublisher_setGoCbRef(pub_status, "IED3_BCU/LLN0$GO$Status");
    GoosePublisher_setDataSetRef(pub_status, "IED3_BCU/LLN0$StatusDataSet");
    printf("[GOOSE-SB] Durum publisher hazir (AppID:0x%04X)\n", APPID_BCU_STATUS_SB);

    /* ------------------------------------------------------------------ */
    /* 3. PROCESS BUS GOOSE RECEIVER — Trip + BFP + Control IED           */
    /* ------------------------------------------------------------------ */
    GooseReceiver pb_rx = GooseReceiver_create();
    GooseReceiver_setInterfaceId(pb_rx, iface_pb);

    /* 3a. Protection IED Trip */
    GooseSubscriber sub_trip = GooseSubscriber_create("IED2_PROT/LLN0$GO$Trip", NULL);
    GooseSubscriber_setAppId(sub_trip, APPID_PROT_TRIP);
    GooseSubscriber_setListener(sub_trip, gooseListener_ProtTrip, NULL);
    GooseReceiver_addSubscriber(pb_rx, sub_trip);

    /* 3b. BFP Genişletilmiş Trip */
    GooseSubscriber sub_bfp = GooseSubscriber_create("IED2_PROT/LLN0$GO$BFPTrip", NULL);
    GooseSubscriber_setAppId(sub_bfp, APPID_BFP_EXT_TRIP);
    GooseSubscriber_setListener(sub_bfp, gooseListener_BFPTrip, NULL);
    GooseReceiver_addSubscriber(pb_rx, sub_bfp);

    /* 3c. Control IED Komutu */
    GooseSubscriber sub_ctrl = GooseSubscriber_create("IED4_CTRL/LLN0$GO$Cmd", NULL);
    GooseSubscriber_setAppId(sub_ctrl, APPID_CTRL_CMD);
    GooseSubscriber_setListener(sub_ctrl, gooseListener_CtrlCmd, NULL);
    GooseReceiver_addSubscriber(pb_rx, sub_ctrl);

    GooseReceiver_start(pb_rx);
    printf("[GOOSE-PB] Trip dinleyici hazir     (AppID:0x%04X)\n", APPID_PROT_TRIP);
    printf("[GOOSE-PB] BFP dinleyici hazir      (AppID:0x%04X)\n", APPID_BFP_EXT_TRIP);
    printf("[GOOSE-PB] Control cmd dinleyici    (AppID:0x%04X)\n", APPID_CTRL_CMD);

    /* ------------------------------------------------------------------ */
    /* 4. STATION BUS GOOSE RECEIVER — Kilitleme                          */
    /* ------------------------------------------------------------------ */
    GooseReceiver sb_rx = GooseReceiver_create();
    GooseReceiver_setInterfaceId(sb_rx, iface_sb);

    GooseSubscriber sub_ilk = GooseSubscriber_create("IED4_CTRL/LLN0$GO$Interlock", NULL);
    GooseSubscriber_setAppId(sub_ilk, APPID_CTRL_INTERLOCK);
    GooseSubscriber_setListener(sub_ilk, gooseListener_Interlock, NULL);
    GooseReceiver_addSubscriber(sb_rx, sub_ilk);
    GooseReceiver_start(sb_rx);
    printf("[GOOSE-SB] Kilitleme dinleyici hazir (AppID:0x%04X)\n", APPID_CTRL_INTERLOCK);

    /* ------------------------------------------------------------------ */
    /* 5. MMS SERVER — Station Bus (eth1 / VLAN20)                        */
    /* ------------------------------------------------------------------ */
    setup_mms_server();

    /* İlk durumu ilan et */
    publish_pos_goose();
    publish_status_goose_sb();

    /* ------------------------------------------------------------------ */
    /* HAZIR                                                               */
    /* ------------------------------------------------------------------ */
    get_ts(ts, sizeof(ts));
    printf("\n[%s] === Breaker IED_1 AKTIF ===\n", ts);
    printf("  CB1 Baslangic durumu: %s\n", breaker_state ? "KAPALI" : "ACIK");
    printf("  SCADA yazma nesnesi : IED3_BCU/LD1/XCBR1.Pos.stVal\n\n");

    /* ------------------------------------------------------------------ */
    /* ANA DÖNGÜ — SCADA MMS Write komutlarını denetle                    */
    /* ------------------------------------------------------------------ */
    int last_mms_state = breaker_state;

    while (running) {
        Thread_sleep(100);

        if (iedServer && da_pos_stVal) {
            int cur = MmsValue_getBoolean(
                IedServer_getAttributeValue(iedServer, da_pos_stVal));
            if (cur != last_mms_state) {
                last_mms_state = cur;
                if (cur != breaker_state)
                    update_breaker(cur, "MMS_SCADA");
            }
        }
    }

    /* Temizlik */
    printf("\nBreaker IED kapatiliyor...\n");
    GooseReceiver_stop(pb_rx); GooseReceiver_destroy(pb_rx);
    GooseReceiver_stop(sb_rx); GooseReceiver_destroy(sb_rx);
    if (pub_pos)    GoosePublisher_destroy(pub_pos);
    if (pub_status) GoosePublisher_destroy(pub_status);
    if (iedServer)  IedServer_stop(iedServer);
    return 0;
}

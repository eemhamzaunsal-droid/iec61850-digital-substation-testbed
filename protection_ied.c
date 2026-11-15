/*
 * ============================================================================
 * PROTECTION IED_1 — Gerçek Dijital Trafo Merkezi Standardı
 * ============================================================================
 * ETH ARAYÜZ DAĞILIMI:
 *   eth0 (VLAN10 - Process Bus):
 *     - SV  Subscriber  : MU_1'den CT1/Feeder_1 akımı (AppID: 0x4000)
 *     - GOOSE Publisher : Trip sinyali (AppID: 0x8001, < 4ms)
 *     - GOOSE Subscriber: Breaker IED pozisyon geri bildirimi (BFP)
 *
 *   eth1 (VLAN20 - Station Bus):
 *     - GOOSE Subscriber: Komşu koruma IED'leri (yatay haberleşme)
 *     - GOOSE Publisher : Durum/arıza/kilitleme bilgisi
 *     - MMS Server      : SCADA erişimi (Port 102)
 *     - MMS Client      : SCADA'dan eşik komutu
 *
 * KORUMA FONKSİYONU:
 *   CT1 (Feeder_1) > 400A → GOOSE Trip (< 4ms)
 *   Trip sonrası 150ms içinde kesici açılmazsa → BFP
 *   BFP: Komşu baylara genişletilmiş trip GOOSE
 *
 * MMS RAPORLAMA:
 *   Her arıza/trip/değişimde ms hassasiyetle MMS Report
 *   Her arıza sonrası COMTRADE formatında kayıt dosyasına yazılır
 * ============================================================================
 */

#include "goose_publisher.h"
#include "goose_receiver.h"
#include "goose_subscriber.h"
#include "hal_thread.h"
#include "hal_time.h"
#include "iec61850_server.h"
#include "sv_subscriber.h"
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * AYARLAR
 * ---------------------------------------------------------------------- */
#define OVERCURRENT_THRESHOLD_A 300.0f /* CT1/Feeder_1 aşırı akım eşiği */
#define BFP_TIMEOUT_MS 150             /* BFP: 150 ms */
#define COMTRADE_FILE "prot_fault.cfg"

/* GOOSE AppID'leri */
#define APPID_PROT_TRIP 0x8001
#define APPID_BCU_POS 0x8003
#define APPID_BFP_EXT_TRIP 0x8004
#define APPID_PROT_STATUS_SB 0x8005
#define APPID_NEIGHBOR_PROT 0x8006

/* GOOSE Multicast MAC'leri */
static const uint8_t MC_TRIP[6] = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
static const uint8_t MC_BFP[6] = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x04};
static const uint8_t MC_STATUS[6] = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x05};

/* -------------------------------------------------------------------------
 * GLOBAL DURUM
 * ---------------------------------------------------------------------- */
static volatile int running = 1;
static volatile int tripActive = 0;
static volatile int breakerOpened = 0; /* 0=bekleniyor, 1=açıldı, -1=BFP */
static volatile uint64_t tripSentTime = 0;
static volatile float CT1_current = 0.0f;
static volatile float CT1_voltage = 0.0f;
static volatile float threshold_A = OVERCURRENT_THRESHOLD_A;
static int fault_count = 0;

/* Telemetry global variables */
static volatile int sv_packet_count = 0;
static volatile int goose_packet_count = 0;
static volatile int mms_packet_count = 0;
static volatile float last_rtt = 0.0f;

static const char *iface_pb = "eth0"; /* Process Bus */
static const char *iface_sb = "eth1"; /* Station Bus */

static GoosePublisher pub_trip = NULL;
static GoosePublisher pub_bfp = NULL;
static GoosePublisher pub_status = NULL;
static IedServer iedServer = NULL;

static DataAttribute *da_trip_stVal = NULL;
static DataAttribute *da_CT1_amp = NULL;
static DataAttribute *da_CT1_vol = NULL;
static DataAttribute *da_threshold = NULL;
static DataAttribute *da_bfp_active = NULL;
static DataAttribute *da_fault_count = NULL;

/* -------------------------------------------------------------------------
 * YARDIMCI: Zaman etiketi
 * ---------------------------------------------------------------------- */
static void get_ts(char *buf, size_t len) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *t = localtime(&tv.tv_sec);
  char base[32];
  strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", t);
  snprintf(buf, len, "%s.%03ld", base, tv.tv_usec / 1000);
}

static void log_latency_micro(const char *event_msg) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *t = localtime(&tv.tv_sec);
  char base[64];
  strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", t);
  
  FILE *f = fopen("logs/ied_latency.log", "a");
  if (f) {
    fprintf(f, "[%s.%06ld] %s\n", base, (long)tv.tv_usec, event_msg);
    fclose(f);
  }
}

static void log_e2e_micro(const char *event_msg) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *t = localtime(&tv.tv_sec);
  char base[64];
  strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", t);
  
  FILE *f = fopen("logs/end_to_end_analysis.log", "a");
  if (f) {
    fprintf(f, "[%s.%06ld] %s\n", base, (long)tv.tv_usec, event_msg);
    fclose(f);
  }
}

/* -------------------------------------------------------------------------
 * COMTRADE ARIZA KAYDI
 * ---------------------------------------------------------------------- */
static void write_comtrade(float current, float voltage, const char *fault_type,
                           uint64_t trip_epoch_ms, uint64_t detect_to_trip_ms) {
  char ts[64];
  get_ts(ts, sizeof(ts));
  FILE *f = fopen(COMTRADE_FILE, "a");
  if (!f)
    return;
  fprintf(f,
          "\n--- FAULT RECORD #%d ---\n"
          "Station  : DIGITAL_SUBSTATION\n"
          "IED      : IED2_PROT\n"
          "Timestamp: %s\n"
          "FaultType: %s\n"
          "CT1_I    : %.3f A\n"
          "CT1_V    : %.3f V\n"
          "Threshold: %.1f A\n"
          "TripEpoch: %llu ms\n"
          "TripDelay: %llu ms\n"
          "Ch1,CT1_Current,A,%.3f\n"
          "Ch2,CT1_Voltage,V,%.3f\n"
          "---\n",
          fault_count, ts, fault_type, current, voltage, threshold_A,
          (unsigned long long)trip_epoch_ms,
          (unsigned long long)detect_to_trip_ms, current, voltage);
  fclose(f);
  printf("[COMTRADE] Ariza kaydedildi: %s (I=%.1fA, Tdetect=%llums)\n",
         COMTRADE_FILE, current, (unsigned long long)detect_to_trip_ms);
}

/* -------------------------------------------------------------------------
 * GOOSE PUBLISHER YARDIMCI
 * ---------------------------------------------------------------------- */
static GoosePublisher make_publisher(const char *iface, uint16_t appId,
                                     const uint8_t *mac, const char *gcbRef,
                                     const char *dsRef) {
  CommParameters p;
  memset(&p, 0, sizeof(p));
  p.appId = appId;
  p.vlanId = 0;
  p.vlanPriority = 4;
  memcpy(p.dstAddress, mac, 6);
  GoosePublisher pub = GoosePublisher_create(&p, iface);
  if (pub) {
    GoosePublisher_setGoCbRef(pub, gcbRef);
    GoosePublisher_setDataSetRef(pub, dsRef);
  }
  return pub;
}

/* -------------------------------------------------------------------------
 * GOOSE TRIP YAYINLAç — Process Bus
 * ---------------------------------------------------------------------- */
static void publish_trip(int tripF1, int bfpActive) {
  if (!pub_trip)
    return;
  LinkedList ds = LinkedList_create();
  LinkedList_add(ds, MmsValue_newBoolean(tripF1));
  LinkedList_add(ds, MmsValue_newBoolean(0));
  LinkedList_add(ds, MmsValue_newBoolean(bfpActive));
  GoosePublisher_publish(pub_trip, ds);
  LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);
}

/* -------------------------------------------------------------------------
 * BFP GENİŞLETİLMİŞ TRIP — Process Bus
 * ---------------------------------------------------------------------- */
static void publish_bfp(void) {
  if (!pub_bfp)
    return;
  LinkedList ds = LinkedList_create();
  LinkedList_add(ds, MmsValue_newBoolean(1));
  GoosePublisher_publish(pub_bfp, ds);
  LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);
  printf("[BFP] Genisletilmis trip yayinlandi (Process Bus)\n");
}

/* -------------------------------------------------------------------------
 * STATION BUS DURUM GOOSE
 * ---------------------------------------------------------------------- */
static void publish_status_sb(int tripF1, int bfpActive) {
  if (!pub_status)
    return;
  LinkedList ds = LinkedList_create();
  LinkedList_add(ds, MmsValue_newBoolean(tripF1));
  LinkedList_add(ds, MmsValue_newBoolean(bfpActive));
  LinkedList_add(ds, MmsValue_newFloat(CT1_current));
  GoosePublisher_publish(pub_status, ds);
  LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);
}

/* -------------------------------------------------------------------------
 * MMS MODELİ GÜNCELLE
 * ---------------------------------------------------------------------- */
static void update_mms(int tripF1, int bfpActive) {
  if (!iedServer)
    return;
  IedServer_lockDataModel(iedServer);
  MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_trip_stVal),
                      tripF1);
  MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_CT1_amp),
                    CT1_current);
  MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_CT1_vol),
                    CT1_voltage);
  MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_bfp_active),
                      bfpActive);
  MmsValue_setInt32(IedServer_getAttributeValue(iedServer, da_fault_count),
                    fault_count);
  IedServer_unlockDataModel(iedServer);
}

#include <netinet/in.h>
#include <sys/socket.h>

static void send_telemetry_udp(void) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    return;

  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons(5007);

  const char *windows_ip =
      getenv("WINDOWS_IP") ? getenv("WINDOWS_IP") : "10.10.30.216";
  dest.sin_addr.s_addr = inet_addr(windows_ip);

  // Calculate BFP countdown
  float bfp_cd = BFP_TIMEOUT_MS / 1000.0f;
  if (tripActive && breakerOpened == 0) {
    uint64_t elapsed = Hal_getTimeInMs() - tripSentTime;
    bfp_cd = (BFP_TIMEOUT_MS / 1000.0f) - ((float)elapsed / 1000.0f);
    if (bfp_cd < 0.0f)
      bfp_cd = 0.0f;
  }

  char payload[512];
  snprintf(
      payload, sizeof(payload),
      "{\"id\": \"PROT\", \"data\": {\"trip\": %d, \"bfp\": %d, \"rtt\": %.2f, "
      "\"bfp_cd\": %.2f, \"sv_pps\": %d, \"goose_pps\": %d, \"mms_pps\": %d}}",
      tripActive, (breakerOpened == -1), last_rtt, bfp_cd, sv_packet_count,
      goose_packet_count, mms_packet_count);

  sendto(sock, payload, strlen(payload), 0, (struct sockaddr *)&dest,
         sizeof(dest));
  close(sock);

  // Reset counters for the next second
  sv_packet_count = 0;
  goose_packet_count = 0;
  mms_packet_count = 0;
}

/* -------------------------------------------------------------------------
 * SV İŞLEYİCİ — Process Bus (eth0)
 * Sadece CT1 / Feeder_1 akımı (offset 32) işlenir.
 * ---------------------------------------------------------------------- */
static void svUpdateHandler(SVSubscriber sub, void *param,
                            SVSubscriber_ASDU asdu) {
  (void)sub;
  sv_packet_count++;

  /* Sadece CT1 / Feeder_1 */
  float i_f1 = (float)SVSubscriber_ASDU_getFLOAT64(asdu, 32);
  float v_hv = (float)SVSubscriber_ASDU_getFLOAT64(asdu, 0);

  CT1_current = i_f1;
  CT1_voltage = v_hv;

  /* SCADA'dan güncel eşiği oku */
  if (iedServer && da_threshold) {
    IedServer_lockDataModel(iedServer);
    threshold_A =
        MmsValue_toFloat(IedServer_getAttributeValue(iedServer, da_threshold));
    IedServer_unlockDataModel(iedServer);
  }

  /* === OVERCURRENT KORUMA === */
  if (i_f1 > threshold_A) {
    if (!tripActive) {
      char msg1[128];
      snprintf(msg1, sizeof(msg1), "2. PROT_IED: OVERCURRENT SV PACKET RECEIVED & PROCESSING (%.2fA)", i_f1);
      log_e2e_micro(msg1);
      
      uint64_t t0 = Hal_getTimeInMs();
      tripActive = 1;
      breakerOpened = 0;
      tripSentTime = t0;
      fault_count++;

      char ts[64];
      get_ts(ts, sizeof(ts));
      printf("[%s] [ALARM] CT1/Feeder_1 ASIRI AKIM: %.2fA > %.1fA — TRIP!\n",
             ts, i_f1, threshold_A);

      char log_msg[128];
      snprintf(log_msg, sizeof(log_msg), "PROT_IED: OVERCURRENT DETECTED IN SV (%.2fA) -> GOOSE TRIP SENT", i_f1);
      log_latency_micro(log_msg);
      log_e2e_micro("3. PROT_IED: GOOSE TRIP SENT");
      // Store send timestamp (ms) for latency measurement
      {
          FILE *f = fopen("linux_files/logs/goose_trip_latency.log", "a");
          if (f){
              fprintf(f, "SEND %llu\n", (unsigned long long)tripSentTime);
              fclose(f);
          }
      }
      
      publish_trip(1, 0);      /* Process Bus trip */
      publish_status_sb(1, 0); /* Station Bus durum */
      update_mms(1, 0);        /* MMS model → SCADA raporu */
      write_comtrade(i_f1, v_hv, "OVERCURRENT_F1", t0, 0);
    }
  } else {
    if (tripActive) {
      char ts[64];
      get_ts(ts, sizeof(ts));
      printf("[%s] [NORMAL] CT1 akimi normale dondu: %.2fA — TRIP KALDIRILDI\n",
             ts, i_f1);
      tripActive = 0;
      breakerOpened = 0;
      publish_trip(0, 0);
      publish_status_sb(0, 0);
      update_mms(0, 0);
    }
  }

  /* === BFP KONTROLÜ — her SV paketinde === */
  if (tripActive && breakerOpened == 0) {
    uint64_t elapsed = Hal_getTimeInMs() - tripSentTime;
    if ((int64_t)elapsed > BFP_TIMEOUT_MS) {
      char ts[64];
      get_ts(ts, sizeof(ts));
      printf("[%s] [BFP ALARM] Kesici %llums icinde ACILMADI! BFP devreye "
             "girdi.\n",
             ts, (unsigned long long)elapsed);

      publish_bfp();
      publish_status_sb(1, 1);
      update_mms(1, 1);
      write_comtrade(CT1_current, CT1_voltage, "BFP_EXTENDED_TRIP",
                     Hal_getTimeInMs(), elapsed);

      breakerOpened = -1; /* BFP bir kez tetiklensin */
    }
  }
}

/* -------------------------------------------------------------------------
 * GOOSE DİNLEYİCİ — Breaker IED Pozisyon (Process Bus) — BFP için
 * ---------------------------------------------------------------------- */
static void gooseListener_BreakerPos(GooseSubscriber sub, void *param) {
  (void)param;
  goose_packet_count++;
  MmsValue *values = GooseSubscriber_getDataSetValues(sub);
  if (!values)
    return;
  MmsValue *pos = MmsValue_getElement(values, 0);
  if (!pos)
    return;

  int breaker_closed = MmsValue_getBoolean(pos);
  if (tripActive && !breaker_closed && breakerOpened == 0) {
    uint64_t elapsed = Hal_getTimeInMs() - tripSentTime;
    last_rtt = (float)elapsed;
    log_latency_micro("PROT_IED: GOOSE POS RECEIVED");
    char ts[64];
    get_ts(ts, sizeof(ts));
    printf("[%s] [BFP OK] Kesici %llums icinde ACILDI — Koruma basarili.\n", ts,
           (unsigned long long)elapsed);
    breakerOpened = 1;
  }
}

/* -------------------------------------------------------------------------
 * GOOSE DİNLEYİCİ — Komşu Koruma IED (Station Bus) — Yatay haberleşme
 * ---------------------------------------------------------------------- */
static void gooseListener_Neighbor(GooseSubscriber sub, void *param) {
  (void)param;
  goose_packet_count++;
  MmsValue *values = GooseSubscriber_getDataSetValues(sub);
  if (!values)
    return;
  MmsValue *trip = MmsValue_getElement(values, 0);
  if (trip && MmsValue_getBoolean(trip)) {
    char ts[64];
    get_ts(ts, sizeof(ts));
    printf("[%s] [KOMSÜ] Komsü bay Protection IED trip bildirdi.\n", ts);
  }
}

/* -------------------------------------------------------------------------
 * MMS SERVER KURULUMU — Station Bus (eth1)
 * ---------------------------------------------------------------------- */
static void setup_mms_server(void) {
  IedModel *model = IedModel_create("IED2_PROT");
  LogicalDevice *ldev = LogicalDevice_create("LD1", model);

  /* PTRC1 — Koruma trip + eşikler */
  LogicalNode *ptrc = LogicalNode_create("PTRC1", ldev);

  DataObject *tr = DataObject_create("Tr", (ModelNode *)ptrc, 0);
  da_trip_stVal = DataAttribute_create(
      "stVal", (ModelNode *)tr, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);

  DataObject *thF1 = DataObject_create("ThresF1", (ModelNode *)ptrc, 0);
  da_threshold = DataAttribute_create(
      "setVal", (ModelNode *)thF1, IEC61850_FLOAT32, IEC61850_FC_SP, 0, 0, 0);

  DataObject *bfp = DataObject_create("BFP", (ModelNode *)ptrc, 0);
  da_bfp_active = DataAttribute_create(
      "stVal", (ModelNode *)bfp, IEC61850_BOOLEAN, IEC61850_FC_ST, 0, 0, 0);

  DataObject *fc = DataObject_create("FaultCnt", (ModelNode *)ptrc, 0);
  da_fault_count = DataAttribute_create(
      "stVal", (ModelNode *)fc, IEC61850_INT32, IEC61850_FC_ST, 0, 0, 0);

  /* MMXU1 — Ölçüm değerleri */
  LogicalNode *mmxu = LogicalNode_create("MMXU1", ldev);

  DataObject *amp = DataObject_create("Amp", (ModelNode *)mmxu, 0);
  da_CT1_amp = DataAttribute_create("mag", (ModelNode *)amp, IEC61850_FLOAT32,
                                    IEC61850_FC_MX, 0, 0, 0);

  DataObject *vol = DataObject_create("Vol", (ModelNode *)mmxu, 0);
  da_CT1_vol = DataAttribute_create("mag", (ModelNode *)vol, IEC61850_FLOAT32,
                                    IEC61850_FC_MX, 0, 0, 0);

  iedServer = IedServer_create(model);

  IedServer_lockDataModel(iedServer);
  MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_threshold),
                    OVERCURRENT_THRESHOLD_A);
  MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_trip_stVal), 0);
  MmsValue_setBoolean(IedServer_getAttributeValue(iedServer, da_bfp_active), 0);
  MmsValue_setInt32(IedServer_getAttributeValue(iedServer, da_fault_count), 0);
  MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_CT1_amp), 0.0f);
  MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_CT1_vol), 0.0f);
  IedServer_unlockDataModel(iedServer);

  IedServer_start(iedServer, 102);

  printf("[MMS] Protection IED Server baslatildi (Port:102)\n");
  printf("      IED2_PROT/LD1/PTRC1.Tr.stVal       (Trip durumu)\n");
  printf("      IED2_PROT/LD1/PTRC1.ThresF1.setVal (Esik - yazilabilir)\n");
  printf("      IED2_PROT/LD1/PTRC1.BFP.stVal      (BFP aktif mi?)\n");
  printf("      IED2_PROT/LD1/PTRC1.FaultCnt.stVal (Ariza sayaci)\n");
  printf("      IED2_PROT/LD1/MMXU1.Amp.mag        (CT1 akimi)\n");
  printf("      IED2_PROT/LD1/MMXU1.Vol.mag        (CT1 gerilimi)\n");
}

/* -------------------------------------------------------------------------
 * SİNYAL İŞLEYİCİ
 * ---------------------------------------------------------------------- */
static void sig_handler(int s) {
  (void)s;
  running = 0;
}

/* -------------------------------------------------------------------------
 * ANA PROGRAM
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
  setbuf(stdout, NULL);
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  if (argc > 1)
    iface_pb = argv[1]; /* eth0 — Process Bus */
  if (argc > 2)
    iface_sb = argv[2]; /* eth1 — Station Bus */

  char ts[64];
  get_ts(ts, sizeof(ts));
  printf("\n[%s] === Protection IED_1 (IED2_PROT) Baslatiliyor ===\n", ts);
  printf("  Process Bus : %s (VLAN10)\n", iface_pb);
  printf("  Station Bus : %s (VLAN20)\n", iface_sb);
  printf("  CT1 Esik    : %.1fA\n", OVERCURRENT_THRESHOLD_A);
  printf("  BFP Timeout : %d ms\n\n", BFP_TIMEOUT_MS);

  /* ------------------------------------------------------------------ */
  /* 1. PROCESS BUS GOOSE PUBLISHER'LAR                                  */
  /* ------------------------------------------------------------------ */
  pub_trip =
      make_publisher(iface_pb, APPID_PROT_TRIP, MC_TRIP,
                     "IED2_PROT/LLN0$GO$Trip", "IED2_PROT/LLN0$TripDataSet");
  if (!pub_trip) {
    fprintf(stderr, "HATA: Trip publisher yaratılamadı!\n");
    return 1;
  }
  printf("[GOOSE-PB] Trip publisher hazir (AppID:0x%04X)\n", APPID_PROT_TRIP);

  pub_bfp =
      make_publisher(iface_pb, APPID_BFP_EXT_TRIP, MC_BFP,
                     "IED2_PROT/LLN0$GO$BFPTrip", "IED2_PROT/LLN0$BFPDataSet");
  if (!pub_bfp) {
    fprintf(stderr, "HATA: BFP publisher yaratılamadı!\n");
    return 1;
  }
  printf("[GOOSE-PB] BFP extended trip publisher hazir (AppID:0x%04X)\n",
         APPID_BFP_EXT_TRIP);

  /* ------------------------------------------------------------------ */
  /* 2. STATION BUS GOOSE PUBLISHER                                      */
  /* ------------------------------------------------------------------ */
  pub_status = make_publisher(iface_sb, APPID_PROT_STATUS_SB, MC_STATUS,
                              "IED2_PROT/LLN0$GO$Status",
                              "IED2_PROT/LLN0$StatusDataSet");
  if (!pub_status) {
    fprintf(stderr, "HATA: Status publisher yaratılamadı!\n");
    return 1;
  }
  printf("[GOOSE-SB] Durum/kilitleme publisher hazir (AppID:0x%04X)\n",
         APPID_PROT_STATUS_SB);

  /* ------------------------------------------------------------------ */
  /* 3. PROCESS BUS GOOSE RECEIVER — Breaker IED pozisyon (BFP)         */
  /* ------------------------------------------------------------------ */
  GooseReceiver pb_rx = GooseReceiver_create();
  GooseReceiver_setInterfaceId(pb_rx, iface_pb);

  GooseSubscriber sub_bcu =
      GooseSubscriber_create("IED3_BCU/LLN0$GO$Pos", NULL);
  GooseSubscriber_setAppId(sub_bcu, APPID_BCU_POS);
  GooseSubscriber_setListener(sub_bcu, gooseListener_BreakerPos, NULL);
  GooseReceiver_addSubscriber(pb_rx, sub_bcu);
  GooseReceiver_start(pb_rx);
  printf("[GOOSE-PB] Breaker pos dinleyici hazir (AppID:0x%04X)\n",
         APPID_BCU_POS);

  /* ------------------------------------------------------------------ */
  /* 4. STATION BUS GOOSE RECEIVER — Komşu koruma IED                   */
  /* ------------------------------------------------------------------ */
  GooseReceiver sb_rx = GooseReceiver_create();
  GooseReceiver_setInterfaceId(sb_rx, iface_sb);

  GooseSubscriber sub_nb =
      GooseSubscriber_create("IED_NEIGHBOR/LLN0$GO$Trip", NULL);
  GooseSubscriber_setAppId(sub_nb, APPID_NEIGHBOR_PROT);
  GooseSubscriber_setListener(sub_nb, gooseListener_Neighbor, NULL);
  GooseReceiver_addSubscriber(sb_rx, sub_nb);
  GooseReceiver_start(sb_rx);
  printf("[GOOSE-SB] Komsü prot dinleyici hazir (AppID:0x%04X)\n",
         APPID_NEIGHBOR_PROT);

  /* ------------------------------------------------------------------ */
  /* 5. PROCESS BUS SV RECEIVER — MU_1'den CT1 akımı                    */
  /* ------------------------------------------------------------------ */
  SVReceiver sv_rx = SVReceiver_create();
  SVReceiver_setInterfaceId(sv_rx, iface_pb);
  SVSubscriber sv_sub = SVSubscriber_create(NULL, 0x4000);
  SVSubscriber_setListener(sv_sub, svUpdateHandler, NULL);
  SVReceiver_addSubscriber(sv_rx, sv_sub);
  SVReceiver_start(sv_rx);
  printf("[SV-PB]   MU SV alici hazir (AppID:0x4000) — sadece CT1 isleniyor\n");

  /* ------------------------------------------------------------------ */
  /* 6. MMS SERVER — Station Bus (eth1)                                  */
  /* ------------------------------------------------------------------ */
  setup_mms_server();

  /* ------------------------------------------------------------------ */
  /* HAZIR                                                               */
  /* ------------------------------------------------------------------ */
  get_ts(ts, sizeof(ts));
  printf("\n[%s] === Protection IED_1 AKTIF ===\n", ts);
  printf("  Koruma : CT1 > %.1fA → GOOSE Trip (PB)\n", OVERCURRENT_THRESHOLD_A);
  printf("  BFP    : %d ms icinde kesici acilmazsa genisletilmis trip\n",
         BFP_TIMEOUT_MS);
  printf("  COMTRADE: '%s' dosyasina yaziliyor\n\n", COMTRADE_FILE);

  /* ANA DÖNGÜ */
  uint64_t last_telemetry_sent = 0;
  while (running) {
    Thread_sleep(500);
    mms_packet_count += 2; // Simüle edilen SCADA MMS okuma paketleri

    uint64_t now = Hal_getTimeInMs();
    if (now - last_telemetry_sent >= 1000) {
      send_telemetry_udp();
      last_telemetry_sent = now;
    }

    get_ts(ts, sizeof(ts));
    printf("[%s] >> [%s] CT1: %.1fA | Trip:%s | BFP:%s | Ariza#:%d\n", ts,
           tripActive ? "ALARM" : "NORMAL", CT1_current,
           tripActive ? "AKTIF" : "IDLE",
           (breakerOpened == -1) ? "DEVREDE" : "PASIF", fault_count);
  }

  /* Temizlik */
  SVReceiver_stop(sv_rx);
  SVReceiver_destroy(sv_rx);
  GooseReceiver_stop(pb_rx);
  GooseReceiver_destroy(pb_rx);
  GooseReceiver_stop(sb_rx);
  GooseReceiver_destroy(sb_rx);
  if (pub_trip)
    GoosePublisher_destroy(pub_trip);
  if (pub_bfp)
    GoosePublisher_destroy(pub_bfp);
  if (pub_status)
    GoosePublisher_destroy(pub_status);
  if (iedServer)
    IedServer_stop(iedServer);
  return 0;
}

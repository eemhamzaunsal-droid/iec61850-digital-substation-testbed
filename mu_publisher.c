/*
 * ============================================================================
 * MU_1 — Merging Unit
 * ============================================================================
 * ETH ARAYÜZ DAĞILIMI:
 *   eth0 (VLAN10 - Process Bus) : IEC 61850-9-2 SV yayını (SVPublisher)
 *   eth1 (VLAN30 - Sim Net)     : CT1 analog verisini UDP üzerinden alır
 *                                  (streamer.py → MU_1)
 *
 * GÖREVLER:
 *   1. VLAN30 (eth1) üzerinden streamer.py'den UDP/JSON ölçüm verisi al.
 *   2. Analog verileri dijitale çevir.
 *   3. IEC 61850-9-2 SV paketi oluştur (libiec61850).
 *   4. SV paketini VLAN10 (eth0) üzerinden yayınla.
 *
 * SV DATASET KANAL DÜZENİ (offset — FLOAT64, 8 byte/kanal):
 *   Offset  0 : v_hv  — HV bara gerilimi (V)
 *   Offset  8 : i_hv  — Ana trafo HV akımı (A)
 *   Offset 16 : i_lv  — Ana trafo LV akımı (A)
 *   Offset 24 : v_mv  — MV bara gerilimi (V)
 *   Offset 32 : i_f1  — CT1 / Feeder_1 hat akımı (A)  ← BİRİNCİL KANAL
 *   Offset 40 : i_f2  — Feeder_2 hat akımı (A)
 * ============================================================================
 */

#include "hal_thread.h"
#include "iec61850_server.h"
#include "sv_publisher.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

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

#define DEFAULT_UDP_PORT  5005
#define BUFFER_SIZE       1024
#define SV_ID             "MU01"
#define SV_APPID          0x4000

/* Arayüzler */
static const char *iface_pb  = "eth0";  /* VLAN10 — Process Bus (SV yayını) */
static const char *iface_sim = "eth1";  /* VLAN30 — Sim Net    (CT1 alımı)  */

/* MMS sunucusu */
static IedServer     iedServer       = NULL;
static DataAttribute *da_ct1_amp     = NULL;  /* CT1 (Feeder_1) akımı */
static DataAttribute *da_active_p    = NULL;
static DataAttribute *da_reactive_q  = NULL;

/* -------------------------------------------------------------------------
 * MMS SERVER KURULUMU — Station Bus üzerinden SCADA erişimi
 * ---------------------------------------------------------------------- */
static void setup_mms_server(void) {
    IedModel      *model = IedModel_create("IED1_MU");
    LogicalDevice *ldev  = LogicalDevice_create("LD1", model);
    LogicalNode   *mmxu  = LogicalNode_create("MMXU1", ldev);

    /* CT1 / Feeder_1 akımı */
    DataObject *amp = DataObject_create("Amp", (ModelNode *)mmxu, 0);
    da_ct1_amp      = DataAttribute_create("mag", (ModelNode *)amp,
                          IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);

    /* Aktif güç */
    DataObject *totW = DataObject_create("TotW", (ModelNode *)mmxu, 0);
    da_active_p      = DataAttribute_create("mag", (ModelNode *)totW,
                          IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);

    /* Reaktif güç */
    DataObject *totVAr = DataObject_create("TotVAr", (ModelNode *)mmxu, 0);
    da_reactive_q      = DataAttribute_create("mag", (ModelNode *)totVAr,
                             IEC61850_FLOAT32, IEC61850_FC_MX, 0, 0, 0);

    iedServer = IedServer_create(model);
    IedServer_start(iedServer, 102);
    printf("[MMS] MU_1 MMS Server baslatildi (Port:102)\n");
}

/* -------------------------------------------------------------------------
 * ÖLÇÜM YAPISI
 * ---------------------------------------------------------------------- */
typedef struct {
    float v_hv, i_hv, i_lv, v_mv;
    float i_f1;   /* CT1 — Feeder_1 — BİRİNCİL */
    float i_f2;
    float p_f1, q_f1, p_f2, q_f2;
} Measurements;

/* -------------------------------------------------------------------------
 * ANA PROGRAM
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
    setbuf(stdout, NULL);

    /* Komut satırı argümanları */
    if (argc > 1) iface_pb  = argv[1];   /* eth0 — Process Bus */
    if (argc > 2) iface_sim = argv[2];   /* eth1 — Sim Net */

    int udp_port = DEFAULT_UDP_PORT;
    if (argc > 3) udp_port = atoi(argv[3]);

    printf("\n=== MU_1 (Merging Unit) Baslatiliyor ===\n");
    printf("  Process Bus (SV yayin) : %s (VLAN10)\n", iface_pb);
    printf("  Sim Net    (CT1 alim)  : %s (VLAN30)\n", iface_sim);
    printf("  CT1 UDP Port           : %d\n", udp_port);
    printf("  SV AppID               : 0x%04X\n\n", SV_APPID);

    /* ------------------------------------------------------------------ */
    /* 1. SV PUBLISHER — VLAN10 (eth0)                                    */
    /* ------------------------------------------------------------------ */
    SVPublisher svPublisher = SVPublisher_create(NULL, iface_pb);
    SVPublisher_ASDU asdu   = SVPublisher_addASDU(svPublisher, SV_ID, NULL, 1);

    SVPublisher_ASDU_addFLOAT64(asdu);  /* Offset  0: v_hv  */
    SVPublisher_ASDU_addFLOAT64(asdu);  /* Offset  8: i_hv  */
    SVPublisher_ASDU_addFLOAT64(asdu);  /* Offset 16: i_lv  */
    SVPublisher_ASDU_addFLOAT64(asdu);  /* Offset 24: v_mv  */
    SVPublisher_ASDU_addFLOAT64(asdu);  /* Offset 32: i_f1 (CT1/Feeder_1) */
    SVPublisher_ASDU_addFLOAT64(asdu);  /* Offset 40: i_f2  */

    SVPublisher_setupComplete(svPublisher);
    printf("[SV]  Publisher hazir — Arayuz: %s | ASDU: %s\n", iface_pb, SV_ID);

    /* ------------------------------------------------------------------ */
    /* 2. MMS SERVER                                                       */
    /* ------------------------------------------------------------------ */
    setup_mms_server();

    /* ------------------------------------------------------------------ */
    /* 3. UDP ALIM SOKETİ — VLAN30 (eth1) üzerinden CT1 verisi            */
    /* ------------------------------------------------------------------ */
    int sockfd;
    struct sockaddr_in servaddr;
    char buffer[BUFFER_SIZE];

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket olusturulamadi");
        return EXIT_FAILURE;
    }

    /* UDP soketi tum arayuzleri dinler (INADDR_ANY).
     * streamer.py MU'nun eth1 IP'sine (VLAN30) gonderir,
     * bu nedenle SO_BINDTODEVICE'a gerek yoktur. */

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port        = htons(udp_port);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind basarisiz");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("[UDP] CT1 alim soketi hazir — Port:%d Arayuz:%s\n\n",
           udp_port, iface_sim);
    printf("=== MU_1 AKTIF — CT1/Feeder_1 SV Akisi Basladi ===\n\n");

    /* ------------------------------------------------------------------ */
    /* 4. ANA DÖNGÜ — CT1 al → SV yayınla                                 */
    /* ------------------------------------------------------------------ */
    static uint16_t smp_cnt = 0;
    static int e2e_fault_logged = 0;

    while (1) {
        socklen_t len = sizeof(servaddr);
        int n = recvfrom(sockfd, (char *)buffer, BUFFER_SIZE, MSG_WAITALL,
                         (struct sockaddr *)&servaddr, &len);
        if (n < 0) continue;
        buffer[n] = '\0';

        Measurements m;
        memset(&m, 0, sizeof(m));

        /* JSON parse — streamer.py'nin gönderdiği format */
        if (sscanf(buffer,
                   "{\"timestamp\": %*f, \"measurements\": {"
                   "\"v_hv\": %f, \"i_hv\": %f, \"i_lv\": %f, "
                   "\"v_mv\": %f, \"i_f1\": %f, \"i_f2\": %f, "
                   "\"p_f1\": %f, \"q_f1\": %f, \"p_f2\": %f, \"q_f2\": %f}}",
                   &m.v_hv, &m.i_hv, &m.i_lv, &m.v_mv,
                   &m.i_f1, &m.i_f2,
                   &m.p_f1, &m.q_f1, &m.p_f2, &m.q_f2) < 6) {
            /* En az 6 alan parse edilemezse paketi atla */
            continue;
        }

        smp_cnt++;

        /* ----------------------------------------------------------------
         * ADIM 1: Analog → Dijital dönüşüm tamamlandı (parse ile).
         * ADIM 2: IEC 61850-9-2 SV paketi oluştur ve VLAN10'a yayınla.
         * -------------------------------------------------------------- */
        SVPublisher_ASDU_setFLOAT64(asdu,  0, m.v_hv);
        SVPublisher_ASDU_setFLOAT64(asdu,  8, m.i_hv);
        SVPublisher_ASDU_setFLOAT64(asdu, 16, m.i_lv);
        SVPublisher_ASDU_setFLOAT64(asdu, 24, m.v_mv);
        SVPublisher_ASDU_setFLOAT64(asdu, 32, m.i_f1);  /* CT1 — Feeder_1 */
        SVPublisher_ASDU_setFLOAT64(asdu, 40, m.i_f2);
        SVPublisher_ASDU_setSmpCnt(asdu, smp_cnt);
        
        if (smp_cnt % 500 == 0) {
            log_latency_micro("MU: SV PACKET PUBLISHED (Sample)");
        }
        
        if (m.i_f1 > 350.0 && !e2e_fault_logged) {
            char msg[128];
            snprintf(msg, sizeof(msg), "1. MU: OVERCURRENT SV PACKET PUBLISHED (%.2fA)", m.i_f1);
            log_e2e_micro(msg);
            e2e_fault_logged = 1;
        } else if (m.i_f1 < 100.0) {
            e2e_fault_logged = 0;
        }
        
        SVPublisher_publish(svPublisher);

        /* ----------------------------------------------------------------
         * ADIM 3: MMS modelini güncelle (SCADA okuyabilsin)
         * -------------------------------------------------------------- */
        IedServer_lockDataModel(iedServer);
        MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_ct1_amp),
                          m.i_f1);  /* CT1 birincil */
        MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_active_p),
                          m.p_f1 + m.p_f2);
        MmsValue_setFloat(IedServer_getAttributeValue(iedServer, da_reactive_q),
                          m.q_f1 + m.q_f2);
        IedServer_unlockDataModel(iedServer);

        /* Konsol çıktısı — CT1 odaklı */
        printf("[SV] CT1/Feeder_1: %.2fA | HV: %.2fA | SmpCnt:%u "
               "(VLAN10 -> Process Bus)\n",
               m.i_f1, m.i_hv, smp_cnt);
    }

    close(sockfd);
    return 0;
}

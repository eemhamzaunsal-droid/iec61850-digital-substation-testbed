# --- LIBIEC61850 PATH ---
# Kütüphane genellikle ana klasörünüzde bulunur. PATH'i buna göre güncelliyoruz.
LIB_DIR ?= $(HOME)/libiec61850

# --- COMPILER ---
CC = gcc

# Automatically discover ALL directories containing .h files in the library
IEC_INCLUDES = $(shell find $(LIB_DIR) -name "*.h" -not -path "*/examples/*" -not -path "*/tests/*" -exec dirname {} \; 2>/dev/null | sort -u | sed 's/^/-I/')

CFLAGS = $(IEC_INCLUDES) -Wall -std=c99
LDFLAGS = -L$(LIB_DIR)/build -liec61850 -lpthread -lrt

all: mu_publisher protection_ied breaker_ied control_ied scada_monitor

# Saldiri araclari varsayilan derlemeye DAHIL DEGIL (isletim profilini bozmamak icin).
# Derlemek icin: make attacker
attacker: goose_attacker

mu_publisher: mu_publisher.c
	$(CC) $(CFLAGS) mu_publisher.c -o mu_publisher $(LDFLAGS)

protection_ied: protection_ied.c
	$(CC) $(CFLAGS) protection_ied.c -o protection_ied $(LDFLAGS)

breaker_ied: breaker_ied.c
	$(CC) $(CFLAGS) breaker_ied.c -o breaker_ied $(LDFLAGS)

control_ied: control_ied.c
	$(CC) $(CFLAGS) control_ied.c -o control_ied $(LDFLAGS)

scada_monitor: scada_monitor.c
	$(CC) $(CFLAGS) scada_monitor.c -o scada_monitor $(LDFLAGS)

# --- SALDIRI ARACLARI (yalnizca izole testbed) ---
goose_attacker: goose_attacker.c
	$(CC) $(CFLAGS) goose_attacker.c -o goose_attacker $(LDFLAGS)

clean:
	rm -f mu_publisher protection_ied breaker_ied control_ied scada_monitor goose_attacker

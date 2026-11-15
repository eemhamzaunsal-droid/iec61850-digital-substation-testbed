#!/bin/bash

# --- KONFİGÜRASYON ---
TARGET="scada_monitor"
LIB_PATH="${HOME}/libiec61850"

echo "------------------------------------------------"
echo "📊 CENTRAL SCADA MONITOR ALL-IN-ONE SETUP"
echo "------------------------------------------------"

# 1. Bağımlılık Kontrolü
echo "🔍 Bağımlılıklar kontrol ediliyor..."
sudo apt-get update
sudo apt-get install -y build-essential gcc make

# 2. Üçüncü Parti Kütüphane (libiec61850) Kontrolü (Opsiyonel ama standart olsun)
if [ ! -d "$LIB_PATH" ]; then
    echo "📦 libiec61850 bulunamadı, GitHub'dan indiriliyor..."
    git clone https://github.com/mz-automation/libiec61850.git "$LIB_PATH"
    cd "$LIB_PATH" && make && cd -
else
    echo "✅ libiec61850 zaten mevcut."
fi

# 3. Proje Derleme (Make)
echo "📦 SCADA Monitör derleniyor..."
make $TARGET
if [ $? -ne 0 ]; then
    echo "❌ HATA: Derleme başarısız oldu!"
    exit 1
fi

echo "------------------------------------------------"
echo "✅ SCADA KURULUM VE DERLEME TAMAMLANDI."
echo "------------------------------------------------"
echo "👉 KULLANIM:"
echo "   Sistemi başlatmak için şu komutu verin:"
echo "   ./$TARGET"
echo "------------------------------------------------"

# 4. Otomatik Başlatma
read -p "🚀 SCADA Monitörü şimdi başlatmak ister misiniz? (y/n): " RUN_NOW
if [[ $RUN_NOW == "y" || $RUN_NOW == "Y" ]]; then
    ./$TARGET
fi

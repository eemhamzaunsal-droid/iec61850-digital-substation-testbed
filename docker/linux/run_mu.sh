#!/bin/bash

# --- KONFİGÜRASYON ---
INTERFACE="enp0s3"
TARGET="mu_publisher"
LIB_PATH="${HOME}/libiec61850"
CONFIG_FILE="../network_config.json"

echo "------------------------------------------------"
echo "🚀 MERGING UNIT (MU) ALL-IN-ONE SETUP"
echo "------------------------------------------------"

# 1. Bağımlılık Kontrolü
echo "🔍 Bağımlılıklar kontrol ediliyor..."
sudo apt-get update
sudo apt-get install -y build-essential git gcc make jq

# 2. Yapılandırma Yükleme
if [ -f "$CONFIG_FILE" ]; then
    echo "⚙️ Yapılandırma dosyadan okunuyor..."
    UDP_PORT=$(jq -r '.UDP_PORT' $CONFIG_FILE)
    echo "🔌 UDP Port (Dosyadan): $UDP_PORT"
else
    echo "⚠️ $CONFIG_FILE bulunamadı! Varsayılan ayarlar kullanılacak."
    UDP_PORT=5005
fi

# 2. Üçüncü Parti Kütüphane (libiec61850) Kontrolü
if [ ! -d "$LIB_PATH" ]; then
    echo "📦 libiec61850 bulunamadı, GitHub'dan indiriliyor..."
    git clone https://github.com/mz-automation/libiec61850.git "$LIB_PATH"
    cd "$LIB_PATH"
    echo "⚙️ Kütüphane derleniyor (bu işlem biraz sürebilir)..."
    make
    cd -
else
    echo "✅ libiec61850 zaten mevcut."
fi

# 3. Proje Derleme (Make)
echo "📦 Proje derleniyor..."
make $TARGET
if [ $? -ne 0 ]; then
    echo "❌ HATA: Derleme başarısız oldu!"
    exit 1
fi

echo "------------------------------------------------"
echo "✅ KURULUM VE DERLEME TAMAMLANDI."
echo "------------------------------------------------"
echo "👉 ÖRNEK KULLANIM:"
echo "   Sistemi başlatmak için şu komutu verin:"
echo "   sudo ./$TARGET $INTERFACE"
echo "------------------------------------------------"

# 4. Otomatik Başlatma
read -p "🚀 Uygulamayı şimdi başlatmak ister misiniz? (y/n): " RUN_NOW
if [[ $RUN_NOW == "y" || $RUN_NOW == "Y" ]]; then
    sudo ./$TARGET $INTERFACE $UDP_PORT
fi

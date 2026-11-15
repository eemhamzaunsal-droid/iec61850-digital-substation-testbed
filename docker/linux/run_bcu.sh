#!/bin/bash

# --- KONFİGÜRASYON ---
INTERFACE="enp0s3"
TARGET="breaker_ied"
LIB_PATH="${HOME}/libiec61850"
CONFIG_FILE="../network_config.json"
DEFAULT_WINDOWS_IP="192.168.1.100"

echo "------------------------------------------------"
echo "🛠️ BREAKER CONTROL UNIT (BCU) ALL-IN-ONE SETUP"
echo "------------------------------------------------"

# 1. Bağımlılık Kontrolü
echo "🔍 Bağımlılıklar kontrol ediliyor..."
sudo apt-get update
sudo apt-get install -y build-essential git gcc make jq

# 2. Yapılandırma Yükleme
if [ -f "$CONFIG_FILE" ]; then
    echo "⚙️ Yapılandırma dosyadan okunuyor..."
    WIN_IP=$(jq -r '.WINDOWS_IP' $CONFIG_FILE)
    WIN_PORT=$(jq -r '.CONTROL_PORT' $CONFIG_FILE)
    echo "🖥️ Windows IP: $WIN_IP, Port: $WIN_PORT"
else
    echo "⚠️ $CONFIG_FILE bulunamadı! Manuel giriş gerekecek."
    read -p "🖥️ Windows IP Adresini Girin [$DEFAULT_WINDOWS_IP]: " WIN_IP
    WIN_IP=${WIN_IP:-$DEFAULT_WINDOWS_IP}
    WIN_PORT=5006
fi

# 3. Üçüncü Parti Kütüphane (libiec61850) Kontrolü
if [ ! -d "$LIB_PATH" ]; then
    echo "📦 libiec61850 bulunamadı, GitHub'dan indiriliyor..."
    git clone https://github.com/mz-automation/libiec61850.git "$LIB_PATH"
    cd "$LIB_PATH" && make && cd -
else
    echo "✅ libiec61850 zaten mevcut."
fi

# 4. Proje Derleme (Make)
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
echo "   sudo ./$TARGET $INTERFACE $WIN_IP $WIN_PORT"
echo "------------------------------------------------"

# 5. Otomatik Başlatma
read -p "🚀 Uygulamayı şimdi başlatmak ister misiniz? (y/n): " RUN_NOW
if [[ $RUN_NOW == "y" || $RUN_NOW == "Y" ]]; then
    sudo ./$TARGET $INTERFACE $WIN_IP $WIN_PORT
fi

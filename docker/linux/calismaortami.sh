#!/bin/bash

echo -e "\e[36m================================================\e[0m"
echo -e "\e[36m=== LINUX ÇALIŞMA ORTAMI HAZIRLAMA SCRIPTI ===\e[0m"
echo -e "\e[36m================================================\e[0m"

echo -e "\n\e[33m[1/8] Paket listeleri güncelleniyor...\e[0m"
sudo apt-get update

echo -e "\n\e[33m[2/8] Gerekli araçlar (jq, curl, git) kuruluyor...\e[0m"
sudo apt-get install -y jq curl git

echo -e "\n\e[33m[3/8] Docker kontrol ediliyor...\e[0m"
if ! command -v docker &> /dev/null; then
    echo -e "\e[31mDocker bulunamadı. Resmi script ile kuruluyor...\e[0m"
    curl -fsSL https://get.docker.com -o get-docker.sh
    sudo sh get-docker.sh
    rm get-docker.sh
    
    # Kullanıcıyı docker grubuna ekle
    sudo usermod -aG docker $USER
    echo -e "\e[32mDocker başarıyla kuruldu.\e[0m"
else
    echo -e "\e[32mDocker zaten kurulu.\e[0m"
fi

echo -e "\n\e[33m[4/8] Docker Servisi başlatılıyor ve kontrol ediliyor...\e[0m"
sudo systemctl enable docker
sudo systemctl start docker
if systemctl is-active --quiet docker; then
    echo -e "\e[32mDocker servisi aktif ve sorunsuz çalışıyor.\e[0m"
else
    echo -e "\e[31mUYARI: Docker servisi başlatılamadı! Lütfen manuel kontrol edin.\e[0m"
fi

echo -e "\n\e[33m[5/8] Docker Compose kontrol ediliyor...\e[0m"
if ! command -v docker-compose &> /dev/null && ! docker compose version &> /dev/null; then
    echo -e "\e[31mDocker Compose bulunamadı. Kuruluyor...\e[0m"
    sudo apt-get install -y docker-compose
else
    echo -e "\e[32mDocker Compose zaten kurulu.\e[0m"
fi

echo -e "\n\e[33m[6/8] libiec61850 kütüphanesi kontrol ediliyor...\e[0m"
# Build context: bu scriptin 2 üst dizini (linux_files/)
BUILD_CTX="$(cd ../.. && pwd)"
LIB_DEST="$BUILD_CTX/libiec61850"

if [ -d "$LIB_DEST" ]; then
    echo -e "\e[32mlibiec61850 zaten mevcut: $LIB_DEST\e[0m"
else
    echo -e "\e[33mlibiec61850 bulunamadı. Sistemde aranıyor...\e[0m"
    # Sistemde var mı diye ara
    FOUND=$(find /home /root /opt /tmp -maxdepth 4 -name "libiec61850" -type d 2>/dev/null | head -1)
    if [ -n "$FOUND" ]; then
        echo -e "\e[32mBulundu: $FOUND — Build context'e kopyalanıyor...\e[0m"
        cp -r "$FOUND" "$LIB_DEST"
        echo -e "\e[32mKopyalama tamamlandı.\e[0m"
    else
        echo -e "\e[33mSistemde bulunamadı. GitHub'dan indiriliyor...\e[0m"
        git clone https://github.com/mz-automation/libiec61850.git "$LIB_DEST"
        if [ $? -eq 0 ]; then
            echo -e "\e[32mlibiec61850 başarıyla indirildi.\e[0m"
        else
            echo -e "\e[31mHATA: libiec61850 indirilemedi! İnternet bağlantısını kontrol edin.\e[0m"
            exit 1
        fi
    fi
fi

echo -e "\n\e[33m[7/8] Ağ çakışmalarını temizleme (Macvlan hazırlığı)...\e[0m"
echo -e "Kullanılmayan eski Docker ağları temizleniyor..."
sudo docker network prune -f
echo -e "\e[32mAğ temizliği tamamlandı.\e[0m"

echo -e "\n\e[33m[8/8] NIC Promiscuous Mode aktifleştiriliyor ve VLAN Arayüzleri kuruluyor...\e[0m"
NIC=$(ip route | grep default | awk '{print $5}' | head -1)
if [ -n "$NIC" ]; then
    # Create VLAN 10 interface if it doesn't exist
    if ! ip link show "$NIC.10" &>/dev/null; then
        sudo ip link add link "$NIC" name "$NIC.10" type vlan id 10
        sudo ip link set dev "$NIC.10" up
        echo -e "\e[32m$NIC.10 VLAN 10 arayüzü oluşturuldu.\e[0m"
    fi
    # Create VLAN 20 interface if it doesn't exist
    if ! ip link show "$NIC.20" &>/dev/null; then
        sudo ip link add link "$NIC" name "$NIC.20" type vlan id 20
        sudo ip link set dev "$NIC.20" up
        echo -e "\e[32m$NIC.20 VLAN 20 arayüzü oluşturuldu.\e[0m"
    fi
    # Create VLAN 30 interface if it doesn't exist
    if ! ip link show "$NIC.30" &>/dev/null; then
        sudo ip link add link "$NIC" name "$NIC.30" type vlan id 30
        sudo ip link set dev "$NIC.30" up
        echo -e "\e[32m$NIC.30 VLAN 30 arayüzü oluşturuldu.\e[0m"
    fi
    sudo ip addr add 10.10.10.1/24 dev "$NIC.10" 2>/dev/null || true
    sudo ip addr add 10.10.20.1/24 dev "$NIC.20" 2>/dev/null || true
    sudo ip addr add 10.10.30.1/24 dev "$NIC.30" 2>/dev/null || true
    
    sudo ip link set "$NIC" promisc on
    sudo ip link set "$NIC.10" promisc on
    sudo ip link set "$NIC.20" promisc on
    sudo ip link set "$NIC.30" promisc on
    echo -e "\e[32m$NIC, $NIC.10, $NIC.20 ve $NIC.30 promiscuous moda alındı.\e[0m"
else
    echo -e "\e[31mUYARI: Aktif NIC bulunamadı, promiscuous mode ve VLAN arayüzleri manuel ayarlanmalı!\e[0m"
fi

echo -e "\n\e[33m[*] Gerekli izinler ayarlanıyor...\e[0m"
chmod +x *.sh

echo -e "\n\e[32m========================================================================\e[0m"
echo -e "\e[32m=== ÇALIŞMA ORTAMI BAŞARIYLA HAZIRLANDI! ===\e[0m"
echo -e "\e[32m========================================================================\e[0m"
echo -e "\e[31mÖNEMLİ HATIRLATMA (MACVLAN AĞI İÇİN):\e[0m"
echo -e "Konteynerlerin yerel ağda (192.168.3.x) kendi IP'lerini alıp diğer"
echo -e "cihazlar (telefon/PC) tarafından görülebilmesi için VirtualBox ayarlarınızda:"
echo -e "1. Ağ bağdaştırıcısı \e[1m'Köprü Bağdaştırıcısı' (Bridged Adapter)\e[0m olmalıdır."
echo -e "2. \e[1m'Karmaşık Kip' (Promiscuous Mode)\e[0m seçeneği \e[1m'Tümüne İzin Ver' (Allow All)\e[0m olmalıdır."
echo -e "\nEğer bu ayarlar tamsa, sistemi başlatmak için şu komutu verebilirsiniz:"
echo -e "\e[36msudo ./start_docker.sh\e[0m\n"

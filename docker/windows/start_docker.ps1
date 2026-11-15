# start_docker.ps1
Set-Location -Path "..\.." # linux_files dizinine dön
$configPath = "..\network_config.json"

if (-not (Test-Path $configPath)) {
    Write-Host "Hata: network_config.json bulunamadi!" -ForegroundColor Red
    exit 1
}

Write-Host "network_config.json okunuyor..." -ForegroundColor Cyan
$config = Get-Content $configPath | ConvertFrom-Json

$envFile = ".env"
$envContent = @(
    "WINDOWS_IP=$($config.WINDOWS_IP)"
    "LINUX_1_IP=$($config.LINUX_1_IP)"
    "LINUX_2_IP=$($config.LINUX_2_IP)"
    "LINUX_3_IP=$($config.LINUX_3_IP)"
    "LINUX_SCADA_IP=$($config.LINUX_SCADA_IP)"
    "UDP_PORT=$($config.UDP_PORT)"
    "CONTROL_PORT=$($config.CONTROL_PORT)"
)

$envContent | Set-Content $envFile
Write-Host ".env dosyasi olusturuldu." -ForegroundColor Green
Write-Host "Docker container'lari baslatiliyor..." -ForegroundColor Cyan
docker-compose up -d --build
Write-Host "Islem tamamlandi! Container'lari gormek icin 'docker ps' yazabilirsiniz." -ForegroundColor Green

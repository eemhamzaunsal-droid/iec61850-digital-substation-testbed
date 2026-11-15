# start_mu.ps1
Set-Location -Path "..\.."
$configPath = "..\network_config.json"
if (Test-Path $configPath) {
    $config = Get-Content $configPath | ConvertFrom-Json
    $envContent = @("WINDOWS_IP=$($config.WINDOWS_IP)", "LINUX_1_IP=$($config.LINUX_1_IP)", "LINUX_2_IP=$($config.LINUX_2_IP)", "LINUX_3_IP=$($config.LINUX_3_IP)", "LINUX_SCADA_IP=$($config.LINUX_SCADA_IP)", "UDP_PORT=$($config.UDP_PORT)", "CONTROL_PORT=$($config.CONTROL_PORT)")
    $envContent | Set-Content .env
}
Write-Host "Merging Unit (mu_publisher) baslatiliyor..." -ForegroundColor Cyan
docker-compose up -d --build mu_publisher
Write-Host "Baslatildi!" -ForegroundColor Green

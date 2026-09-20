# PowerShell скрипт для диагностики и исправления прав доступа Yandex Cloud Vision OCR

Write-Host "=== Диагностика Yandex Cloud Vision OCR ===" -ForegroundColor Cyan
Write-Host ""

# Проверка 1: Yandex CLI установлен
Write-Host "1. Проверка Yandex Cloud CLI..." -ForegroundColor Yellow
try {
    $null = yc config list 2>&1
    Write-Host "✓ Yandex Cloud CLI установлен" -ForegroundColor Green
} catch {
    Write-Host "❌ Yandex Cloud CLI не установлен" -ForegroundColor Red
    Write-Host "   Установите: https://yandex.cloud/ru/docs/cli/quickstart"
    exit 1
}
Write-Host ""

# Получаем конфигурацию
Write-Host "2. Получение конфигурации..." -ForegroundColor Yellow
$folderId = yc config get folder-id
$cloudId = yc config get cloud-id

Write-Host "   Cloud ID: $cloudId"
Write-Host "   Folder ID: $folderId"
Write-Host ""

# Проверка Folder ID
if ([string]::IsNullOrWhiteSpace($folderId)) {
    Write-Host "❌ Folder ID не настроен" -ForegroundColor Red
    Write-Host ""
    Write-Host "Доступные каталоги:" -ForegroundColor Yellow
    yc resource-manager folder list
    Write-Host ""
    Write-Host "Выполните:" -ForegroundColor Cyan
    Write-Host "   yc config set folder-id <ID_каталога_из_списка_выше>"
    exit 1
}
Write-Host "✓ Folder ID настроен" -ForegroundColor Green
Write-Host ""

# Проверка прав
Write-Host "3. Проверка прав доступа..." -ForegroundColor Yellow
$bindings = yc resource-manager folder list-access-bindings --id $folderId 2>&1 | Out-String

if ($bindings -match "permission denied|Permission denied") {
    Write-Host "❌ Нет прав на просмотр привязок доступа" -ForegroundColor Red
    Write-Host "   Обратитесь к администратору" -ForegroundColor Yellow
    exit 1
}

Write-Host $bindings
Write-Host ""

# Проверка роли ai.vision.user
Write-Host "4. Проверка роли ai.vision.user..." -ForegroundColor Yellow
if ($bindings -match "ai.vision.user") {
    Write-Host "✓ Роль ai.vision.user найдена" -ForegroundColor Green
} else {
    Write-Host "⚠ Роль ai.vision.user НЕ найдена" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "НЕОБХОДИМО ДОБАВИТЬ РОЛЬ ВРУЧНУЮ:" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Вариант 1 - Через веб-консоль (РЕКОМЕНДУЕТСЯ):" -ForegroundColor White
    Write-Host "1. Откройте: https://console.yandex.cloud/folders/$folderId/access-bindings"
    Write-Host "2. Нажмите 'Назначить привязки' (Assign bindings)"
    Write-Host "3. Выберите ваш аккаунт"
    Write-Host "4. Добавьте роль: ai.vision.user"
    Write-Host "5. Нажмите 'Сохранить'"
    Write-Host ""
    Write-Host "Вариант 2 - Через CLI:" -ForegroundColor White
    Write-Host "Получите ваш User ID:"
    yc iam user-account list
    Write-Host ""
    Write-Host "Затем выполните (замените YOUR_USER_ID):"
    Write-Host "yc resource-manager folder add-access-binding $folderId --role ai.vision.user --subject userAccount:YOUR_USER_ID"
    Write-Host ""
    Write-Host -NoNewLine "Нажмите Enter после добавления роли..." -ForegroundColor Cyan
    $null = Read-Host
}
Write-Host ""

# Проверка billing
Write-Host "5. Проверка платежного аккаунта..." -ForegroundColor Yellow
try {
    $billing = yc billing account list --format json 2>&1 | ConvertFrom-Json
    if ($billing.status -eq "ACTIVE" -or $billing.status -eq "TRIAL_ACTIVE") {
        Write-Host "✓ Billing account активен ($($billing.status))" -ForegroundColor Green
    } else {
        Write-Host "⚠ Billing account: $($billing.status)" -ForegroundColor Yellow
        Write-Host "   Проверьте: https://console.yandex.cloud/billing"
    }
} catch {
    Write-Host "⚠ Не удалось проверить billing" -ForegroundColor Yellow
    Write-Host "   Проверьте вручную: https://console.yandex.cloud/billing"
}
Write-Host ""

# Создание IAM токена
Write-Host "6. Создание нового IAM токена..." -ForegroundColor Yellow
$iamToken = yc iam create-token

if ([string]::IsNullOrWhiteSpace($iamToken)) {
    Write-Host "❌ Не удалось создать IAM токен" -ForegroundColor Red
    exit 1
}

Write-Host "✓ IAM токен создан (действителен 12 часов)" -ForegroundColor Green
Write-Host ""

# Тест API
Write-Host "7. Тестирование Vision OCR API..." -ForegroundColor Yellow
Write-Host "   Отправка тестового запроса..." -ForegroundColor Gray

$testImage = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
$body = @{
    mimeType = "PNG"
    languageCodes = @("en")
    model = "page"
    content = $testImage
} | ConvertTo-Json

$headers = @{
    "Content-Type" = "application/json"
    "Authorization" = "Bearer $iamToken"
    "x-folder-id" = $folderId
}

try {
    $response = Invoke-WebRequest -Uri "https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText" `
        -Method POST `
        -Headers $headers `
        -Body $body `
        -UseBasicParsing
    
    Write-Host ""
    Write-Host "✅ SUCCESS! Vision OCR API работает!" -ForegroundColor Green
    Write-Host "   HTTP Status: $($response.StatusCode)" -ForegroundColor Green
    
} catch {
    $statusCode = $_.Exception.Response.StatusCode.Value__
    Write-Host ""
    Write-Host "❌ HTTP $statusCode - $($_.Exception.Response.StatusDescription)" -ForegroundColor Red
    
    $errorBody = $_.ErrorDetails.Message
    if ($errorBody) {
        Write-Host ""
        Write-Host "Ответ сервера:" -ForegroundColor Yellow
        Write-Host ($errorBody | ConvertFrom-Json | ConvertTo-Json -Depth 10)
    }
    
    if ($statusCode -eq 403) {
        Write-Host ""
        Write-Host "=== ПРОБЛЕМА: НЕТ ПРАВ ДОСТУПА ===" -ForegroundColor Red
        Write-Host ""
        Write-Host "Роль ai.vision.user добавлена, но еще не активна." -ForegroundColor Yellow
        Write-Host ""
        Write-Host "Решения:" -ForegroundColor Cyan
        Write-Host "1. Подождите 1-2 минуты и запустите скрипт снова"
        Write-Host "2. Добавьте роль через веб-консоль:"
        Write-Host "   https://console.yandex.cloud/folders/$folderId/access-bindings"
        Write-Host "3. Проверьте что billing account активен:"
        Write-Host "   https://console.yandex.cloud/billing"
        Write-Host ""
        exit 1
    }
    
    exit 1
}

Write-Host ""
Write-Host "=== ✅ ВСЁ РАБОТАЕТ! ===" -ForegroundColor Green
Write-Host ""
Write-Host "Скопируйте эти данные в chiaki-ng:" -ForegroundColor Cyan
Write-Host ""
Write-Host "IAM Token:" -ForegroundColor White
Write-Host $iamToken -ForegroundColor Gray
Write-Host ""
Write-Host "Folder ID:" -ForegroundColor White
Write-Host $folderId -ForegroundColor Gray
Write-Host ""
Write-Host "Инструкция:" -ForegroundColor Yellow
Write-Host "1. Откройте chiaki-ng"
Write-Host "2. Настройки → Конфигурация"
Write-Host "3. Вставьте IAM Token и Folder ID"
Write-Host "4. Сохраните и перезапустите chiaki-ng"
Write-Host "5. Нажмите Alt+T во время игры"
Write-Host ""
Write-Host "=== Готово! ===" -ForegroundColor Green


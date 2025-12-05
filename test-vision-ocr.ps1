# PowerShell скрипт для тестирования Yandex Cloud Vision OCR API
# Поможет понять почему возникает ошибка 403

Write-Host "=== Тест Yandex Cloud Vision OCR API ===" -ForegroundColor Cyan
Write-Host ""

# Шаг 1: Получаем IAM токен
Write-Host "1. Получение свежего IAM токена..." -ForegroundColor Yellow
try {
    $iamToken = yc iam create-token 2>&1 | Out-String
    $iamToken = $iamToken.Trim()
    
    if ([string]::IsNullOrWhiteSpace($iamToken) -or $iamToken -match "ERROR|error") {
        Write-Host "❌ Не удалось получить IAM токен" -ForegroundColor Red
        Write-Host $iamToken
        Write-Host ""
        Write-Host "Выполните: yc init" -ForegroundColor Yellow
        exit 1
    }
    
    Write-Host "✓ IAM токен получен" -ForegroundColor Green
    Write-Host "  Длина: $($iamToken.Length) символов" -ForegroundColor Gray
    Write-Host "  Начало: $($iamToken.Substring(0, [Math]::Min(30, $iamToken.Length)))..." -ForegroundColor Gray
    Write-Host "  Конец: ...$($iamToken.Substring([Math]::Max(0, $iamToken.Length - 30)))" -ForegroundColor Gray
} catch {
    Write-Host "❌ Ошибка при получении токена: $_" -ForegroundColor Red
    exit 1
}
Write-Host ""

# Шаг 2: Получаем Folder ID
Write-Host "2. Получение Folder ID..." -ForegroundColor Yellow
$folderId = yc config get folder-id 2>&1 | Out-String
$folderId = $folderId.Trim()

if ([string]::IsNullOrWhiteSpace($folderId)) {
    Write-Host "❌ Folder ID не настроен" -ForegroundColor Red
    Write-Host ""
    Write-Host "Доступные каталоги:" -ForegroundColor Yellow
    yc resource-manager folder list
    Write-Host ""
    Write-Host "Выполните: yc config set folder-id <ID>" -ForegroundColor Cyan
    exit 1
}

Write-Host "✓ Folder ID: $folderId" -ForegroundColor Green
Write-Host ""

# Шаг 3: Тестовое изображение (1x1 прозрачный пиксель в PNG)
Write-Host "3. Подготовка тестового изображения..." -ForegroundColor Yellow
$testImage = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
Write-Host "✓ Тестовое изображение готово (1x1 px PNG)" -ForegroundColor Green
Write-Host ""

# Шаг 4: Формируем запрос
Write-Host "4. Формирование запроса к Vision OCR API..." -ForegroundColor Yellow

$requestBody = @{
    mimeType = "PNG"
    languageCodes = @("en")
    model = "page"
    content = $testImage
} | ConvertTo-Json -Compress

$headers = @{
    "Content-Type" = "application/json"
    "Authorization" = "Bearer $iamToken"
    "x-folder-id" = $folderId
    "x-data-logging-enabled" = "true"
}

Write-Host "✓ Запрос сформирован" -ForegroundColor Green
Write-Host ""

# Показываем детали запроса
Write-Host "=== ДЕТАЛИ ЗАПРОСА ===" -ForegroundColor Cyan
Write-Host "URL: https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText"
Write-Host "Method: POST"
Write-Host ""
Write-Host "Headers:" -ForegroundColor White
Write-Host "  Content-Type: application/json"
Write-Host "  Authorization: Bearer $($iamToken.Substring(0, 30))...$($iamToken.Substring($iamToken.Length - 30))"
Write-Host "  x-folder-id: $folderId"
Write-Host "  x-data-logging-enabled: true"
Write-Host ""
Write-Host "Body:" -ForegroundColor White
Write-Host $requestBody
Write-Host ""
Write-Host "=== КОНЕЦ ДЕТАЛЕЙ ===" -ForegroundColor Cyan
Write-Host ""

# Шаг 5: Отправляем запрос
Write-Host "5. Отправка запроса к Yandex Cloud Vision OCR..." -ForegroundColor Yellow
Write-Host "   Ожидайте ответа..." -ForegroundColor Gray

try {
    $response = Invoke-WebRequest `
        -Uri "https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText" `
        -Method POST `
        -Headers $headers `
        -Body $requestBody `
        -ContentType "application/json" `
        -UseBasicParsing
    
    Write-Host ""
    Write-Host "✅ ✅ ✅ SUCCESS! ✅ ✅ ✅" -ForegroundColor Green
    Write-Host ""
    Write-Host "HTTP Status: $($response.StatusCode)" -ForegroundColor Green
    Write-Host ""
    Write-Host "Response:" -ForegroundColor White
    $responseJson = $response.Content | ConvertFrom-Json
    Write-Host ($responseJson | ConvertTo-Json -Depth 10)
    Write-Host ""
    Write-Host "=== ✓ Vision OCR API РАБОТАЕТ! ===" -ForegroundColor Green
    Write-Host ""
    Write-Host "Используйте эти credentials в chiaki-ng:" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "IAM Token:" -ForegroundColor White
    Write-Host $iamToken -ForegroundColor Gray
    Write-Host ""
    Write-Host "Folder ID:" -ForegroundColor White
    Write-Host $folderId -ForegroundColor Gray
    Write-Host ""
    Write-Host "⚠️ ВАЖНО: Скопируйте IAM токен сейчас!" -ForegroundColor Yellow
    Write-Host "   Он действует только 12 часов с момента создания" -ForegroundColor Gray
    Write-Host ""
    
} catch {
    $statusCode = $_.Exception.Response.StatusCode.value__
    $statusDescription = $_.Exception.Response.StatusDescription
    
    Write-Host ""
    Write-Host "❌ ❌ ❌ ОШИБКА ❌ ❌ ❌" -ForegroundColor Red
    Write-Host ""
    Write-Host "HTTP Status: $statusCode - $statusDescription" -ForegroundColor Red
    Write-Host ""
    
    # Парсим тело ошибки
    $errorBody = $_.ErrorDetails.Message
    if ($errorBody) {
        Write-Host "Ответ сервера:" -ForegroundColor Yellow
        try {
            $errorJson = $errorBody | ConvertFrom-Json
            Write-Host ($errorJson | ConvertTo-Json -Depth 10) -ForegroundColor Red
            
            # Анализ ошибки
            Write-Host ""
            Write-Host "=== ДИАГНОСТИКА ===" -ForegroundColor Cyan
            
            if ($statusCode -eq 403) {
                Write-Host ""
                Write-Host "Причина: НЕТ ПРАВ ДОСТУПА" -ForegroundColor Red
                Write-Host ""
                Write-Host "Проблема в сообщении:" -ForegroundColor Yellow
                Write-Host $errorJson.error.message -ForegroundColor Red
                Write-Host ""
                Write-Host "Решения:" -ForegroundColor Cyan
                Write-Host ""
                Write-Host "1. Добавьте роль через веб-консоль:" -ForegroundColor White
                Write-Host "   https://console.yandex.cloud/folders/$folderId/access-bindings" -ForegroundColor Gray
                Write-Host "   Добавьте роль: ai.vision.user"
                Write-Host ""
                Write-Host "2. Или выполните в PowerShell:" -ForegroundColor White
                Write-Host "   # Получите ваш User ID" -ForegroundColor Gray
                Write-Host "   yc iam user-account list"
                Write-Host ""
                Write-Host "   # Добавьте роль (замените YOUR_USER_ID)" -ForegroundColor Gray
                Write-Host "   yc resource-manager folder add-access-binding $folderId ``" -ForegroundColor Gray
                Write-Host "     --role ai.vision.user ``" -ForegroundColor Gray
                Write-Host "     --subject userAccount:YOUR_USER_ID" -ForegroundColor Gray
                Write-Host ""
                Write-Host "3. Проверьте billing account:" -ForegroundColor White
                Write-Host "   https://console.yandex.cloud/billing" -ForegroundColor Gray
                Write-Host "   Статус должен быть: ACTIVE или TRIAL_ACTIVE"
                Write-Host ""
                Write-Host "4. Подождите 2-3 минуты после добавления роли" -ForegroundColor White
                Write-Host "   Права активируются не мгновенно!"
                Write-Host ""
                
            } elseif ($statusCode -eq 401) {
                Write-Host "Причина: НЕВЕРНЫЙ ИЛИ ИСТЕКШИЙ ТОКЕН" -ForegroundColor Red
                Write-Host "Токен был только что создан, возможно проблема в yc auth"
                Write-Host ""
                Write-Host "Решение:" -ForegroundColor Cyan
                Write-Host "   yc init"
                
            } elseif ($statusCode -eq 400) {
                Write-Host "Причина: НЕВЕРНЫЙ ФОРМАТ ЗАПРОСА" -ForegroundColor Red
                Write-Host "Проверьте что тестовое изображение валидно"
                
            } else {
                Write-Host "Неожиданная ошибка $statusCode" -ForegroundColor Red
            }
            
        } catch {
            Write-Host $errorBody -ForegroundColor Red
        }
    } else {
        Write-Host $_.Exception.Message -ForegroundColor Red
    }
    
    Write-Host ""
    Write-Host "=== Дополнительная диагностика ===" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Проверьте права на folder:" -ForegroundColor Yellow
    Write-Host "yc resource-manager folder list-access-bindings --id $folderId" -ForegroundColor Gray
    Write-Host ""
    Write-Host "Проверьте ваши роли:" -ForegroundColor Yellow  
    Write-Host "yc iam user-account list" -ForegroundColor Gray
    Write-Host ""
    
    exit 1
}

Write-Host ""
Write-Host "=== Тест завершен ===" -ForegroundColor Cyan


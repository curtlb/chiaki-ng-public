$iamToken = yc iam create-token
$folderId = "b1gnoptgctl7f8pqmsp8"

# Тест Translate API
$body = @{
    targetLanguageCode = "ru"
    texts = @("Hello")
    folderId = $folderId
} | ConvertTo-Json

try {
    $result = Invoke-WebRequest `
        -Uri "https://translate.api.cloud.yandex.net/translate/v2/translate" `
        -Method POST `
        -Headers @{
            "Content-Type" = "application/json"
            "Authorization" = "Bearer $iamToken"
        } `
        -Body $body `
        -UseBasicParsing
    
    Write-Host "SUCCESS! Translate API works!" -ForegroundColor Green
    Write-Host $result.Content
}
catch {
    Write-Host "ERROR: $($_.Exception.Response.StatusCode.value__)" -ForegroundColor Red
    Write-Host $_.ErrorDetails.Message
}
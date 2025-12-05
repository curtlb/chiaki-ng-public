# PowerShell script for testing Yandex Cloud Vision OCR API
# Helps to understand why 403 error occurs

Write-Host "=== Test Yandex Cloud Vision OCR API ===" -ForegroundColor Cyan
Write-Host ""

# Step 1: Get IAM token
Write-Host "1. Getting fresh IAM token..." -ForegroundColor Yellow
try {
    $iamToken = yc iam create-token 2>&1 | Out-String
    $iamToken = $iamToken.Trim()
    
    if ([string]::IsNullOrWhiteSpace($iamToken) -or $iamToken -match "ERROR|error") {
        Write-Host "ERROR: Failed to get IAM token" -ForegroundColor Red
        Write-Host $iamToken
        Write-Host ""
        Write-Host "Run: yc init" -ForegroundColor Yellow
        exit 1
    }
    
    Write-Host "SUCCESS: IAM token obtained" -ForegroundColor Green
    Write-Host "  Length: $($iamToken.Length) chars" -ForegroundColor Gray
    Write-Host "  Start: $($iamToken.Substring(0, [Math]::Min(30, $iamToken.Length)))..." -ForegroundColor Gray
    Write-Host "  End: ...$($iamToken.Substring([Math]::Max(0, $iamToken.Length - 30)))" -ForegroundColor Gray
}
catch {
    Write-Host "ERROR getting token: $_" -ForegroundColor Red
    exit 1
}
Write-Host ""

# Step 2: Get Folder ID
Write-Host "2. Getting Folder ID..." -ForegroundColor Yellow
$folderId = yc config get folder-id 2>&1 | Out-String
$folderId = $folderId.Trim()

if ([string]::IsNullOrWhiteSpace($folderId)) {
    Write-Host "ERROR: Folder ID not configured" -ForegroundColor Red
    Write-Host ""
    Write-Host "Available folders:" -ForegroundColor Yellow
    yc resource-manager folder list
    Write-Host ""
    Write-Host "Run: yc config set folder-id <ID>" -ForegroundColor Cyan
    exit 1
}

Write-Host "SUCCESS: Folder ID: $folderId" -ForegroundColor Green
Write-Host ""

# Step 3: Test image
Write-Host "3. Preparing test image..." -ForegroundColor Yellow
$testImage = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
Write-Host "SUCCESS: Test image ready (1x1 px PNG)" -ForegroundColor Green
Write-Host ""

# Step 4: Prepare request
Write-Host "4. Preparing request..." -ForegroundColor Yellow

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

Write-Host "SUCCESS: Request prepared" -ForegroundColor Green
Write-Host ""

# Show request details
Write-Host "=== REQUEST DETAILS ===" -ForegroundColor Cyan
Write-Host "URL: https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText"
Write-Host "Method: POST"
Write-Host ""
Write-Host "Headers:" -ForegroundColor White
Write-Host "  Content-Type: application/json"
Write-Host "  Authorization: Bearer $($iamToken.Substring(0, 30))...$($iamToken.Substring($iamToken.Length - 30))"
Write-Host "  x-folder-id: $folderId"
Write-Host ""
Write-Host "Body:" -ForegroundColor White
Write-Host $requestBody
Write-Host ""
Write-Host "=== END REQUEST ===" -ForegroundColor Cyan
Write-Host ""

# Step 5: Send request
Write-Host "5. Sending request to Yandex Cloud..." -ForegroundColor Yellow
Write-Host "   Waiting for response..." -ForegroundColor Gray

try {
    $response = Invoke-WebRequest `
        -Uri "https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText" `
        -Method POST `
        -Headers $headers `
        -Body $requestBody `
        -ContentType "application/json" `
        -UseBasicParsing
    
    Write-Host ""
    Write-Host "====== SUCCESS! ======" -ForegroundColor Green
    Write-Host ""
    Write-Host "HTTP Status: $($response.StatusCode)" -ForegroundColor Green
    Write-Host ""
    Write-Host "Response:" -ForegroundColor White
    $responseJson = $response.Content | ConvertFrom-Json
    Write-Host ($responseJson | ConvertTo-Json -Depth 10)
    Write-Host ""
    Write-Host "=== Vision OCR API WORKS! ===" -ForegroundColor Green
    Write-Host ""
    Write-Host "Use these credentials in chiaki-ng:" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "IAM Token:" -ForegroundColor White
    Write-Host $iamToken -ForegroundColor Gray
    Write-Host ""
    Write-Host "Folder ID:" -ForegroundColor White
    Write-Host $folderId -ForegroundColor Gray
    Write-Host ""
    Write-Host "IMPORTANT: Copy IAM token now!" -ForegroundColor Yellow
    Write-Host "It expires in 12 hours" -ForegroundColor Gray
    Write-Host ""
}
catch {
    $statusCode = $_.Exception.Response.StatusCode.value__
    $statusDescription = $_.Exception.Response.StatusDescription
    
    Write-Host ""
    Write-Host "====== ERROR ======" -ForegroundColor Red
    Write-Host ""
    Write-Host "HTTP Status: $statusCode - $statusDescription" -ForegroundColor Red
    Write-Host ""
    
    $errorBody = $_.ErrorDetails.Message
    if ($errorBody) {
        Write-Host "Server response:" -ForegroundColor Yellow
        try {
            $errorJson = $errorBody | ConvertFrom-Json
            Write-Host ($errorJson | ConvertTo-Json -Depth 10) -ForegroundColor Red
            
            Write-Host ""
            Write-Host "=== DIAGNOSTICS ===" -ForegroundColor Cyan
            
            if ($statusCode -eq 403) {
                Write-Host ""
                Write-Host "CAUSE: NO ACCESS PERMISSIONS" -ForegroundColor Red
                Write-Host ""
                Write-Host "Error message:" -ForegroundColor Yellow
                Write-Host $errorJson.error.message -ForegroundColor Red
                Write-Host ""
                Write-Host "SOLUTIONS:" -ForegroundColor Cyan
                Write-Host ""
                Write-Host "1. Add role via web console:" -ForegroundColor White
                Write-Host "   https://console.yandex.cloud/folders/$folderId/access-bindings"
                Write-Host "   Add role: ai.vision.user"
                Write-Host ""
                Write-Host "2. Check billing account:" -ForegroundColor White
                Write-Host "   https://console.yandex.cloud/billing"
                Write-Host "   Status must be: ACTIVE or TRIAL_ACTIVE"
                Write-Host ""
                Write-Host "3. Wait 2-3 minutes after adding role" -ForegroundColor White
                Write-Host "   Permissions take time to activate!"
                Write-Host ""
                Write-Host "4. Try adding editor role:" -ForegroundColor White
                Write-Host "   Sometimes ai.vision.user is not enough"
                Write-Host ""
                
            }
            elseif ($statusCode -eq 401) {
                Write-Host "CAUSE: INVALID OR EXPIRED TOKEN" -ForegroundColor Red
                Write-Host ""
                Write-Host "Solution:" -ForegroundColor Cyan
                Write-Host "   yc init"
                
            }
            elseif ($statusCode -eq 400) {
                Write-Host "CAUSE: INVALID REQUEST FORMAT" -ForegroundColor Red
            }
            else {
                Write-Host "Unexpected error $statusCode" -ForegroundColor Red
            }
        }
        catch {
            Write-Host $errorBody -ForegroundColor Red
        }
    }
    else {
        Write-Host $_.Exception.Message -ForegroundColor Red
    }
    
    Write-Host ""
    Write-Host "=== Additional diagnostics ===" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Check folder permissions:" -ForegroundColor Yellow
    Write-Host "yc resource-manager folder list-access-bindings --id $folderId"
    Write-Host ""
    Write-Host "Check your roles:" -ForegroundColor Yellow  
    Write-Host "yc iam user-account list"
    Write-Host ""
    
    exit 1
}

Write-Host ""
Write-Host "=== Test complete ===" -ForegroundColor Cyan

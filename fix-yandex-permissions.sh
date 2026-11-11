#!/bin/bash

# Скрипт для диагностики и исправления прав доступа Yandex Cloud Vision OCR

echo "=== Диагностика Yandex Cloud Vision OCR ==="
echo ""

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Проверка 1: Yandex CLI установлен
echo "1. Проверка Yandex Cloud CLI..."
if ! command -v yc &> /dev/null; then
    echo -e "${RED}❌ Yandex Cloud CLI не установлен${NC}"
    echo "   Установите: https://yandex.cloud/ru/docs/cli/quickstart"
    exit 1
fi
echo -e "${GREEN}✓ Yandex Cloud CLI установлен${NC}"
echo ""

# Проверка 2: Авторизация
echo "2. Проверка авторизации..."
if ! yc config list &> /dev/null; then
    echo -e "${RED}❌ Не авторизованы в Yandex Cloud${NC}"
    echo "   Выполните: yc init"
    exit 1
fi
echo -e "${GREEN}✓ Авторизованы${NC}"
echo ""

# Получаем конфигурацию
FOLDER_ID=$(yc config get folder-id)
CLOUD_ID=$(yc config get cloud-id)

echo "3. Текущая конфигурация:"
echo "   Cloud ID: ${CLOUD_ID}"
echo "   Folder ID: ${FOLDER_ID}"
echo ""

# Проверка 4: Folder ID настроен
if [ -z "$FOLDER_ID" ]; then
    echo -e "${RED}❌ Folder ID не настроен${NC}"
    echo ""
    echo "Доступные каталоги:"
    yc resource-manager folder list
    echo ""
    echo -e "${YELLOW}Выполните:${NC}"
    echo "   yc config set folder-id <ID_каталога_из_списка_выше>"
    exit 1
fi
echo -e "${GREEN}✓ Folder ID настроен${NC}"
echo ""

# Проверка 5: Права доступа
echo "4. Проверка прав доступа к каталогу ${FOLDER_ID}..."
BINDINGS=$(yc resource-manager folder list-access-bindings --id $FOLDER_ID 2>&1)

if echo "$BINDINGS" | grep -q "permission denied\|Permission denied"; then
    echo -e "${RED}❌ Нет прав на просмотр привязок доступа${NC}"
    echo "   Обратитесь к администратору организации"
    exit 1
fi

echo "$BINDINGS"
echo ""

# Проверка роли ai.vision.user
echo "5. Проверка роли ai.vision.user..."
if echo "$BINDINGS" | grep -q "ai.vision.user"; then
    echo -e "${GREEN}✓ Роль ai.vision.user найдена${NC}"
else
    echo -e "${YELLOW}⚠ Роль ai.vision.user НЕ найдена${NC}"
    echo ""
    echo "Попробуем добавить роль..."
    
    # Получаем ID текущего пользователя
    USER_ID=$(yc iam user-account list --format json | jq -r '.[0].id')
    
    if [ -z "$USER_ID" ]; then
        echo -e "${RED}❌ Не удалось получить User ID${NC}"
        echo ""
        echo -e "${YELLOW}Добавьте роль вручную:${NC}"
        echo "1. Откройте https://console.yandex.cloud"
        echo "2. Выберите каталог ${FOLDER_ID}"
        echo "3. Access bindings → Assign bindings"
        echo "4. Добавьте роль: ai.vision.user"
        exit 1
    fi
    
    echo "User ID: ${USER_ID}"
    echo "Добавляем роль..."
    
    if yc resource-manager folder add-access-binding $FOLDER_ID \
        --role ai.vision.user \
        --subject userAccount:$USER_ID; then
        echo -e "${GREEN}✓ Роль ai.vision.user успешно добавлена!${NC}"
    else
        echo -e "${RED}❌ Не удалось добавить роль${NC}"
        echo ""
        echo -e "${YELLOW}Добавьте роль вручную через веб-консоль${NC}"
        exit 1
    fi
fi
echo ""

# Проверка 6: Billing account
echo "6. Проверка платежного аккаунта..."
BILLING=$(yc billing account list --format json 2>&1)

if echo "$BILLING" | grep -q "error\|Error"; then
    echo -e "${YELLOW}⚠ Не удалось проверить billing account${NC}"
    echo "   Проверьте вручную в веб-консоли:"
    echo "   https://console.yandex.cloud/billing"
else
    BILLING_STATUS=$(echo "$BILLING" | jq -r '.[0].status // empty')
    if [ "$BILLING_STATUS" = "ACTIVE" ] || [ "$BILLING_STATUS" = "TRIAL_ACTIVE" ]; then
        echo -e "${GREEN}✓ Billing account активен (${BILLING_STATUS})${NC}"
    else
        echo -e "${RED}❌ Billing account неактивен: ${BILLING_STATUS}${NC}"
        echo "   Активируйте на https://console.yandex.cloud/billing"
    fi
fi
echo ""

# Создаем новый IAM токен
echo "7. Создание нового IAM токена..."
IAM_TOKEN=$(yc iam create-token 2>&1)

if [ -z "$IAM_TOKEN" ] || echo "$IAM_TOKEN" | grep -q "error\|Error"; then
    echo -e "${RED}❌ Не удалось создать IAM токен${NC}"
    echo "$IAM_TOKEN"
    exit 1
fi

echo -e "${GREEN}✓ IAM токен создан${NC}"
echo ""

# Тест Vision OCR API
echo "8. Тестирование Vision OCR API..."
echo "   Отправка тестового запроса..."

TEST_IMAGE="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="

RESPONSE=$(curl -s -w "\nHTTP_CODE:%{http_code}" -X POST \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer ${IAM_TOKEN}" \
  -H "x-folder-id: ${FOLDER_ID}" \
  -d "{\"mimeType\":\"PNG\",\"languageCodes\":[\"en\"],\"model\":\"page\",\"content\":\"${TEST_IMAGE}\"}" \
  https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText)

HTTP_CODE=$(echo "$RESPONSE" | grep "HTTP_CODE:" | cut -d: -f2)
RESPONSE_BODY=$(echo "$RESPONSE" | sed '/HTTP_CODE:/d')

echo "   HTTP Status: ${HTTP_CODE}"
echo ""

if [ "$HTTP_CODE" = "200" ]; then
    echo -e "${GREEN}✅ SUCCESS! Vision OCR API работает!${NC}"
    echo ""
    echo "=== ✓ ВСЁ ГОТОВО К ИСПОЛЬЗОВАНИЮ ==="
    echo ""
    echo "Используйте эти credentials в chiaki-ng:"
    echo ""
    echo "IAM Token (первые 50 символов):"
    echo "  ${IAM_TOKEN:0:50}..."
    echo ""
    echo "Folder ID:"
    echo "  ${FOLDER_ID}"
    echo ""
    echo "Полный IAM Token (скопируйте):"
    echo "${IAM_TOKEN}"
    echo ""
    echo "Откройте chiaki-ng → Настройки → Конфигурация"
    echo "и вставьте эти значения"
    
elif [ "$HTTP_CODE" = "403" ]; then
    echo -e "${RED}❌ HTTP 403 Forbidden - Нет прав доступа${NC}"
    echo ""
    echo "Response:"
    echo "$RESPONSE_BODY" | jq '.' 2>/dev/null || echo "$RESPONSE_BODY"
    echo ""
    echo -e "${YELLOW}Решение:${NC}"
    echo "1. Добавьте роль ai.vision.user вручную:"
    echo "   https://console.yandex.cloud/folders/${FOLDER_ID}/access-bindings"
    echo ""
    echo "2. Или попробуйте другой каталог:"
    yc resource-manager folder list
    
elif [ "$HTTP_CODE" = "401" ]; then
    echo -e "${RED}❌ HTTP 401 Unauthorized - Проблема с токеном${NC}"
    echo "   Попробуйте повторно авторизоваться: yc init"
    
else
    echo -e "${YELLOW}⚠ Неожиданный ответ${NC}"
    echo "Response:"
    echo "$RESPONSE_BODY" | jq '.' 2>/dev/null || echo "$RESPONSE_BODY"
fi

echo ""
echo "=== Конец диагностики ==="


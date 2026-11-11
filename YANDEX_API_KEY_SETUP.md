# Альтернативное решение: API ключ вместо IAM токена

## Проблема с IAM токеном

IAM токены имеют несколько проблем:
- ⏰ Истекают через 12 часов
- 🔐 Требуют сложную настройку прав
- ⏱️ Права активируются с задержкой
- 🐛 Могут не работать из-за настроек организации

## ✅ Решение: Service Account с API ключом

API ключи:
- ✅ **Не истекают** (работают постоянно)
- ✅ Проще в настройке
- ✅ Стабильнее для программного доступа

## Пошаговая настройка

### Шаг 1: Создайте Service Account

```bash
# Создайте service account для chiaki-ng
yc iam service-account create \
  --name chiaki-translation \
  --description "Service account for chiaki-ng translation feature"

# Получите его ID
yc iam service-account get chiaki-translation
```

Запомните **ID** (будет выглядеть как `aje1234567890abcdef`)

### Шаг 2: Назначьте роли Service Account

```bash
# Получите ID service account
SA_ID=$(yc iam service-account get chiaki-translation --format json | jq -r .id)

# Получите folder ID
FOLDER_ID=$(yc config get folder-id)

# Если folder ID пустой, установите его
# yc config set folder-id <ваш_folder_id>

# Назначьте роль ai.vision.user
yc resource-manager folder add-access-binding $FOLDER_ID \
  --role ai.vision.user \
  --subject serviceAccount:$SA_ID

# Опционально: добавьте роль для Translate API
yc resource-manager folder add-access-binding $FOLDER_ID \
  --role ai.translate.user \
  --subject serviceAccount:$SA_ID

echo "✓ Roles assigned successfully"
```

### Шаг 3: Создайте API ключ

```bash
# Создайте API ключ для service account
yc iam api-key create \
  --service-account-name chiaki-translation \
  --description "API key for chiaki-ng OCR translation"
```

**ВАЖНО:** Скопируйте **secret** из вывода команды! Он больше не будет доступен.

Вывод будет примерно таким:
```yaml
api_key:
  id: ajek1234567890abcdef
  service_account_id: aje9876543210fedcba
  created_at: "2025-12-05T10:00:00Z"
  description: API key for chiaki-ng OCR translation
secret: AQVNxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

**Скопируйте `secret`** - это и есть ваш API ключ!

### Шаг 4А: ЕСЛИ API ключ поддерживается Vision OCR

(К сожалению, Vision OCR может требовать именно IAM токен, а не API ключ)

Если Vision OCR не поддерживает API ключи, переходите к **Шагу 4Б**.

### Шаг 4Б: Используйте Service Account IAM токен (РЕКОМЕНДУЕТСЯ)

Service Account IAM токены работают надёжнее:

```bash
# Создайте авторизованный ключ для service account
yc iam key create \
  --service-account-name chiaki-translation \
  --output key.json

# Получите IAM токен для service account
yc iam create-token --service-account-id $(yc iam service-account get chiaki-translation --format json | jq -r .id)
```

Этот токен:
- ✅ Имеет гарантированные права (через service account)
- ✅ Не зависит от прав вашего личного аккаунта
- ⚠️ Всё равно истекает через 12 часов

## Почему Service Account лучше?

| Параметр | User IAM Token | Service Account IAM Token |
|----------|----------------|---------------------------|
| Срок действия | 12 часов | 12 часов |
| Права | Личные (могут меняться) | Фиксированные |
| Надежность | Зависит от org settings | Стабильнее |
| Настройка | Сложнее | Проще |

## Полное решение через веб-консоль

Если CLI не работает, всё можно сделать через браузер:

### 1. Создайте Service Account

https://console.yandex.cloud/folders/YOUR_FOLDER_ID/service-accounts

1. **"Создать сервисный аккаунт"**
2. Имя: `chiaki-translation`
3. Роли: **`ai.vision.user`** + **`ai.translate.user`**
4. **"Создать"**

### 2. Создайте авторизованный ключ

1. Откройте созданный service account
2. **"Создать новый ключ"** → **"Создать авторизованный ключ"**
3. Скачайте файл `key.json`

### 3. Получите IAM токен через API

Используйте этот PowerShell скрипт с файлом `key.json`:

```powershell
# Загрузите ключ
$key = Get-Content key.json | ConvertFrom-Json

# Создайте JWT токен (требуется дополнительная библиотека)
# Или проще - используйте yc CLI
yc iam create-token --service-account-id $key.service_account_id
```

## Альтернатива: Используйте другой folder

Возможно у вас есть другой folder с более простыми настройками:

```bash
# Посмотрите все доступные folders
yc resource-manager folder list

# Попробуйте создать новый folder специально для chiaki
yc resource-manager folder create --name chiaki-ocr

# Установите его как default
NEW_FOLDER_ID=$(yc resource-manager folder list --format json | jq -r '.[] | select(.name=="chiaki-ocr") | .id')
yc config set folder-id $NEW_FOLDER_ID

# Добавьте себе роль admin на этот folder
yc resource-manager folder add-access-binding $NEW_FOLDER_ID \
  --role admin \
  --subject userAccount:$(yc iam user-account list --format json | jq -r '.[0].id')

# Получите новый токен
yc iam create-token
```

## Проверка прав (детальная)

Выполните для диагностики:

```powershell
# 1. Проверьте текущего пользователя
yc iam user-account list

# 2. Проверьте права на folder
yc resource-manager folder list-access-bindings --id b1gnoptgctl7f8pqmsp8

# 3. Проверьте можете ли вы вообще использовать Vision API
yc ai vision list

# 4. Проверьте квоты
yc resource-manager folder get b1gnoptgctl7f8pqmsp8

# 5. Проверьте billing
yc billing account list
```

## Если ничего не помогает - используйте мой тестовый метод

Я создам упрощённую версию которая покажет ТОЧНУЮ ошибку:

```powershell
# Скопируйте ваш ПОЛНЫЙ IAM токен
$token = "ВСТАВЬТЕ_ПОЛНЫЙ_ТОКЕН_СЮДА"
$folder = "b1gnoptgctl7f8pqmsp8"

# Минимальный тестовый запрос
$headers = @{
    "Authorization" = "Bearer $token"
    "x-folder-id" = $folder
}

try {
    # Простейший запрос к Vision API
    $result = Invoke-WebRequest `
        -Uri "https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText" `
        -Method POST `
        -Headers $headers `
        -Body '{"mimeType":"PNG","languageCodes":["en"],"model":"page","content":"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="}' `
        -ContentType "application/json"
    
    Write-Host "SUCCESS!" -ForegroundColor Green
    Write-Host $result.Content
    
} catch {
    Write-Host "Error:" -ForegroundColor Red
    Write-Host $_.Exception.Message
    Write-Host $_.ErrorDetails.Message | ConvertFrom-Json | ConvertTo-Json -Depth 10
}
```

Если это даст 403 - проблема **точно** в правах.  
Если даст 200 - проблема в том как chiaki-ng отправляет токен.

## Контакты поддержки Yandex Cloud

Если ничего не помогает:
- Поддержка: https://yandex.cloud/ru/docs/support/
- Форум: https://yandex.cloud/ru/docs/overview/community

Опишите проблему:
```
Добавил роль ai.vision.user для folder, создал IAM токен, 
но получаю 403 Permission denied при обращении к Vision OCR API.
Folder ID: b1gnoptgctl7f8pqmsp8
```

Вероятнее всего поддержка скажет что нужно:
1. Активировать Vision OCR API для вашего облака
2. Или добавить дополнительную роль `resource-manager.clouds.member`

Попробуйте сначала Вариант 1 (добавление роли через веб-консоль) и подождите 2-3 минуты! 🕐


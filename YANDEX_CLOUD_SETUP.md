# Настройка Yandex Cloud для функции перевода

## Проблема: `yc config get folder-id` возвращает пустое значение

Это означает что у вас не настроен каталог (folder) по умолчанию в Yandex Cloud CLI.

## Решение: Пошаговая настройка

### Шаг 1: Проверьте авторизацию

```bash
yc config list
```

Вы должны увидеть:
```
token: <ваш токен>
cloud-id: <id облака>
folder-id: <пусто или значение>
```

### Шаг 2: Получите список доступных каталогов

```bash
yc resource-manager folder list
```

Результат:
```
+----------------------+-------------+--------+--------+
|          ID          |    NAME     | LABELS | STATUS |
+----------------------+-------------+--------+--------+
| b1g9d2k0ria4om8k2qmc | default     |        | ACTIVE |
| b1gnhcshomo5kmi35q1t | my-folder   |        | ACTIVE |
+----------------------+-------------+--------+--------+
```

Если список пустой, значит у вас нет доступа к каталогам!

### Шаг 3А: Если каталоги есть - установите folder по умолчанию

```bash
# Используйте ID каталога из списка выше
yc config set folder-id b1g9d2k0ria4om8k2qmc

# Проверьте
yc config get folder-id
```

### Шаг 3Б: Если каталогов нет - создайте новый

```bash
# Получите ID вашего облака
yc config get cloud-id

# Создайте новый каталог
yc resource-manager folder create --name chiaki-translation

# Результат покажет ID нового каталога, установите его
yc config set folder-id <ID_нового_каталога>
```

### Шаг 4: Убедитесь что есть права на Vision OCR

```bash
# Получите ID вашего folder
FOLDER_ID=$(yc config get folder-id)

# Проверьте права доступа
yc resource-manager folder list-access-bindings --id $FOLDER_ID

# Если нужной роли нет, добавьте её
yc resource-manager folder add-access-binding $FOLDER_ID \
  --role ai.vision.user \
  --subject userAccount:<YOUR_USER_ID>
```

Чтобы получить YOUR_USER_ID:
```bash
yc iam user-account get --id <ваш email>
```

Или проще через консоль:
1. Откройте https://console.yandex.cloud
2. Выберите каталог
3. **Access bindings** → **Assign bindings**
4. Выберите ваш аккаунт
5. Добавьте роль **`ai.vision.user`**

### Шаг 5: Получите новый IAM токен

```bash
yc iam create-token
```

Скопируйте полученный токен.

### Шаг 6: Получите Folder ID

```bash
yc config get folder-id
```

Теперь должен показать ID!

### Шаг 7: Обновите настройки в chiaki-ng

1. Откройте **chiaki-ng** → **Настройки** → **Конфигурация**
2. Вставьте:
   - **IAM Token** (из шага 5)
   - **Folder ID** (из шага 6)
3. Сохраните
4. Перезапустите chiaki-ng

## Альтернатива: Использование Service Account

Если у вас сложности с правами пользователя, создайте Service Account:

```bash
# Создайте service account
yc iam service-account create --name chiaki-ocr

# Получите его ID
SA_ID=$(yc iam service-account get chiaki-ocr --format json | jq -r .id)

# Назначьте роль ai.vision.user
yc resource-manager folder add-access-binding $(yc config get folder-id) \
  --role ai.vision.user \
  --subject serviceAccount:$SA_ID

# Создайте authorized key для service account
yc iam key create --service-account-name chiaki-ocr -o key.json

# Получите IAM токен для service account
yc iam create-token --service-account-id $SA_ID
```

## Проверка что всё работает

Протестируйте доступ к Vision OCR API:

```bash
# Получите IAM токен
IAM_TOKEN=$(yc iam create-token)

# Получите folder ID
FOLDER_ID=$(yc config get folder-id)

# Создайте тестовое изображение (любое)
echo "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==" > test.txt

# Отправьте тестовый запрос
curl -X POST \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer ${IAM_TOKEN}" \
  -H "x-folder-id: ${FOLDER_ID}" \
  -d "{\"mimeType\":\"PNG\",\"languageCodes\":[\"en\"],\"model\":\"page\",\"content\":\"$(cat test.txt)\"}" \
  https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText
```

**Ожидаемый результат:** JSON с результатами распознавания или ошибка с деталями.

**Если получаете 403:** У вас нет прав. Вернитесь к Шагу 4.

**Если получаете 401:** IAM токен неверный или истек. Вернитесь к Шагу 5.

**Если получаете 200:** ✅ Всё работает! Используйте эти credentials в chiaki-ng.

## Полезные команды для отладки

```bash
# Посмотреть всю конфигурацию
yc config list

# Посмотреть информацию о текущем пользователе
yc iam user-account get

# Посмотреть список облаков
yc resource-manager cloud list

# Посмотреть список каталогов
yc resource-manager folder list

# Посмотреть список ролей для каталога
yc resource-manager folder list-access-bindings --id <folder-id>

# Обновить IAM токен
yc iam create-token

# Проверить квоты Vision OCR
yc resource-manager folder get <folder-id>
```

## Частые ошибки

### ❌ "folder-id is not set"

**Решение:** `yc config set folder-id <ID>`

### ❌ "Permission denied"

**Решение:** Добавьте роль `ai.vision.user` (см. Шаг 4)

### ❌ "Invalid authentication credentials"

**Решение:** Получите новый IAM токен (Шаг 5)

### ❌ "Folder not found"

**Решение:** Используйте folder из `yc resource-manager folder list`

## Быстрая настройка с нуля

Если у вас вообще ничего не настроено:

```bash
# 1. Инициализация
yc init

# Следуйте инструкциям, выберите:
# - Cloud
# - Folder
# - Zone

# 2. Проверьте что folder установлен
yc config get folder-id

# 3. Добавьте роль Vision OCR (в веб-консоли проще)

# 4. Получите IAM токен
yc iam create-token

# 5. Готово!
```

## Ссылки

- [Yandex Cloud CLI](https://yandex.cloud/ru/docs/cli/)
- [Vision OCR Documentation](https://yandex.cloud/ru/docs/vision/)
- [IAM Tokens](https://yandex.cloud/ru/docs/iam/operations/iam-token/create)
- [Access Management](https://yandex.cloud/ru/docs/iam/operations/roles/grant)


# Отладка функции перевода

## Как включить подробное логирование

### Вариант 1: Через интерфейс
1. Откройте **Настройки** → вкладка **Конфигурация**
2. Внизу страницы найдите чекбокс **"Подробное логирование"**
3. Включите его ✅

### Вариант 2: Через командную строку
Запустите chiaki с параметром для подробного логирования:

```bash
chiaki.exe --verbose
```

## Где найти логи

### Windows
Логи находятся в:
```
C:\Users\<ИМЯ_ПОЛЬЗОВАТЕЛЯ>\AppData\Local\Chiaki\
```

Или откройте через настройки:
1. **Настройки** → **Конфигурация**
2. В логах будет показана директория логов

### Во время работы
Если запускаете из терминала/консоли, логи выводятся прямо в консоль.

## Что искать в логах при нажатии Alt+T

При нажатии Alt+T вы должны увидеть следующую последовательность сообщений:

### 1. Обработка горячей клавиши
```
Alt key pressed with key: 84
Alt+T pressed - triggering translation toggle
```

### 2. Запуск процесса перевода
```
=== toggleTranslation() called ===
  text_overlay exists: true
  text_overlay->isActive(): false
Overlay not active, triggering new translation
=== triggerTranslation() START ===
  translation_in_progress: false
  has_video: true
  IAM Token length: <длина токена>
  Folder ID: b1gnhcshomo5kmi35q1t
```

### 3. Захват фрейма
```
=== captureCurrentFrame() START ===
  av_frame exists: true
  Frame info: 1920x1080 format: <число>
  hw_frames_ctx: true/false
...
✓ Frame captured successfully!
  Result image size: QSize(1920, 1080)
  Result isNull: false
=== captureCurrentFrame() END ===
```

### 4. Отправка к Yandex Cloud
```
=== YandexOCR::recognizeText() START ===
  Image size: QSize(1920, 1080)
  Image isNull: false
  isConfigured: true
Encoding image to base64...
  Base64 image length: <длина>
Sending OCR request to Yandex Cloud...
  URL: https://ocr.api.cloud.yandex.net/ocr/v1/recognizeText
  Folder ID: b1gnhcshomo5kmi35q1t
  Request size: <размер> bytes
OCR request sent, waiting for response...
```

### 5. Получение ответа
```
=== onRecognitionReplyFinished() START ===
  HTTP Status: 200
  Error code: 0
  Response size: <размер> bytes
✓ Response parsed successfully, processing...
YandexOCR: Recognized <N> text blocks
```

### 6. Перевод текста
```
=== onTranslationReplyFinished() ===
  Block index: 0
  HTTP Status: 200
YandexOCR: Translated block 0: <переведенный текст>
...
YandexOCR: All translations completed
```

### 7. Отображение оверлея
```
TextOverlay: Set <N> text blocks, image size: QSize(1920, 1080)
Translation overlay activated with <N> blocks
```

## Типичные проблемы и их диагностика

### ❌ Горячая клавиша не срабатывает

**Симптом:** Нет сообщения `Alt+T pressed`

**Причины:**
- Фокус не на окне chiaki-ng
- Клавиша перехватывается другим приложением
- Режим захвата ввода активен (grab_input)

**Решение:**
- Убедитесь что окно chiaki-ng в фокусе
- Закройте меню стрима если оно открыто (Ctrl+O)

### ❌ Нет видео

**Симптом:** 
```
⚠️ No video available for translation
```

**Причина:** Игра ещё не запущена или видео не инициализировано

**Решение:** Дождитесь загрузки игры и появления видео

### ❌ Учетные данные не настроены

**Симптом:**
```
⚠️ Yandex OCR credentials not configured!
  IAM Token empty: true
  Folder ID empty: true
```

**Причина:** IAM токен или Folder ID не введены в настройках

**Решение:**
1. Откройте **Настройки** → **Конфигурация**
2. Введите IAM токен (получить: `yc iam create-token`)
3. Введите Folder ID (получить: `yc config get folder-id`)
4. Перезапустите chiaki-ng

### ❌ Не удалось захватить фрейм

**Симптом:**
```
⚠️ No frame available to capture
```

**Причина:** Видео фрейм недоступен в момент захвата

**Решение:** Попробуйте нажать Alt+T снова через секунду

### ❌ Ошибка HTTP при запросе к API

**Симптом:**
```
HTTP Status: 401
⚠️ YandexOCR: Recognition error: Unauthorized
```

**Причины:**
- IAM токен истек (действует 12 часов)
- Неверный IAM токен
- Неверный Folder ID

**Решение:**
1. Получите новый IAM токен: `yc iam create-token`
2. Обновите его в настройках
3. Попробуйте снова

### ❌ Ошибка HTTP 403 Forbidden

**Симптом:**
```
HTTP Status: 403
```

**Причины:**
- Нет доступа к Vision OCR API
- Платежный аккаунт неактивен
- Превышены квоты

**Решение:**
1. Проверьте статус аккаунта в Yandex Cloud Console
2. Убедитесь что у вас есть роль `ai.vision.user` или выше
3. Проверьте квоты и лимиты

### ❌ Текст не найден

**Симптом:**
```
No text found in the image
```

**Причина:** На экране нет распознаваемого текста или текст слишком мелкий/нечеткий

**Решение:**
- Попробуйте захватить кадр с более четким текстом
- Убедитесь что текст достаточно крупный
- Попробуйте в месте с высоким контрастом текста

## Тестовая процедура

1. **Включите подробное логирование** в настройках
2. **Запустите игру** через chiaki-ng
3. **Дождитесь появления видео**
4. **Откройте консоль/терминал** где запущен chiaki (или смотрите лог-файл)
5. **Найдите экран с английским текстом** в игре
6. **Нажмите Alt+T**
7. **Наблюдайте логи** - они покажут каждый шаг процесса

## Пример успешного лога

```
[INFO] Alt+T pressed - triggering translation toggle
[INFO] === triggerTranslation() START ===
[INFO]   translation_in_progress: false
[INFO]   has_video: true
[INFO]   IAM Token length: 523
[INFO]   Folder ID: b1gnhcshomo5kmi35q1t
[INFO] Starting translation process...
[INFO] Capturing current frame...
[INFO] === captureCurrentFrame() START ===
[INFO]   av_frame exists: true
[INFO]   Frame info: 1920x1080 format: 61
[INFO] ✓ Frame captured successfully!
[INFO] === YandexOCR::recognizeText() START ===
[INFO] Encoding image to base64...
[INFO]   Base64 image length: 245678
[INFO] Sending OCR request to Yandex Cloud...
[INFO] === onRecognitionReplyFinished() START ===
[INFO]   HTTP Status: 200
[INFO]   Response size: 3456 bytes
[INFO] ✓ Response parsed successfully
[INFO] YandexOCR: Recognized 3 text blocks
[INFO] YandexOCR: Translated block 0: <текст>
[INFO] YandexOCR: All translations completed
[INFO] Translation overlay activated with 3 blocks
```

## Дополнительная помощь

Если проблема не решается:
1. Сделайте полный лог работы с момента запуска chiaki до нажатия Alt+T
2. Проверьте что IAM токен свежий (меньше 12 часов)
3. Убедитесь что у вас есть доступ к Yandex Cloud Vision OCR
4. Откройте issue на GitHub с логами


# GitHub Actions Workflows для chiaki-ng

## Обзор workflows для Windows сборки

### 🚀 `build-dev-quick.yml` - Супер-быстрая dev сборка

**Для:** Быстрое тестирование изменений в коде

**Триггеры:**
- Manual (`workflow_dispatch`)

**Особенности:**
- ⚡ Максимально быстрая (~5-10 минут после первого запуска)
- 💾 Агрессивное кэширование всех зависимостей
- 🔧 CCCache для инкрементальной компиляции
- 🎯 Только `chiaki.exe` (без DLL)
- 📦 Минимальный артефакт

**Оптимизации:**
- Кэш MSYS2 пакетов
- Кэш libplacebo (не пересобирается)
- Кэш FFmpeg (скачивается 1 раз)
- Кэш third-party библиотек
- CCCache для C/C++ компиляции
- Parallel build с максимальным количеством потоков
- Shallow git clone
- Без создания installer

**Когда использовать:**
- ✅ Тестирование изменений в коде
- ✅ Проверка что код компилируется
- ✅ Быстрая итерация разработки

**Когда НЕ использовать:**
- ❌ Для релиза
- ❌ Когда нужен полный portable package

---

### 🏃 `build-msys2-fast.yml` - Быстрая полная сборка

**Для:** Полноценная сборка с portable package

**Триггеры:**
- Manual (`workflow_dispatch`)
- Push в `main` ветку (только если изменились файлы GUI/lib)

**Особенности:**
- ⚡ Быстрая (~10-15 минут после кэширования)
- 💾 Кэширование зависимостей
- 📦 Полный portable package со всеми DLL
- 🔧 CCCache для ускорения
- 🎯 Готовый к использованию артефакт

**Оптимизации:**
- Кэш MSYS2 пакетов
- Кэш libplacebo
- Кэш FFmpeg
- Кэш CMake build третьих сторон
- CCCache
- Parallel build
- Автоматический запуск при изменении GUI кода

**Когда использовать:**
- ✅ Нужен готовый portable пакет
- ✅ Тестирование на другом ПК
- ✅ Проверка полной функциональности
- ✅ Автоматическая сборка при коммите

---

### 📋 `build-msys2.yml` - Оригинальная полная сборка

**Для:** Релизы и официальные сборки

**Особенности:**
- 📦 Portable package
- 💿 Installer (.exe)
- ✅ Полное тестирование
- 🐢 Самая медленная (~20-30 минут)

**Когда использовать:**
- ✅ Создание релиза
- ✅ Официальные сборки
- ✅ Нужен installer

---

## Как запустить workflow

### Вариант 1: Через GitHub Web UI

1. Откройте ваш репозиторий на GitHub
2. Перейдите в **Actions**
3. Выберите нужный workflow:
   - `Quick Dev Build` - для быстрой проверки
   - `Fast Build chiaki-ng Windows` - для полного package
4. Нажмите **Run workflow**
5. Выберите ветку (обычно `main`)
6. Нажмите зеленую кнопку **Run workflow**

### Вариант 2: Через GitHub CLI

```bash
# Quick dev build
gh workflow run build-dev-quick.yml

# Fast full build
gh workflow run build-msys2-fast.yml

# Standard build
gh workflow run build-msys2.yml
```

### Вариант 3: Автоматический запуск

`build-msys2-fast.yml` автоматически запускается при push в `main` если изменились:
- `gui/**`
- `lib/**`
- `CMakeLists.txt`

## Сравнение производительности

| Workflow | Первый запуск | С кэшем | Артефакт | Автозапуск |
|----------|---------------|---------|----------|------------|
| `build-dev-quick.yml` | ~20 мин | ~5-10 мин | `.exe` only | ❌ |
| `build-msys2-fast.yml` | ~25 мин | ~10-15 мин | Full portable | ✅ |
| `build-msys2.yml` | ~30 мин | ~20-25 мин | Portable + Installer | ❌ |

## Оптимизации в деталях

### 1. Кэширование MSYS2 пакетов
```yaml
- uses: actions/cache@v4
  with:
    path: D:\a\_temp\msys64
    key: msys2-packages-${{ runner.os }}-v2
```
**Экономия:** ~5-7 минут на установке пакетов

### 2. Кэширование libplacebo
```yaml
- uses: actions/cache@v4
  with:
    path: /mingw64/lib/libplacebo*
    key: libplacebo-${{ hashFiles('scripts/build-libplacebo-windows.sh') }}
```
**Экономия:** ~3-5 минут на сборке libplacebo

### 3. Кэширование FFmpeg
```yaml
- uses: actions/cache@v4
  with:
    path: ffmpeg-cached
    key: ffmpeg-7.1-win64-gpl
```
**Экономия:** ~2-3 минуты на скачивании и распаковке

### 4. CCCache для инкрементальной компиляции
```yaml
-DCMAKE_C_COMPILER_LAUNCHER=ccache
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```
**Экономия:** ~5-10 минут при повторных сборках

### 5. Параллельная сборка
```bash
cmake --build build -j $(nproc)  # Использует все CPU cores
```
**Экономия:** ~3-5 минут на компиляции

### 6. Shallow clone
```yaml
fetch-depth: 1  # Только последний коммит
```
**Экономия:** ~30 секунд на клонировании

## Мониторинг кэша

Посмотреть размер кэша:
1. Settings → Actions → Caches
2. Там будут видны все активные кэши

**Лимиты GitHub:**
- 10 GB на репозиторий
- Кэши старше 7 дней без использования удаляются

## Troubleshooting

### Сборка медленная даже с кэшем

**Проверьте:**
1. Кэши восстанавливаются (в логах должно быть "Cache restored successfully")
2. CCCache работает (ccache -s должен показывать hits)
3. Параллельная сборка использует все ядра

### Кэш не восстанавливается

**Причины:**
- Изменился ключ кэша (например, изменился файл зависимостей)
- Кэш истек (>7 дней)
- Кэш занимает >10 GB (превышен лимит)

**Решение:**
- Первая сборка после изменений будет медленной
- Последующие будут быстрыми

### Нехватка места для кэша

**Решение:**
1. Удалите старые кэши в Settings → Actions → Caches
2. Уменьшите `retention-days` для артефактов
3. Используйте `build-dev-quick.yml` (меньше артефактов)

## Рекомендации по использованию

### Во время активной разработки:

1. **Первый запуск дня:** `build-msys2-fast.yml`
   - Создаст все кэши
   - Получите полный portable package

2. **Последующие тесты:** `build-dev-quick.yml`
   - Только проверить что компилируется
   - Максимально быстро

3. **Перед коммитом:** `build-msys2-fast.yml`
   - Убедиться что полная сборка работает
   - Получить тестовый package

### Для релизов:

Используйте стандартный `build-msys2.yml` - он создает и installer, и portable version.

## Локальная оптимизация сборки

Для еще большей скорости локально:

```bash
# Установите ccache
pacman -S ccache

# Настройте CMake с ccache
cmake -B build -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# Parallel build
cmake --build build -j $(nproc)

# Проверить статистику ccache
ccache -s
```

## Дополнительные оптимизации (экспериментально)

Если нужна МАКСИМАЛЬНАЯ скорость, можно:

1. **Использовать precompiled headers:**
   ```cmake
   target_precompile_headers(chiaki PRIVATE <QtCore> <QtGui>)
   ```

2. **Unity builds (объединение cpp файлов):**
   ```cmake
   set_target_properties(chiaki PROPERTIES UNITY_BUILD ON)
   ```

3. **LTO отключить для dev сборок:**
   ```cmake
   -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
   ```

4. **Использовать RAM диск для build директории**

## Итоговые рекомендации

| Сценарий | Workflow | Время | Результат |
|----------|----------|-------|-----------|
| Быстрый тест кода | `build-dev-quick.yml` | ~5 мин | `.exe` |
| Тестовая сборка | `build-msys2-fast.yml` | ~10 мин | Portable package |
| Релиз | `build-msys2.yml` | ~20 мин | Full package + installer |
| Автоматическая проверка | `build-msys2-fast.yml` | ~10 мин | Auto on push |

**Общая экономия времени:** До 70% по сравнению со стандартной сборкой! 🚀


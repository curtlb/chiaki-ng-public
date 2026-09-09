# Chiaki Config Converter - C++ инструмент

Инструмент для конвертации INI конфига chiaki-ng в JSON формат для Android, используя нативные библиотеки chiaki-ng.

## Преимущества

- ✅ Использует те же функции парсинга, что и chiaki-ng
- ✅ Правильно обрабатывает @ByteArray формат через QSettings
- ✅ Гарантированно создает совместимый JSON формат
- ✅ Использует нативный код chiaki-ng для максимальной совместимости

## Автоматическая сборка через GitHub Actions

Самый простой способ получить готовый инструмент - использовать GitHub Actions:

1. Перейдите в раздел **Actions** в вашем репозитории
2. Найдите workflow **"Build Config Converter Tool"**
3. Нажмите **"Run workflow"** или он запустится автоматически при изменении файлов в `tools/`
4. После завершения сборки скачайте артефакт `chiaki-config-converter-windows`

Артефакт содержит готовый исполняемый файл и все необходимые DLL.

## Ручная компиляция

### Windows

1. Установите зависимости:
   - CMake 3.10+
   - Qt6 (Core компонент)
   - Компилятор C++ (MSVC, MinGW, или Clang)

2. Соберите проект:

```powershell
# Создайте директорию для сборки
mkdir build
cd build

# Настройте CMake (укажите путь к Qt6 если нужно)
cmake .. -DCHIAKI_ENABLE_GUI=ON

# Соберите проект
cmake --build . --config Release

# Или через Visual Studio
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

После сборки инструмент будет в `build/tools/Release/chiaki-config-converter.exe` (или `Debug` в зависимости от конфигурации).

### Linux

```bash
# Установите зависимости (пример для Ubuntu/Debian)
sudo apt-get install cmake qt6-base-dev build-essential

# Соберите проект
mkdir build
cd build
cmake .. -DCHIAKI_ENABLE_GUI=ON
make chiaki-config-converter
```

### macOS

```bash
# Установите зависимости через Homebrew
brew install cmake qt@6

# Соберите проект
mkdir build
cd build
cmake .. -DCHIAKI_ENABLE_GUI=ON -DQt6_DIR=$(brew --prefix qt@6)/lib/cmake/Qt6
make chiaki-config-converter
```

## Использование

```bash
# Windows
chiaki-config-converter.exe -i "C:\Users\YourName\Desktop\config.ini" -o "C:\Users\YourName\Desktop\config.json"

# Linux/macOS
./chiaki-config-converter -i ~/Desktop/config.ini -o ~/Desktop/config.json

# Или без указания выходного файла (создастся автоматически с расширением .json)
./chiaki-config-converter -i config.ini
```

## Что делает

1. Загружает INI файл через QSettings (правильный парсинг @ByteArray)
2. Использует `RegisteredHost::LoadFromSettings` для правильного парсинга хостов
3. Определяет активный хост (из manual_hosts с registered=true)
4. Сериализует в JSON формат, совместимый с Android приложением

## Альтернатива: Python скрипт

Если у вас нет возможности скомпилировать C++ инструмент, используйте Python скрипт `convert_chiaki_config.py` в корне проекта - он работает аналогично, но использует Python библиотеки для парсинга.

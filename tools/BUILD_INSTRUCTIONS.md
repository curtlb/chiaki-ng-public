# Инструкция по сборке chiaki-config-converter

## Быстрый старт (Windows)

Если у вас уже есть собранный проект chiaki-ng:

```powershell
# Перейдите в директорию проекта
cd C:\Users\curtlb\Documents\GitHub\chiaki-ng

# Создайте директорию для сборки (если еще не создана)
mkdir build
cd build

# Настройте CMake (если еще не настроен)
cmake .. -DCHIAKI_ENABLE_GUI=ON

# Соберите только инструмент конвертации
cmake --build . --target chiaki-config-converter --config Release
```

После сборки инструмент будет в:
- `build\tools\Release\chiaki-config-converter.exe` (Release)
- `build\tools\Debug\chiaki-config-converter.exe` (Debug)

## Использование

```powershell
# Пример использования
.\build\tools\Release\chiaki-config-converter.exe -i "C:\Users\curtlb\Desktop\psaccount80.ini" -o "C:\Users\curtlb\Desktop\output.json"

# Или просто (автоматически создаст .json файл)
.\build\tools\Release\chiaki-config-converter.exe -i "C:\Users\curtlb\Desktop\psaccount80.ini"
```

## Если проект еще не собран

1. Установите зависимости:
   - **CMake** (3.10+): https://cmake.org/download/
   - **Qt6**: https://www.qt.io/download
   - **Visual Studio** или **MinGW** (компилятор C++)

2. Соберите проект как описано выше

## Альтернатива: Python скрипт

Если компиляция вызывает проблемы, используйте Python скрипт `convert_chiaki_config.py` в корне проекта:

```powershell
python convert_chiaki_config.py "C:\Users\curtlb\Desktop\psaccount80.ini" "C:\Users\curtlb\Desktop\output.json"
```

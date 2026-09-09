#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
Конвертер конфигураций Chiaki-ng

Конвертирует конфиги из формата chiaki-ng (INI с @ByteArray) 
в JSON формат с base64-кодированными ключами.

Проблема: chiaki-ng использует формат @ByteArray с escape-последовательностями,
которые могут быть неправильно обработаны другими приложениями.
"""

import re
import base64
import json
import configparser
from typing import Dict, List, Optional, Tuple

# Размеры ключей в байтах
RP_KEY_SIZE = 0x10  # 16 байт
RP_REGIST_KEY_SIZE = 0x10  # 16 байт (CHIAKI_SESSION_AUTH_SIZE)
MAC_SIZE = 6

def parse_bytearray(value: str) -> bytes:
    r"""
    Парсит формат @ByteArray(...) из QSettings.
    
    Формат может содержать:
    - Hex escape: \xHH
    - Null byte: \0
    - Newline: \n
    - Tab: \t
    - Bell: \a (0x07)
    - Backspace: \b (0x08)
    - Return: \r (0x0D)
    - Literal bytes (без escape)
    
    Args:
        value: Строка в формате @ByteArray(...) или просто содержимое в скобках
        
    Returns:
        bytes: Распарсенные байты
    """
    # Убираем кавычки если есть (в начале и конце)
    value = value.strip().strip('"')
    
    # Убираем @ByteArray( и ) если есть
    # Важно: проверяем закрывающую скобку ДО парсинга, чтобы не парсить её как обычный символ
    if value.startswith('@ByteArray('):
        value = value[11:]  # len('@ByteArray(') = 11
        # Убираем закрывающую скобку только если она в самом конце (не часть данных)
        # Нужно проверить, что перед ) нет неэкранированного обратного слеша
        if value.endswith(')') and len(value) > 0:
            # Проверяем, не является ли ) частью escape последовательности
            # Идем назад от конца, считая количество обратных слешей подряд
            check_pos = len(value) - 2
            backslash_count = 0
            while check_pos >= 0 and value[check_pos] == '\\':
                backslash_count += 1
                check_pos -= 1
            
            # Если четное количество обратных слешей (включая 0), то ) не экранирована
            # Это означает, что это закрывающая скобка @ByteArray, а не часть данных
            if (backslash_count % 2) == 0:
                value = value[:-1]
    
    result = bytearray()
    i = 0
    while i < len(value):
        if value[i] == '\\' and i + 1 < len(value):
            # Escape последовательность
            esc_char = value[i + 1]
            if esc_char == 'x':
                # Hex escape: \xHH (должно быть ровно 2 hex цифры)
                # Qt QSettings использует формат \xHH где HH - это ровно 2 hex цифры
                # Если после \x идет не hex цифра или только одна цифра, это не валидный escape
                if i + 3 < len(value):
                    # Проверяем, что есть минимум 2 символа после \x
                    char1 = value[i + 2]
                    char2 = value[i + 3] if i + 3 < len(value) else None
                    
                    # Проверяем, что оба символа - hex цифры
                    if char1 in '0123456789abcdefABCDEF' and char2 and char2 in '0123456789abcdefABCDEF':
                        # Валидный hex escape \xHH
                        try:
                            hex_byte = value[i + 2:i + 4]
                            result.append(int(hex_byte, 16))
                            i += 4
                        except ValueError:
                            # Если не hex, добавляем как обычные символы
                            result.append(ord('\\'))
                            result.append(ord('x'))
                            i += 2
                    else:
                        # Не валидный hex escape (например, \x1u), добавляем как литерал
                        result.append(ord('\\'))
                        result.append(ord('x'))
                        i += 2
                else:
                    # Недостаточно символов для \xHH, добавляем как обычные символы
                    result.append(ord('\\'))
                    result.append(ord('x'))
                    i += 2
            elif esc_char == '0':
                # Null byte: \0
                result.append(0)
                i += 2
            elif esc_char == 'n':
                # Newline: \n
                result.append(0x0A)
                i += 2
            elif esc_char == 't':
                # Tab: \t
                result.append(0x09)
                i += 2
            elif esc_char == 'r':
                # Return: \r
                result.append(0x0D)
                i += 2
            elif esc_char == 'a':
                # Bell: \a
                result.append(0x07)
                i += 2
            elif esc_char == 'b':
                # Backspace: \b
                result.append(0x08)
                i += 2
            elif esc_char == '\\':
                # Literal backslash: \\
                result.append(ord('\\'))
                i += 2
            else:
                # Неизвестная escape, добавляем как есть
                result.append(ord('\\'))
                result.append(ord(esc_char))
                i += 2
        else:
            # Обычный символ
            result.append(ord(value[i]))
            i += 1
    
    return bytes(result)

def parse_hex_string(value: str) -> Optional[bytes]:
    """
    Парсит hex-строку (например, для rp_regist_key в некоторых случаях).
    
    Args:
        value: Hex строка
        
    Returns:
        bytes: Распарсенные байты или None если ошибка
    """
    value = value.strip()
    # Убираем префиксы если есть
    if value.startswith('0x') or value.startswith('0X'):
        value = value[2:]
    # Убираем пробелы
    value = value.replace(' ', '').replace('\n', '').replace('\t', '')
    try:
        return bytes.fromhex(value)
    except ValueError:
        return None

def parse_mac(value: str) -> Optional[str]:
    r"""
    Парсит MAC адрес из различных форматов.
    
    Args:
        value: MAC адрес в формате @ByteArray или строке
        
    Returns:
        str: MAC адрес в формате "XX:XX:XX:XX:XX:XX" или None
    """
    value_clean = value.strip().strip('"')
    
    # Парсим как ByteArray если это формат
    if '@ByteArray' in value_clean:
        mac_bytes = parse_bytearray(value_clean)
        if mac_bytes and len(mac_bytes) == MAC_SIZE:
            return ':'.join(f'{b:02x}' for b in mac_bytes)
        elif mac_bytes and len(mac_bytes) > MAC_SIZE:
            # Берём первые 6 байт
            return ':'.join(f'{b:02x}' for b in mac_bytes[:MAC_SIZE])
    else:
        # Может быть уже в формате MAC
        # Проверяем формат XX:XX:XX:XX:XX:XX или XXXXXXXXXXXX
        mac_match = re.match(r'^([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}$|^[0-9A-Fa-f]{12}$', value_clean)
        if mac_match:
            if ':' in value_clean or '-' in value_clean:
                return value_clean.lower().replace('-', ':')
            else:
                # Формат без разделителей
                return ':'.join(value_clean[i:i+2] for i in range(0, len(value_clean), 2)).lower()
    
    return None

def parse_target(value: str) -> str:
    """
    Конвертирует числовое значение target в строковое представление.
    
    Args:
        value: Числовое значение target
        
    Returns:
        str: Строковое представление (PS4_10, PS5_1 и т.д.)
    """
    try:
        target_int = int(value)
        # CHIAKI_TARGET_PS5_1 = 1000100 (из примера)
        # CHIAKI_TARGET_PS4_10 = 1000000 (предположительно)
        if target_int == 1000100:
            return "PS5_1"
        elif target_int == 1000000:
            return "PS4_10"
        elif target_int >= 1000100:
            # PS5 варианты
            ps5_num = target_int - 1000100 + 1
            return f"PS5_{ps5_num}"
        elif target_int >= 1000000:
            # PS4 варианты
            ps4_num = target_int - 1000000 + 10
            return f"PS4_{ps4_num}"
        else:
            return f"UNKNOWN_{target_int}"
    except ValueError:
        return value

def convert_ini_to_json(ini_file: str, output_file: Optional[str] = None) -> Dict:
    """
    Конвертирует INI конфиг chiaki-ng в JSON формат.
    
    Args:
        ini_file: Путь к INI файлу
        output_file: Путь для сохранения JSON (опционально)
        
    Returns:
        dict: JSON структура с настройками
    """
    # Отключаем интерполяцию, чтобы избежать проблем с % в значениях
    config = configparser.ConfigParser(allow_no_value=True, interpolation=None)
    # Сохраняем регистр ключей
    config.optionxform = str
    
    try:
        config.read(ini_file, encoding='utf-8')
    except Exception as e:
        print(f"Ошибка чтения INI файла: {e}")
        return {}
    
    result = {
        "format": "chiaki-settings",
        "version": 2,
        "settings": {
            "registered_hosts": [],
            "manual_hosts": []
        }
    }
    
    # Определяем активный хост (тот, который будет виден при импорте)
    # Сначала проверяем manual_hosts - если есть registered=true, то это активный хост
    active_host_mac = None
    if 'manual_hosts' in config:
        manual_size = config.getint('manual_hosts', 'size', fallback=0)
        for i in range(1, manual_size + 1):
            prefix = f"{i}\\"
            if config.has_option('manual_hosts', f'{prefix}registered'):
                registered = config.get('manual_hosts', f'{prefix}registered')
                if registered.lower() == 'true':
                    if config.has_option('manual_hosts', f'{prefix}registered_mac'):
                        mac_value = config.get('manual_hosts', f'{prefix}registered_mac')
                        active_host_mac = parse_mac(mac_value)
                        break
    
    # Парсим registered_hosts
    if 'registered_hosts' in config:
        registered_hosts = []
        size = config.getint('registered_hosts', 'size', fallback=0)
        
        for i in range(1, size + 1):
            prefix = f"{i}\\"
            host = {}
            
            # Парсим target
            if config.has_option('registered_hosts', f'{prefix}target'):
                target = config.get('registered_hosts', f'{prefix}target')
                host['target'] = parse_target(target)
            
            # Парсим простые строковые поля
            # Android приложение использует null для пустых значений (ap_ssid, ap_key)
            # Порядок полей важен! Следуем порядку из SerializedRegisteredHost
            # Android приложение НЕ использует console_pin в JSON - пропускаем его
            
            # target уже добавлен выше
            
            # ap_ssid (пустые строки, не null)
            if config.has_option('registered_hosts', f'{prefix}ap_ssid'):
                value = config.get('registered_hosts', f'{prefix}ap_ssid')
                host['ap_ssid'] = "" if not value or value == "" else value
            
            # ap_bssid
            if config.has_option('registered_hosts', f'{prefix}ap_bssid'):
                value = config.get('registered_hosts', f'{prefix}ap_bssid')
                host['ap_bssid'] = value if value else ""
            
            # ap_key (пустые строки, не null)
            if config.has_option('registered_hosts', f'{prefix}ap_key'):
                value = config.get('registered_hosts', f'{prefix}ap_key')
                host['ap_key'] = "" if not value or value == "" else value
            
            # ap_name
            if config.has_option('registered_hosts', f'{prefix}ap_name'):
                value = config.get('registered_hosts', f'{prefix}ap_name')
                host['ap_name'] = value if value else ""
            
            # server_mac (порядок важен!)
            if config.has_option('registered_hosts', f'{prefix}server_mac'):
                mac_value = config.get('registered_hosts', f'{prefix}server_mac')
                mac = parse_mac(mac_value)
                if mac:
                    host['server_mac'] = mac
            
            # server_nickname
            if config.has_option('registered_hosts', f'{prefix}server_nickname'):
                value = config.get('registered_hosts', f'{prefix}server_nickname')
                host['server_nickname'] = value if value else None
            
            # console_pin НЕ добавляем - Android приложение его не использует в JSON
            
            # Парсим rp_regist_key
            if config.has_option('registered_hosts', f'{prefix}rp_regist_key'):
                rp_regist_key_value = config.get('registered_hosts', f'{prefix}rp_regist_key')
                rp_regist_key_bytes = None
                
                if rp_regist_key_value.startswith('@ByteArray'):
                    # Извлекаем содержимое ByteArray
                    content = rp_regist_key_value
                    if content.startswith('@ByteArray('):
                        content = content[11:]
                    if content.endswith(')'):
                        content = content[:-1]
                    content = content.strip().strip('"')
                    
                    # Проверяем, является ли это hex строкой перед escape-последовательностями
                    # Android приложение ожидает, что rp_regist_key будет hex строкой как UTF-8 байты
                    # (например, "6b18efc2" как байты 36 62 31 38 65 66 63 32, а не 6b 18 ef c2)
                    # Убираем все escape-последовательности для проверки
                    content_clean = content.replace('\\0', '').replace('\\', '')
                    if re.match(r'^[0-9a-fA-F]+', content_clean):
                        # Это hex строка, парсим как UTF-8 строку (не как hex байты!)
                        hex_part = re.match(r'^[0-9a-fA-F]+', content_clean)
                        if hex_part:
                            hex_str = hex_part.group(0)
                            # Конвертируем hex строку в UTF-8 байты (ASCII символы), а не в hex байты
                            # Это важно для Android приложения, которое ожидает hex строку в UTF-8
                            rp_regist_key_bytes = hex_str.encode('utf-8')
                            # Дополняем нулевыми байтами до нужного размера (16 байт)
                            rp_regist_key_bytes = rp_regist_key_bytes.ljust(RP_REGIST_KEY_SIZE, b'\x00')
                    else:
                        # Обычный парсинг ByteArray
                        rp_regist_key_bytes = parse_bytearray(rp_regist_key_value)
                else:
                    # Попробуем как hex строку
                    rp_regist_key_bytes = parse_hex_string(rp_regist_key_value)
                
                if rp_regist_key_bytes:
                    # Убедимся, что размер правильный (дополним нулями если нужно)
                    if len(rp_regist_key_bytes) < RP_REGIST_KEY_SIZE:
                        rp_regist_key_bytes = rp_regist_key_bytes.ljust(RP_REGIST_KEY_SIZE, b'\x00')
                    elif len(rp_regist_key_bytes) > RP_REGIST_KEY_SIZE:
                        rp_regist_key_bytes = rp_regist_key_bytes[:RP_REGIST_KEY_SIZE]
                    
                    host['rp_regist_key'] = base64.b64encode(rp_regist_key_bytes).decode('ascii')
            
            # Парсим rp_key_type
            if config.has_option('registered_hosts', f'{prefix}rp_key_type'):
                rp_key_type = config.get('registered_hosts', f'{prefix}rp_key_type')
                try:
                    host['rp_key_type'] = int(rp_key_type)
                except ValueError:
                    host['rp_key_type'] = 0
            
            # Парсим rp_key
            if config.has_option('registered_hosts', f'{prefix}rp_key'):
                rp_key_value = config.get('registered_hosts', f'{prefix}rp_key')
                rp_key_bytes = None
                
                if rp_key_value.startswith('@ByteArray'):
                    rp_key_bytes = parse_bytearray(rp_key_value)
                else:
                    # Попробуем как hex строку
                    rp_key_bytes = parse_hex_string(rp_key_value)
                
                if rp_key_bytes:
                    # Убедимся, что размер правильный
                    # rp_key должен быть ровно 16 байт (0x10)
                    if len(rp_key_bytes) < RP_KEY_SIZE:
                        # Дополняем нулями если меньше
                        rp_key_bytes = rp_key_bytes.ljust(RP_KEY_SIZE, b'\x00')
                    elif len(rp_key_bytes) > RP_KEY_SIZE:
                        # Обрезаем до 16 байт если больше (берем первые 16 байт)
                        # Это важно, так как в INI файле могут быть лишние байты
                        rp_key_bytes = rp_key_bytes[:RP_KEY_SIZE]
                    
                    host['rp_key'] = base64.b64encode(rp_key_bytes).decode('ascii')
            
            if host:
                registered_hosts.append(host)
        
        # Если не нашли активный хост в manual_hosts, берем первый из registered_hosts
        if not active_host_mac and registered_hosts:
            active_host_mac = registered_hosts[0].get('server_mac')
        
        # Оставляем только активный хост
        original_host_count = len(registered_hosts)
        if active_host_mac:
            filtered_hosts = [h for h in registered_hosts if h.get('server_mac') == active_host_mac]
            if filtered_hosts:
                registered_hosts = filtered_hosts
                print(f"Найден активный хост: {active_host_mac} (оставлен 1 из {original_host_count} хостов)")
            else:
                # Если не нашли соответствующий хост, берем первый
                if registered_hosts:
                    registered_hosts = [registered_hosts[0]]
                    print(f"Активный хост не найден в registered_hosts, оставлен первый хост (из {original_host_count} хостов)")
        elif registered_hosts:
            # Если нет active_host_mac, берем первый хост
            registered_hosts = [registered_hosts[0]]
            print(f"Активный хост не определен, оставлен первый хост (из {original_host_count} хостов)")
        
        result['settings']['registered_hosts'] = registered_hosts
    
    # Парсим manual_hosts (только те, что соответствуют активному хосту)
    if 'manual_hosts' in config:
        manual_hosts = []
        size = config.getint('manual_hosts', 'size', fallback=0)
        
        for i in range(1, size + 1):
            prefix = f"{i}\\"
            host = {}
            
            if config.has_option('manual_hosts', f'{prefix}host'):
                host['host'] = config.get('manual_hosts', f'{prefix}host')
            
            if config.has_option('manual_hosts', f'{prefix}registered_mac'):
                mac_value = config.get('manual_hosts', f'{prefix}registered_mac')
                mac = parse_mac(mac_value)
                if mac:
                    host['server_mac'] = mac
                    # Оставляем только manual_host, который соответствует активному registered_host
                    if active_host_mac and mac == active_host_mac:
                        manual_hosts.append(host)
                        break  # Берем только первый подходящий
            
            # Если нет active_host_mac, берем первый manual_host
            if not active_host_mac and host:
                manual_hosts.append(host)
                break
        
        result['settings']['manual_hosts'] = manual_hosts
    
    # Сохраняем в файл если указан
    if output_file:
        with open(output_file, 'w', encoding='utf-8') as f:
            # Используем indent=2 и ensure_ascii=False для совместимости с Android приложением
            # allow_nan=False, чтобы не допустить NaN/Infinity
            json.dump(result, f, indent=2, ensure_ascii=False, allow_nan=False)
        print(f"Конфиг сохранен в: {output_file}")
    
    return result

def main():
    import sys
    
    if len(sys.argv) < 2:
        print("Использование: python convert_chiaki_config.py <input.ini> [output.json]")
        print("\nПример:")
        print("  python convert_chiaki_config.py plusaccount.ini output.json")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    if not output_file:
        # Автоматически генерируем имя файла
        output_file = input_file.rsplit('.', 1)[0] + '.json'
    
    print(f"Чтение конфига: {input_file}")
    result = convert_ini_to_json(input_file, output_file)
    
    # Выводим информацию для отладки
    print(f"\nКонвертировано:")
    print(f"  Зарегистрированных хостов: {len(result['settings']['registered_hosts'])}")
    print(f"  Ручных хостов: {len(result['settings']['manual_hosts'])}")
    
    # Предупреждение, если оставлено больше одного хоста
    if len(result['settings']['registered_hosts']) > 1:
        print(f"\n⚠ Внимание: В выходном файле {len(result['settings']['registered_hosts'])} хостов!")
        print(f"   При импорте в chiaki-ng будет предложен только один хост.")
    
    if result['settings']['registered_hosts']:
        host = result['settings']['registered_hosts'][0]
        print(f"\nПервый хост:")
        print(f"  Target: {host.get('target', 'N/A')}")
        print(f"  MAC: {host.get('server_mac', 'N/A')}")
        print(f"  RP Key Type: {host.get('rp_key_type', 'N/A')}")
        if 'rp_key' in host:
            rp_key_bytes = base64.b64decode(host['rp_key'])
            print(f"  RP Key (hex): {rp_key_bytes.hex()}")
            print(f"  RP Key (size): {len(rp_key_bytes)} байт")
        if 'rp_regist_key' in host:
            rp_regist_key_bytes = base64.b64decode(host['rp_regist_key'])
            print(f"  RP Regist Key (hex): {rp_regist_key_bytes.hex()}")
            print(f"  RP Regist Key (size): {len(rp_regist_key_bytes)} байт")

if __name__ == '__main__':
    main()

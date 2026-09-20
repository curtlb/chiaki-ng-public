#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import sys
sys.path.insert(0, '.')
from convert_chiaki_config import parse_bytearray
import base64
import configparser

config = configparser.ConfigParser(allow_no_value=True, interpolation=None)
config.optionxform = str
config.read(r'C:\Users\curtlb\Desktop\psaccount80.ini', encoding='utf-8')

# Читаем реальное значение из INI
rp_key_raw = config.get('registered_hosts', '2\\rp_key')
print(f'Реальное значение из INI: {rp_key_raw}')
print(f'Длина строки: {len(rp_key_raw)}')
print()

# Парсим
result = parse_bytearray(rp_key_raw)
print('Результат парсинга:')
print(f'  Hex: {result.hex()}')
print(f'  Length: {len(result)}')
print(f'  Expected length: 16')
print(f'  Base64: {base64.b64encode(result).decode("ascii")}')

expected = base64.b64decode('Lv/uzChwkqyCePhceDF1Kw==')
print(f'\nExpected:')
print(f'  Hex: {expected.hex()}')
print(f'  Length: {len(expected)}')
print(f'  Base64: Lv/uzChwkqyCePhceDF1Kw==')

print(f'\nMatch: {result.hex() == expected.hex() and len(result) == len(expected)}')

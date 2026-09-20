#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import sys
sys.path.insert(0, '.')
from convert_chiaki_config import parse_bytearray
import base64

test_str = r'@ByteArray(.\xff\xee\xcc(p\x92\xac\x82x\xf8\x1u+\xdd))'
result = parse_bytearray(test_str)

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

if len(result) != len(expected):
    print(f'\nРазница в длине: {len(result) - len(expected)} байт')
    if len(result) > len(expected):
        print(f'Лишние байты: {result[len(expected):].hex()}')

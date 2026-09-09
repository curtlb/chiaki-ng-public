#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Тестируем парсинг \x1u
test_str = r'x\xf8\x1u+x'

print("=== Тест парсинга \\x1u ===\n")
print(f"Исходная строка: {test_str}")
print()

# Симулируем парсинг
value = test_str
result = bytearray()
i = 0

while i < len(value):
    if value[i] == '\\' and i + 1 < len(value):
        esc_char = value[i + 1]
        print(f"Позиция {i}: Escape символ '{esc_char}'")
        
        if esc_char == 'x':
            # Hex escape
            if i + 3 < len(value):
                char1 = value[i + 2]
                char2 = value[i + 3] if i + 3 < len(value) else None
                
                print(f"  char1: '{char1}', char2: '{char2}'")
                print(f"  char1 is hex: {char1 in '0123456789abcdefABCDEF'}")
                print(f"  char2 is hex: {char2 and char2 in '0123456789abcdefABCDEF'}")
                
                if char1 in '0123456789abcdefABCDEF' and char2 and char2 in '0123456789abcdefABCDEF':
                    # Валидный hex escape
                    hex_byte = value[i + 2:i + 4]
                    byte_val = int(hex_byte, 16)
                    print(f"  Валидный hex escape: \\x{hex_byte} = 0x{byte_val:02x}")
                    result.append(byte_val)
                    i += 4
                else:
                    # Не валидный hex escape
                    print(f"  НЕ валидный hex escape, добавляем как литерал: \\ + x")
                    result.append(ord('\\'))
                    result.append(ord('x'))
                    i += 2
            else:
                print(f"  Недостаточно символов для \\xHH")
                result.append(ord('\\'))
                result.append(ord('x'))
                i += 2
        else:
            print(f"  Другой escape символ")
            i += 2
    else:
        print(f"Позиция {i}: Обычный символ '{value[i]}' = {ord(value[i])}")
        result.append(ord(value[i]))
        i += 1

print(f"\nРезультат: {result.hex()}")
print(f"Длина: {len(result)}")
print(f"Ожидаемая длина (если \\x1u - литерал): 8")
print(f"Ожидаемая длина (если \\x1u парсится как \\x1 + u): 6")

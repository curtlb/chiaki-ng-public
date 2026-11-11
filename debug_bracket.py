#!/usr/bin/env python3
# -*- coding: utf-8 -*-

test_str = r'@ByteArray(.\xff\xee\xcc(p\x92\xac\x82x\xf8\x1u+\xdd))'

print("=== Детальная отладка удаления скобки ===\n")
print(f"Исходная строка: {test_str}")
print(f"Длина: {len(test_str)}")
print()

# Шаг 1: strip
value = test_str.strip().strip('"')
print(f"После strip: {value}")
print(f"Длина: {len(value)}")
print()

# Шаг 2: Удаляем @ByteArray(
if value.startswith('@ByteArray('):
    value = value[11:]  # len('@ByteArray(') = 11
    print(f"После удаления @ByteArray(: {value}")
    print(f"Длина: {len(value)}")
    print(f"Последний символ: '{value[-1]}'")
    print()
    
    # Проверяем, не является ли ) частью escape последовательности
    if value.endswith(')') and len(value) > 0:
        print("Проверка экранирования скобки:")
        check_pos = len(value) - 2
        backslash_count = 0
        while check_pos >= 0 and value[check_pos] == '\\':
            backslash_count += 1
            check_pos -= 1
            print(f"  Позиция {check_pos + 1}: обратный слеш")
        
        print(f"Количество обратных слешей перед ): {backslash_count}")
        print(f"Четное количество (скобка НЕ экранирована): {(backslash_count % 2) == 0}")
        
        if (backslash_count % 2) == 0:
            value = value[:-1]
            print(f"Удаляем ): {value}")
            print(f"Длина после удаления: {len(value)}")
        else:
            print("Скобка экранирована, не удаляем")

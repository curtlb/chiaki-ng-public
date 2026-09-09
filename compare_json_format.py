#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import json
import base64

# НЕкорректный конфиг
incorrect = {
  "format": "chiaki-settings",
  "version": 2,
  "settings": {
    "registered_hosts": [
      {
        "target": "PS5_1",
        "ap_ssid": None,
        "ap_bssid": "3132333435",
        "ap_key": None,
        "ap_name": "PS5",
        "server_mac": "2c:9e:00:b3:4f:a2",
        "server_nickname": "PS5-855",
        "rp_regist_key": "MTZlN2E4MWIAAAAAAAAAAA==",
        "rp_key_type": 2,
        "rp_key": "Lv/uzChwkqyCePhceDF1Kw=="
      }
    ],
    "manual_hosts": [
      {
        "host": "77.37.160.252",
        "server_mac": "2c:9e:00:b3:4f:a2"
      }
    ]
  }
}

# Корректный конфиг
correct = {
  "format": "chiaki-settings",
  "version": 2,
  "settings": {
    "registered_hosts": [
      {
        "target": "PS5_1",
        "ap_ssid": None,
        "ap_bssid": "3132333435",
        "ap_key": None,
        "ap_name": "PS5",
        "server_mac": "2c:9e:00:b3:4f:a2",
        "server_nickname": "PS5-855",
        "rp_regist_key": "NmIxOGVmYzIAAAAAAAAAAA==",
        "rp_key_type": 2,
        "rp_key": "g7IQ3zOX7htZCvd0mDYHNg=="
      }
    ],
    "manual_hosts": [
      {
        "host": "77.37.160.252",
        "server_mac": "2c:9e:00:b3:4f:a2"
      }
    ]
  }
}

print("=== Сравнение конфигов ===\n")

incorrect_host = incorrect['settings']['registered_hosts'][0]
correct_host = correct['settings']['registered_hosts'][0]

print("Порядок полей в некорректном:")
print(list(incorrect_host.keys()))
print("\nПорядок полей в корректном:")
print(list(correct_host.keys()))

print("\n=== Декодирование ключей ===\n")

print("НЕкорректный конфиг:")
print(f"  rp_regist_key (base64): {incorrect_host['rp_regist_key']}")
rp_reg_incorrect = base64.b64decode(incorrect_host['rp_regist_key'])
print(f"  rp_regist_key (hex): {rp_reg_incorrect.hex()}")
print(f"  rp_regist_key (as string): {rp_reg_incorrect[:8]}")
print(f"  rp_key (base64): {incorrect_host['rp_key']}")
rp_key_incorrect = base64.b64decode(incorrect_host['rp_key'])
print(f"  rp_key (hex): {rp_key_incorrect.hex()}")
print(f"  rp_key (length): {len(rp_key_incorrect)}")

print("\nКорректный конфиг:")
print(f"  rp_regist_key (base64): {correct_host['rp_regist_key']}")
rp_reg_correct = base64.b64decode(correct_host['rp_regist_key'])
print(f"  rp_regist_key (hex): {rp_reg_correct.hex()}")
print(f"  rp_regist_key (as string): {rp_reg_correct[:8]}")
print(f"  rp_key (base64): {correct_host['rp_key']}")
rp_key_correct = base64.b64decode(correct_host['rp_key'])
print(f"  rp_key (hex): {rp_key_correct.hex()}")
print(f"  rp_key (length): {len(rp_key_correct)}")

print("\n=== Формат JSON ===\n")
# Проверяем, как сериализуется JSON
import json
incorrect_json = json.dumps(incorrect, indent=2, ensure_ascii=False)
correct_json = json.dumps(correct, indent=2, ensure_ascii=False)

print("Размер некорректного JSON:", len(incorrect_json), "символов")
print("Размер корректного JSON:", len(correct_json), "символов")

print("\n=== Структурные различия ===\n")
# Сравниваем структуру (без учета ключей)
incorrect_structure = {k: type(v).__name__ for k, v in incorrect_host.items() if k not in ['rp_key', 'rp_regist_key']}
correct_structure = {k: type(v).__name__ for k, v in correct_host.items() if k not in ['rp_key', 'rp_regist_key']}

print("Структура некорректного (без ключей):", incorrect_structure)
print("Структура корректного (без ключей):", correct_structure)
print("Структуры совпадают:", incorrect_structure == correct_structure)

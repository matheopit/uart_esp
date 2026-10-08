| Supported Targets | ESP32-C6 |
| ----------------- | -------- |

ESP32-C6 — contrôleur BLE HCI sur UART
======================================

Kit de développement : l'ESP32-C6 sert de contrôleur Bluetooth Low Energy à un PC
ou une VM Linux, par un câble FTDI (UART H4, 3 Mbit/s, RTS/CTS). Il est vu par
Linux comme un adaptateur Bluetooth `hciX` grâce à `btattach`.

Le firmware fait le pont entre l'UART et le contrôleur BLE (VHCI), et masque
l'advertising étendu et périodique du BLE 5. Le noyau Linux utilise ainsi les
commandes d'advertising legacy, ce qui permet à Qt Bluetooth de faire de
l'advertising.

Basé sur l'exemple ESP-IDF `controller_hci_uart_esp32c3_and_esp32s3`. La version
d'origine pour ESP32-C3 et ESP32-S3 est conservée dans la branche `esp32c3-esp32s3`.

## Démarrage rapide

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor          # port USB de la carte
```

Câblage avec le câble FTDI TTL-232R-3V3 : GPIO4 (TX) → jaune, GPIO5 (RX) → orange,
GPIO6 (RTS) → marron, GPIO7 (CTS) → vert, GND → noir. Le fil rouge (5 V) ne se
branche pas.

Sous Linux :

```bash
echo 1 | sudo tee /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
sudo btattach -B /dev/ttyUSB0 -P h4 -S 3000000
```

## Documentation

- [docs/ESP32C6_HCI_BLE.md](docs/ESP32C6_HCI_BLE.md) : architecture, câblage,
  configuration, filtre BLE 5 et Qt, comparaison avec le module Laird de l'i.MX,
  risques pour la pédale BLE, tests
- [HISTORIQUE.md](HISTORIQUE.md) : déroulé, mesures et points restant à vérifier
- [tools/](tools/) : scripts de test HCI (pyserial)

# Historique — portage ESP32-C6 du contrôleur BLE HCI UART

Projet d'origine : exemple ESP-IDF `controller_hci_uart_esp32c3_and_esp32s3`.
ESP-IDF utilisé : v6.1 (`~/esp/esp-idf`).

## 07/10/2026 — Portage vers l'ESP32-C6

Sur l'ESP32-C6, le contrôleur BLE intègre son propre transport HCI UART, configuré
par Kconfig. L'API `hci_tl_funcs` / `esp_bt_h4tl_eif_io_event_notify` utilisée
pour C3/S3 n'existe pas sur cette puce.

- **`sdkconfig.defaults.esp32c6`** (nouveau) : HCI sur UART1, mode sans DMA.
- **`main/main.c`** : quand `CONFIG_BT_LE_HCI_INTERFACE_USE_UART` est défini, un
  `app_main` simplifié initialise NVS puis le contrôleur et affiche les broches et
  le débit. Le code UHCI pour C3/S3 est conservé dans le `#else`.
- **`README.md`** : ESP32-C6 ajouté aux cibles supportées.
- `idf.py set-target esp32c6` : le `sdkconfig` est régénéré (l'ancien visait
  l'esp32).

Les options *Example Configuration* (`EXAMPLE_HCI_UART_*`) ne s'appliquaient alors
qu'au C3/S3. Sur C6, les réglages étaient dans *Component config → Bluetooth →
Controller Options → HCI Config* (remplacé ensuite par le pont VHCI, voir plus bas).

### Choix du mode sans DMA

Le mode UHCI (DMA) est marqué *EXPERIMENTAL* sur C6, et c'est le mode sans DMA
qu'Espressif utilise dans son exemple `examples/bluetooth/nimble/hci`. Le DMA reste
activable si besoin : `CONFIG_BT_LE_UART_HCI_DMA_MODE=y`.

### Tampons agrandis

| Option | Défaut | Valeur |
|---|---|---|
| `BT_LE_HCI_UART_RX_BUFFER_SIZE` | 512 | 2048 |
| `BT_LE_HCI_UART_TX_BUFFER_SIZE` | 256 | 2048 |
| `BT_LE_HCI_EVT_LO_BUF_COUNT` | 8 | 16 |

## 08/10/2026 — Câblage, flash et tests

### Câblage (câble FTDI TTL-232R-3V3)

| ESP32-C6 (sérigraphie) | GPIO | Rôle ESP | Fil FTDI |
|---|---|---|---|
| A1 | GPIO4 | TX | jaune (RX) |
| A3 | GPIO5 | RX | orange (TX) |
| IO6 | GPIO6 | RTS | marron (CTS) |
| IO7 | GPIO7 | CTS | vert (RTS) |
| GND | – | – | noir |

- **Le VCC (fil rouge, 5 V) n'est pas branché** : la carte est alimentée par son
  propre port USB.
- Les GPIO4 à GPIO7 sont aussi les broches JTAG : le débogage JTAG externe n'est
  plus possible, l'USB-JTAG intégré reste utilisable.

### Environnement

- `.vscode/settings.json` : `idf.flashType` passe de `DFU` à `UART` (le DFU n'est
  pas supporté sur C6), et la config OpenOCD passe à `board/esp32c6-builtin.cfg`.
- WSL2 : les ports USB sont transférés avec `usbipd attach --wsl --busid <id>`.
  Le noyau WSL2 n'a pas le pilote HCI UART (`CONFIG_BT_HCIUART`), donc `btattach`
  ne fonctionne que sous un vrai Linux.

### Débit

- Premier test à 115200 bauds : HCI Reset → `04 0e 04 01 03 0c 00`. Adresse
  Bluetooth du contrôleur : `58:E6:C5:E4:75:1A`.
- Passage à **3 000 000 bauds**. C'est le maximum du câble FTDI (puce FT232R) ;
  l'UART du C6 monte jusqu'à 5 Mbit/s.
- Test de charge à 3 Mbit/s avec RTS/CTS : 1000 commandes sur 1000 correctes,
  environ 1050 commandes/s, aller-retour moyen de 0,95 ms.
- Le contrôleur annonce `Num_HCI_Command_Packets = 1` : il n'accepte qu'une
  commande à la fois tant qu'il n'a pas répondu.
- Le `latency_timer` du FTDI doit être réglé à 1 ms. Avec la valeur par défaut
  (16 ms), on tombe à environ 66 commandes/s.
- Non vérifié : le contrôle de flux RTS/CTS sous forte charge, et le débit réel des
  données vers un appareil BLE connecté.

### Utilisation sous Linux

```bash
echo 1 | sudo tee /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
sudo btattach -B /dev/ttyUSB0 -P h4 -S 3000000
bluetoothctl   # select 58:E6:C5:E4:75:1A, power on, scan le
```

Les outils BlueZ fonctionnent. L'ESP32-C6 ne fait que du BLE (pas de Bluetooth
classique).

## 08/10/2026 — Advertising avec Qt Bluetooth

**Problème :** l'appli Qt renvoie `AdvertisingError`, et `btmon` montre
`Command Disallowed (0x0c)`.

**Cause :** sous Linux, Qt envoie les commandes d'advertising legacy (`LE Set
Advertising Parameters/Data/Enable`) directement par un socket HCI brut. Comme le
contrôleur annonce l'advertising étendu, le noyau et BlueZ utilisent les commandes
étendues. Or la norme interdit de mélanger les deux : le contrôleur rejette donc
les commandes legacy.

**Première tentative, sans effet :** `CONFIG_BT_LE_EXT_ADV=n`. Le problème restait
identique. Sur C6, cette option n'est pas transmise au contrôleur (seules la taille
et le nombre d'instances d'advertising étendu le sont). La liste des fonctions
annoncées est figée dans la bibliothèque binaire `libble_app.a`, et aucune API ne
permet de la modifier. La ligne a donc été retirée.

**Correctif à appliquer côté Qt :** passer par le backend BlueZ D-Bus
(`LEAdvertisingManager1`). C'est alors BlueZ qui envoie les commandes d'advertising,
et il utilise les bonnes.
- Qt 6.7 et plus : c'est le backend par défaut. Il faut que la variable
  `QT_BLUETOOTH_USE_KERNEL_PERIPHERAL` ne soit pas définie.
- Qt 6.5 / 6.6 : lancer l'appli avec `QT_BLUETOOTH_USE_DBUS_PERIPHERAL=1`.
- Qt 6.4 et avant : pas de backend D-Bus. Il faut mettre Qt à jour.
- BlueZ 5.56 minimum (`bluetoothd --version`).
- Avec ce backend, `CAP_NET_ADMIN` et sudo ne sont plus nécessaires.

**Constat :** le même code Qt fonctionne sur une cible i.MX avec un module Laird.
La puce de ce module n'annonce probablement pas l'advertising étendu : le noyau y
utilise donc lui aussi les commandes legacy, et il n'y a pas de conflit.

## 08/10/2026 — Firmware : pont VHCI ↔ UART avec filtrage

Pour que l'ESP32-C6 se comporte comme le module Laird, sans rien changer à l'appli
Qt ni au PC :

- Le contrôleur passe en mode **VHCI** (`CONFIG_BT_LE_HCI_INTERFACE_USE_RAM=y`).
  C'est `main/main.c` qui gère l'UART1 et fait le lien entre l'UART et le
  contrôleur :
  - **UART → contrôleur :** les paquets H4 sont découpés (commandes 0x01, ACL 0x02)
    puis transmis par `esp_vhci_host_send_packet`.
  - **Contrôleur → UART :** le callback `notify_host_recv` écrit les paquets sur
    l'UART.
- **Filtre :** dans la réponse `Command Complete` à `LE Read Local Supported
  Features` (0x2003), les bits 12 (*LE Extended Advertising*) et 13 (*LE Periodic
  Advertising*) sont mis à 0. Le noyau Linux n'utilise alors que les commandes
  legacy d'advertising, de scan et de connexion, ce qui supprime le conflit avec Qt.
- Le débit et le contrôle de flux se règlent maintenant dans *Example
  Configuration* (`EXAMPLE_HCI_UART_BAUDRATE`, plage étendue jusqu'à 3 000 000).
  Les broches restent 4 / 5 / 6 / 7.
- Limites : les paquets ISO (0x05) ne sont pas transmis (pas de LE Audio). Les
  paquets de plus de 1024 octets de données sont ignorés.
- Flashé : la carte démarre et affiche « LE Extended/Periodic Advertising hidden
  from the host ».
- **Tests par le FTDI à 3 Mbit/s, avec RTS/CTS :**
  - `LE Read Local Supported Features` = `0x00001b900f014fff`. Les bits 12 et 13
    sont masqués ; le 2M PHY reste annoncé.
  - Séquence d'advertising legacy, telle que Qt l'envoie (Set Advertising
    Parameters, Set Advertising Data, Advertising Enable 1/0) : tout répond
    `Success`.
  - Charge : 1000 commandes sur 1000 correctes, environ 1070 commandes/s,
    aller-retour moyen de 0,93 ms (identique au transport UART intégré).
  - Scan BLE passif de 10 s : 180 rapports d'advertising, aucun octet hors trame.
    L'environnement contenait peu d'appareils BLE : ce test ne pousse donc pas le
    débit à sa limite.
- **À tester :** l'appli Qt sous Linux, après relance de `btattach`.

## Configuration C6 actuelle (`sdkconfig.defaults.esp32c6`)

```
CONFIG_BT_LE_HCI_INTERFACE_USE_RAM=y
CONFIG_EXAMPLE_HCI_UART_BAUDRATE=3000000
CONFIG_EXAMPLE_HCI_UART_FLOW_CTRL_ENABLE=y
CONFIG_BT_LE_HCI_EVT_LO_BUF_COUNT=16
```

## 08/10/2026 — Projet réservé à l'ESP32-C6

- Le code d'origine pour C3/S3 (transport UHCI) est conservé dans la branche
  `esp32c3-esp32s3`, créée à partir de `main`, c'est-à-dire de l'exemple ESP-IDF
  d'origine.
- Dans les branches C6, le code C3/S3 est retiré de `main/main.c`, ainsi que la
  dépendance `esp_ringbuf`. `sdkconfig.defaults` fixe maintenant la cible
  `esp32c6`, et un `#error` empêche de compiler sans le mode VHCI. Le README est
  réécrit pour le C6.

## 22/10/2026 — Publication

- Tableau des branches ajouté au README.
- Dépôt publié sur https://github.com/matheopit/uart_esp (toutes les branches).

# ESP32-C6 comme contrôleur BLE HCI pour le développement sous Linux

Ce document explique pourquoi et comment un ESP32-C6 sert de contrôleur Bluetooth
Low Energy (BLE) aux machines de développement Linux. Il couvre les choix techniques,
les différences avec la cible i.MX + Laird et les risques à connaître pour le
développement de l'application de pédale BLE.

Le déroulé chronologique et les mesures se trouvent dans [HISTORIQUE.md](../HISTORIQUE.md).

---

## 1. Contexte et architecture

Les machines de développement sont des VM Linux sous VMware. Partager le Bluetooth
de l'hôte, ou un dongle USB Bluetooth en passthrough, n'y fonctionne pas de façon
fiable. En revanche, un **port série USB (câble FTDI) passe sans problème** dans la
VM. Un ESP32-C6 est donc utilisé comme contrôleur BLE externe : il est vu par Linux
comme un adaptateur Bluetooth (`hciX`) branché sur un port série.

```
 Application Qt ── BlueZ (bluetoothd) ── noyau Linux (hci_uart, btattach)
                                              │
                               /dev/ttyUSB0 (FTDI, passthrough USB VMware)
                                              │  UART H4, 3 Mbit/s, RTS/CTS
                                     ┌────────┴────────────────────────┐
                                     │ ESP32-C6                        │
                                     │  main.c : pont UART ⇄ VHCI      │
                                     │  + filtre BLE 5 (option)        │
                                     │  contrôleur BLE (libble)        │
                                     └────────┬────────────────────────┘
                                              │ radio 2,4 GHz
                                         pédale BLE
```

Sur la cible de production (i.MX), le contrôleur est un module **Laird** relié au
même type de pile : noyau, BlueZ et Qt.

---

## 2. Matériel et câblage

Câble FTDI **TTL-232R-3V3** : signaux en 3,3 V, obligatoire car les GPIO du C6 ne
tolèrent pas le 5 V.

| Carte ESP32-C6 | GPIO | Rôle ESP | Fil FTDI |
|---|---|---|---|
| A1 | GPIO4 | TX | jaune (RX) |
| A3 | GPIO5 | RX | orange (TX) |
| IO6 | GPIO6 | RTS | marron (CTS) |
| IO7 | GPIO7 | CTS | vert (RTS) |
| GND | – | – | noir |

- **Le fil rouge (VCC) du câble FTDI ne se branche pas.** Il fournit du 5 V, même sur
  la version 3V3. La carte est alimentée par son propre port USB.
- **RTS/CTS sont indispensables** à 3 Mbit/s. Sans contrôle de flux, le premier
  octet perdu désynchronise le flux H4.
- Les GPIO4 à GPIO7 sont aussi les broches JTAG : le débogage JTAG externe n'est
  plus possible, mais l'USB-JTAG intégré reste utilisable.
- La sérigraphie de la carte mélange des noms Arduino (A1, A3…) et des numéros de
  GPIO (IO6…). Seuls les numéros de GPIO comptent pour le firmware.

---

## 3. Firmware

### Compilation et flash

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # port USB de la carte, pas le FTDI
```

La configuration fait référence dans [`sdkconfig.defaults.esp32c6`](../sdkconfig.defaults.esp32c6) :

| Option | Valeur | Rôle |
|---|---|---|
| `CONFIG_BT_LE_HCI_INTERFACE_USE_RAM` | `y` | Contrôleur en mode VHCI : c'est `main.c` qui gère l'UART |
| `CONFIG_EXAMPLE_HCI_UART_BAUDRATE` | `3000000` | Débit, plafonné par le câble FTDI (FT232R : 3 Mbit/s max) |
| `CONFIG_EXAMPLE_HCI_UART_FLOW_CTRL_ENABLE` | `y` | Contrôle de flux RTS/CTS |
| `CONFIG_BT_LE_HCI_EVT_LO_BUF_COUNT` | `16` | Moins de rapports d'advertising perdus pendant les scans |
| `CONFIG_EXAMPLE_HCI_HIDE_BLE5_ADV` | `n` | Filtre BLE 5 (section 5). À activer uniquement si Qt doit faire de l'advertising |

Le débit et le contrôle de flux se règlent aussi dans `idf.py menuconfig` →
*Example Configuration*.

### Pourquoi un pont dans `main.c`

Le contrôleur du C6 sait gérer l'UART HCI tout seul, et c'était la première
configuration utilisée. Mais il faut **modifier une réponse HCI au passage**
(voir section 5). Le contrôleur tourne donc en mode VHCI, et `main.c` fait le lien :

- **UART → contrôleur :** les paquets H4 sont découpés (commandes `0x01`, ACL
  `0x02`), puis transmis par `esp_vhci_host_send_packet()`.
- **Contrôleur → UART :** le callback `notify_host_recv` filtre les paquets, puis les
  écrit sur l'UART.

Le mode UART sans DMA aurait suffi en débit (le mode DMA est marqué expérimental
sur C6). Le pont donne les mêmes performances que le transport intégré.

---

## 4. Utilisation sous Linux

```bash
sudo apt install bluez
echo 1 | sudo tee /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
sudo btattach -B /dev/ttyUSB0 -P h4 -S 3000000      # laisser tourner
bluetoothctl list                                    # 58:E6:C5:E4:75:1A
```

- **`latency_timer` :** par défaut, le pilote FTDI attend jusqu'à 16 ms avant de
  transmettre ce qu'il a reçu. À 1 ms, on passe d'environ 66 à plus de 1000
  commandes HCI/s. Ce réglage revient à 16 ms à chaque branchement.
- **Relancer `btattach` après chaque reflash ou reset de l'ESP.** Le noyau ne lit les
  fonctions du contrôleur qu'à l'attachement.
- **WSL2 ne convient pas :** son noyau n'a pas le pilote `hci_uart`. Il faut une VM ou
  une machine Linux classique.
- **Pour le diagnostic,** `sudo btmon` affiche toutes les commandes et événements HCI.

---

## 5. Le problème Qt « Command Disallowed » et le filtre BLE 5

### Advertising legacy et advertising étendu

L'*advertising* est le message qu'un appareil BLE diffuse en boucle pour signaler sa
présence : nom, services, données. Il en existe deux générations, chacune avec ses
propres commandes HCI.

| | Legacy (BLE 4.x) | Étendu (BLE 5) |
|---|---|---|
| Données | 31 octets (plus 31 en réponse au scan) | jusqu'à ~1650 octets |
| Canaux | 3 canaux d'advertising (37, 38, 39) | pointeur sur les 3 canaux, données sur un des 37 autres |
| PHY | 1M uniquement | 1M, 2M, Coded (longue portée, environ ×4) |
| Annonces simultanées | 1 | plusieurs « sets » indépendants |
| Advertising périodique | non | oui (base du LE Audio broadcast / Auracast) |
| Commandes HCI | `LE Set Advertising Parameters/Data/Enable` | `LE Set Extended Advertising …` |

**La spécification Bluetooth interdit de mélanger les deux jeux de commandes** sur un
même contrôleur. Une fois qu'une commande étendue a été reçue, toute commande legacy
est rejetée avec `Command Disallowed (0x0C)`, et inversement.

### Ce qui se passait

1. Le C6 annonce, dans `LE Read Local Supported Features`, qu'il gère l'advertising
   étendu (bit 12).
2. Le noyau Linux le voit et utilise donc **les commandes étendues** (scan, création
   de connexion, advertising de BlueZ).
3. Sous Linux, Qt Bluetooth, avec son backend noyau, envoie l'advertising
   **directement par un socket HCI brut, avec les commandes legacy**.
4. Le contrôleur reçoit les deux jeux de commandes, rejette celles de Qt, et Qt
   remonte `AdvertisingError`.

### Pourquoi ça marche sur l'i.MX avec le module Laird

Le même code Qt fonctionne sur la cible i.MX avec un module Laird. L'explication la
plus probable est que **la puce Bluetooth du module Laird n'annonce pas l'advertising
étendu**. Le noyau utilise alors lui aussi les commandes legacy, et il n'y a aucun
conflit.

Autre explication possible : l'image Yocto de l'i.MX a un noyau, un BlueZ ou un Qt
plus anciens, qui n'emploient jamais les commandes étendues.

**À confirmer** sur l'i.MX avec `sudo btmon` au démarrage du Bluetooth. Si « LE
Extended Advertising » n'apparaît pas dans `LE Read Local Supported Features`, c'est
la première explication.

### Pourquoi la correction est dans le firmware et pas dans la config

- `CONFIG_BT_LE_EXT_ADV=n` **n'a aucun effet** sur ce que le C6 annonce. Sur C6,
  cette option n'est pas transmise au contrôleur : seules la taille et le nombre
  d'instances le sont. La liste des fonctions annoncées est figée dans la
  bibliothèque binaire `libble_app.a`, et aucune API ne permet de la modifier.
- Il reste donc deux leviers :
  - **Côté Qt :** utiliser le backend BlueZ D-Bus (`LEAdvertisingManager1`). C'est
    le mode par défaut à partir de Qt 6.7, et il s'active avec
    `QT_BLUETOOTH_USE_DBUS_PERIPHERAL=1` en 6.5 et 6.6. Il faut BlueZ 5.56 ou plus.
    Avec ce backend, BlueZ choisit lui-même les bonnes commandes. Mais il faut
    maîtriser la version de Qt, et le comportement diffère de la cible.
  - **Côté firmware :** faire passer le C6 pour un contrôleur comme le Laird.
    C'est le filtre ci-dessous, activable dans menuconfig.

### Le filtre (option `CONFIG_EXAMPLE_HCI_HIDE_BLE5_ADV`, désactivée par défaut)

*menuconfig → Example Configuration → Hide LE Extended/Periodic Advertising from
the host*

Dans `main.c`, `hci_filter_controller_pkt()` repère l'événement `Command Complete` de
`LE Read Local Supported Features` (opcode `0x2003`). Il y met à 0 :

- le bit 12, *LE Extended Advertising* ;
- le bit 13, *LE Periodic Advertising*.

Le noyau Linux se limite alors aux commandes legacy pour l'advertising, le scan et
la connexion, comme avec le Laird. L'application Qt n'a rien à changer.

**Ce qu'on perd :** les annonces de plus de 31 octets, l'advertising en Coded PHY
(longue portée) et l'advertising périodique. Surtout, **le PC ne voit plus les
appareils qui n'émettent qu'en advertising étendu.** **Ce qu'on garde :** le 2M PHY
et les paquets longs (DLE) une fois connecté, ainsi que le chiffrement et
l'appairage.

### Quand activer le filtre

| Rôle de l'application Qt sous Linux | Filtre |
|---|---|
| **Central** : scanne et se connecte à un périphérique (cas de l'appli pédale) | **désactivé**. Qt passe par BlueZ en D-Bus, sans socket HCI brut, donc sans conflit. Tous les appareils sont visibles. |
| **Périphérique** : fait de l'advertising (`QLowEnergyController::startAdvertising`) avec le backend noyau de Qt | **activé**, ou bien utiliser le backend BlueZ D-Bus de Qt 6.5+ |

Le filtre a d'abord été activé en permanence (tests d'advertising Qt). Il cachait
des appareils BLE 5 lors des scans : c'est pourquoi il est devenu une option.

`tools/hci_scan_compare.py` compare un scan legacy et un scan étendu, et liste les
appareils qui seraient invisibles avec le filtre.

---

## 6. Risques pour le développement de la pédale BLE

### 6.1 La pédale fait de l'advertising étendu (seulement si le filtre est activé)

Avec le filtre activé, le PC ne scanne plus qu'en legacy. **Si la pédale n'émet
que de l'advertising étendu** (Coded PHY, ou annonces de plus de 31 octets),
**elle sera invisible.** Le filtre est désactivé par défaut : l'appli pédale,
qui ne fait que se connecter, n'en a pas besoin.

À vérifier une fois avec *nRF Connect* sur un téléphone : il indique « Legacy » ou
« Extended » pour chaque annonce.

### 6.2 Latence

Pour une pédale, c'est le point le plus sensible. Chaque appui traverse :
pédale → radio → contrôleur → UART → FTDI → USB VMware → noyau → BlueZ → D-Bus →
Qt.

| Étape | Ordre de grandeur | Remarque |
|---|---|---|
| Intervalle de connexion BLE | 7,5 à 50 ms ou plus | négocié avec la pédale, c'est souvent le poste principal |
| FTDI `latency_timer` | 1 ms ou 16 ms | revient à 16 ms à chaque branchement (voir la branche `esp32c6-robustesse`) |
| Passthrough USB VMware | quelques ms, avec gigue | absent sur la cible |
| UART 3 Mbit/s | < 0,1 ms | négligeable |

**Les mesures faites dans la VM ne valent pas pour l'i.MX.** La latence doit être
validée sur la cible.

### 6.3 Pédale HID captée par BlueZ

Si la pédale se présente comme un clavier BLE (profil HID, service `0x1812`), BlueZ
la prend lui-même en charge (plugin `input`/HOGP) et crée un clavier Linux
(`/dev/input/eventX`). **Qt ne peut alors pas lire ses caractéristiques GATT.** Il
faut :

- soit lire les touches comme un clavier (evdev) ;
- soit désactiver le plugin `input` de BlueZ (`bluetoothd -P input`) et gérer le GATT
  dans Qt.

Une pédale avec un service GATT propriétaire n'a pas ce problème. Ce point ne dépend
pas du C6 : il se posera aussi sur l'i.MX.

### 6.4 Arrêt de `btattach` pendant une connexion

Si Linux ne lit plus le port (`btattach` arrêté, VM en pause, câble FTDI débranché),
le contrôle de flux bloque l'UART. Dans la version de base, l'écriture vers l'UART se
fait alors **dans la tâche du contrôleur BLE, qui reste bloquée** : connexion perdue,
et parfois un reset de l'ESP est nécessaire. Corrigé dans la branche
`esp32c6-robustesse`.

### 6.5 Reset de l'ESP pendant que Linux est attaché

Une coupure, une chute de tension ou un reflash remet le contrôleur à zéro, mais le
noyau ne le sait pas : l'adaptateur `hciX` ne répond plus correctement. Il faut
relancer `btattach`. Corrigé dans la branche `esp32c6-robustesse` (voir section 8).

### 6.6 Limites de configuration

| Limite | Valeur | Impact |
|---|---|---|
| Connexions BLE simultanées | 3 (`CONFIG_BT_LE_MAX_CONNECTIONS`) | suffisant pour une pédale, à augmenter pour plusieurs |
| Liste de résolution (adresses privées) | 4 (`CONFIG_BT_LE_LL_RESOLV_LIST_SIZE`) | au-delà de 4 appareils appairés avec adresse privée, la résolution passe par le noyau |
| Paquets ISO (LE Audio) | non transmis par le pont | sans objet pour une pédale |
| Paquets de plus de 1024 octets de données | ignorés | sans objet (tampon ACL du contrôleur : 517 octets) |

### 6.7 Écarts avec la cible i.MX + Laird

La pile est la même (noyau, BlueZ, Qt), mais le contrôleur est différent : MTU,
paramètres de connexion acceptés, comportement à l'appairage et à la reconnexion,
fonctions BLE 5 disponibles. **Ce qui marche avec le C6 doit être revalidé sur la
cible**, en particulier l'appairage, la reconnexion automatique et la latence.

---

## 7. Tests

Les scripts sont dans [`tools/`](../tools). Ils utilisent `pyserial`, déjà présent dans
l'environnement Python d'ESP-IDF.

```bash
python tools/hci_bridge_test.py /dev/ttyUSB0 3000000
python tools/hci_scan_test.py /dev/ttyUSB0 3000000
```

`btattach` doit être arrêté, car le port série ne peut pas être ouvert deux fois.

Résultats du 08/10/2026 (3 Mbit/s, RTS/CTS) :

| Test | Résultat |
|---|---|
| `LE Read Local Supported Features` | `0x00001b900f014fff` : bits 12 et 13 masqués, 2M PHY présent |
| Advertising legacy (Parameters, Data, Enable 1/0) | tout répond `Success` |
| 1000 commandes HCI | 1000/1000, ~1070 cmd/s, aller-retour moyen de 0,93 ms |
| Scan passif de 10 s | 180 rapports, aucune erreur de trame (peu d'appareils autour) |

Non testé : le débit des données (ACL) vers un appareil connecté, et le contrôle de
flux sous très forte charge.

---

## 8. Branche `esp32c6-robustesse`

Corrections des points 6.2, 6.4 et 6.5, développées à part :

- **Écriture UART non bloquante :** si le tampon d'envoi UART reste plein plus de
  50 ms, le paquet du contrôleur est abandonné (et compté) au lieu de bloquer la
  tâche BLE.
- **Signalement d'un reset au démarrage :** si Linux a le port ouvert (CTS actif), le
  firmware envoie un événement HCI *Hardware Error*. Le noyau réinitialise alors
  automatiquement l'adaptateur, sans relance manuelle de `btattach`.
- **`tools/linux/`** :
  - règle udev qui fixe le `latency_timer` FTDI à 1 ms, crée le lien
    `/dev/esp32c6-hci` et démarre le service ;
  - service systemd qui lance `btattach` au branchement et le relance en cas d'arrêt.

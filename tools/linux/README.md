# Intégration Linux du contrôleur ESP32-C6

Au branchement du câble FTDI, ces deux fichiers font automatiquement ce qui se
faisait à la main :
- fixer le `latency_timer` FTDI à 1 ms ;
- créer le lien `/dev/esp32c6-hci` ;
- lancer `btattach`, puis le relancer s'il s'arrête.

## Installation

```bash
sudo cp 99-esp32c6-hci.rules /etc/udev/rules.d/
sudo cp esp32c6-hci.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
# débrancher / rebrancher le câble FTDI
```

## Vérification

```bash
ls -l /dev/esp32c6-hci
cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer   # 1
systemctl status esp32c6-hci
bluetoothctl list
```

## Notes

- Vérifiez le chemin de `btattach` avec `command -v btattach`. Il vaut
  `/usr/bin/btattach` sur Ubuntu et Debian ; adaptez `ExecStart` si besoin.
- Les règles visent tous les câbles FTDI 0403:6001. Pour ne cibler qu'un seul câble,
  ajoutez `ATTRS{serial}=="…"`.
- **Reset de l'ESP pendant que `btattach` tourne :** au démarrage, le firmware
  détecte que le port est ouvert (CTS actif) et envoie un événement HCI *Hardware
  Error*. Le noyau réinitialise alors l'adaptateur tout seul.
- Pour lancer les scripts de `tools/` : `sudo systemctl stop esp32c6-hci`, car le
  port ne peut pas être ouvert deux fois.

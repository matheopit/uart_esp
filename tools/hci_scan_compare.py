"""Compare un scan legacy (BLE 4) et un scan étendu (BLE 5) sur l'ESP32-C6.

Le pont masque l'advertising étendu au noyau Linux, qui ne scanne alors qu'en
legacy. Ce script interroge directement le contrôleur pour lister les appareils
qui ne sont visibles qu'en scan étendu, donc invisibles depuis Linux.

Arrêter btattach avant (le port ne peut pas être ouvert deux fois).
Usage : python tools/hci_scan_compare.py [/dev/ttyUSB0] [3000000] [secondes]
"""
import serial, struct, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 3000000
duration = float(sys.argv[3]) if len(sys.argv) > 3 else 10
s = serial.Serial(port, baud, rtscts=True, timeout=0.2)
try:
    s.set_low_latency_mode(True)
except Exception:
    pass

PHY_NAMES = {0: '-', 1: '1M', 2: '2M', 3: 'Coded'}


def read_event():
    b = s.read(1)
    if not b or b[0] != 0x04:
        return None, None
    h = s.read(2)
    if len(h) < 2:
        return None, None
    return h[0], s.read(h[1])


def cmd(op, params=b''):
    s.write(struct.pack('<BHB', 1, op, len(params)) + params)
    t = time.time()
    while time.time() - t < 2:
        code, body = read_event()
        if code == 0x0E and struct.unpack('<H', body[1:3])[0] == op:
            if body[3] != 0:
                print('  commande 0x%04x : statut 0x%02x' % (op, body[3]))
            return body[3]
    print('  commande 0x%04x : pas de reponse' % op)
    return None


def ad_name(data):
    i = 0
    while i + 1 < len(data):
        ln = data[i]
        if ln == 0 or i + 1 + ln > len(data):
            break
        if data[i + 1] in (0x08, 0x09):
            return data[i + 2:i + 1 + ln].decode(errors='replace')
        i += 1 + ln
    return ''


def addr_str(a):
    return ':'.join('%02X' % x for x in a[::-1])


def setup():
    cmd(0x0C03)                                      # Reset (efface l'état legacy/étendu)
    cmd(0x0C01, bytes.fromhex('ffffffffffffffff'))   # Set Event Mask
    cmd(0x2001, bytes.fromhex('ffff0f0000000000'))   # LE Set Event Mask


def collect(stop_op, stop_params):
    seen = {}
    t = time.time()
    while time.time() - t < duration:
        code, body = read_event()
        if code != 0x3E or not body:
            continue
        if body[0] == 0x02 and body[1] == 1:         # LE Advertising Report (1 rapport)
            addr = addr_str(body[4:10])
            dlen = body[10]
            data = body[11:11 + dlen]
            d = seen.setdefault(addr, {'name': '', 'rssi': -127, 'info': 'legacy'})
            d['name'] = d['name'] or ad_name(data)
            d['rssi'] = max(d['rssi'], struct.unpack('b', body[11 + dlen:12 + dlen] or b'\x81')[0])
        elif body[0] == 0x0D and body[1] == 1:       # LE Extended Advertising Report (1 rapport)
            r = body[2:]
            evt = struct.unpack('<H', r[0:2])[0]
            addr = addr_str(r[3:9])
            p1, p2 = r[9], r[10]
            rssi = struct.unpack('b', r[13:14])[0]
            dlen = r[23]
            data = r[24:24 + dlen]
            legacy = bool(evt & 0x10)
            d = seen.setdefault(addr, {'name': '', 'rssi': -127, 'info': ''})
            d['name'] = d['name'] or ad_name(data)
            d['rssi'] = max(d['rssi'], rssi)
            if not legacy:
                d['info'] = 'ETENDU %s/%s, %d o' % (PHY_NAMES.get(p1, p1), PHY_NAMES.get(p2, p2), dlen)
            elif not d['info']:
                d['info'] = 'legacy'
    cmd(stop_op, stop_params)
    return seen


print('Scan legacy (BLE 4, comme Linux avec le filtre) : %.0f s...' % duration)
setup()
cmd(0x200B, struct.pack('<BHHBB', 1, 0x60, 0x30, 0, 0))            # actif, 60 ms / 30 ms
cmd(0x200C, b'\x01\x00')
legacy = collect(0x200C, b'\x00\x00')

print('Scan etendu (BLE 5, PHY 1M + Coded) : %.0f s...' % duration)
setup()
cmd(0x2041, struct.pack('<BBB', 0, 0, 0x05) + struct.pack('<BHH', 1, 0x60, 0x30) * 2)
cmd(0x2042, struct.pack('<BBHH', 1, 0, 0, 0))
ext = collect(0x2042, struct.pack('<BBHH', 0, 0, 0, 0))
cmd(0x0C03)

only_ext = sorted(set(ext) - set(legacy), key=lambda a: -ext[a]['rssi'])
print()
print('Vus en legacy : %d   Vus en etendu : %d   Invisibles depuis Linux : %d'
      % (len(legacy), len(ext), len(only_ext)))
if only_ext:
    print()
    print('%-17s  %5s  %-28s  %s' % ('Adresse', 'RSSI', 'Advertising', 'Nom'))
    for a in only_ext:
        d = ext[a]
        print('%-17s  %5d  %-28s  %s' % (a, d['rssi'], d['info'], d['name']))
    print()
    print("'ETENDU' = n'emet qu'en advertising etendu : invisible a cause du filtre.")
    print("'legacy' = emet en legacy mais pas capte pendant le scan legacy (portee, hasard) : relancer pour confirmer.")

"""Test du pont HCI ESP32-C6 : filtre BLE 5, advertising legacy, charge.

Usage : python tools/hci_bridge_test.py [/dev/ttyUSB0] [3000000]
"""
import serial, struct, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 3000000
s = serial.Serial(port, baud, rtscts=True, timeout=1, write_timeout=2)
try:
    s.set_low_latency_mode(True)
except Exception:
    pass
s.reset_input_buffer()


def cmd(ogf, ocf, params=b''):
    op = (ogf << 10) | ocf
    s.write(struct.pack('<BHB', 1, op, len(params)) + params)
    while True:
        hdr = s.read(3)
        if len(hdr) < 3:
            return None
        body = s.read(hdr[2])
        if hdr[1] == 0x0E and struct.unpack('<H', body[1:3])[0] == op:
            return body[3:]          # status + return params
        if hdr[1] == 0x0F and struct.unpack('<H', body[2:4])[0] == op:
            return body[0:1]         # command status


def check(name, r):
    ok = r is not None and r[0] == 0
    print('%-32s %s' % (name, 'OK' if ok else 'ECHEC (%s)' % (r.hex(' ') if r else 'pas de reponse')))
    return ok


check('HCI_Reset', cmd(0x03, 0x003))
r = cmd(0x08, 0x003)
check('LE_Read_Local_Features', r)
if r:
    f = int.from_bytes(r[1:9], 'little')
    print('  features = 0x%016x' % f)
    print('  bit 12 LE Extended Advertising : %s' % ('PRESENT' if f >> 12 & 1 else 'masque'))
    print('  bit 13 LE Periodic Advertising : %s' % ('PRESENT' if f >> 13 & 1 else 'masque'))
    print('  bit  8 LE 2M PHY               : %s' % ('present' if f >> 8 & 1 else 'absent'))

# Legacy advertising sequence, as Qt sends it on a raw HCI socket
adv_params = struct.pack('<HHBBB6sBB', 0x00A0, 0x00A0, 0, 0, 0, b'\0' * 6, 0x07, 0)
check('LE_Set_Advertising_Parameters', cmd(0x08, 0x006, adv_params))
data = bytes([2, 1, 6, 5, 9]) + b'TEST'
check('LE_Set_Advertising_Data', cmd(0x08, 0x008, bytes([len(data)]) + data.ljust(31, b'\0')))
check('LE_Set_Advertising_Enable(1)', cmd(0x08, 0x00A, b'\x01'))
check('LE_Set_Advertising_Enable(0)', cmd(0x08, 0x00A, b'\x00'))

N = 1000
ok = 0
t = time.time()
for i in range(N):
    r = cmd(0x04, 0x009)             # HCI_Read_BD_ADDR
    ok += r is not None and r[0] == 0
dt = time.time() - t
print('Charge: %d/%d OK, %.0f cmd/s, aller-retour moyen %.2f ms' % (ok, N, N / dt, dt / N * 1000))

"""Scan BLE passif de 10 s à travers le pont HCI ESP32-C6.

Usage : python tools/hci_scan_test.py [/dev/ttyUSB0] [3000000]
"""
import serial, struct, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 3000000
s = serial.Serial(port, baud, rtscts=True, timeout=1)
try:
    s.set_low_latency_mode(True)
except Exception:
    pass


def send(op, p=b''):
    s.write(struct.pack('<BHB', 1, op, len(p)) + p)
    time.sleep(0.05)


send(0x0C03)                                              # Reset
send(0x0C01, bytes.fromhex('ffffffffffffffff'))           # Set Event Mask (all)
send(0x2001, bytes.fromhex('ff00000000000000'))           # LE Set Event Mask
send(0x200B, struct.pack('<BHHBB', 0, 0x10, 0x10, 0, 0))  # passive scan, 10ms/10ms
send(0x200C, b'\x01\x00')                                 # scan enable, no dup filter
s.reset_input_buffer()
reports = bad = nbytes = 0
t = time.time()
while time.time() - t < 10:
    b = s.read(1)
    if not b:
        continue
    if b[0] != 4:
        bad += 1
        continue
    h = s.read(2)
    body = s.read(h[1])
    nbytes += 3 + h[1]
    if h[0] == 0x3E and body[:1] == b'\x02':
        reports += body[1]
send(0x200C, b'\x00\x00')
print('10 s de scan : %d rapports, %d octets (%.0f o/s), %d octets hors trame' % (reports, nbytes, nbytes / 10, bad))

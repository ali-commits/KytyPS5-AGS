#!/usr/bin/env python3
"""Capture a Bluetooth DualSense fixture: descriptor, feature reports 0x05/0x09/0x20, ~2 s of input."""
import argparse, fcntl, json, os, select, sys, time
from pathlib import Path

def hidiocgfeature(length):           # _IOC(_IOC_READ|_IOC_WRITE, 'H', 0x07, len)
    return (3 << 30) | (length << 16) | (ord('H') << 8) | 0x07

def find():
    for node in sorted(Path('/sys/class/hidraw').iterdir()):
        ev = dict(l.split('=', 1) for l in (node / 'device/uevent').read_text().splitlines() if '=' in l)
        bus, vid, pid = ev.get('HID_ID', '::').split(':')
        if bus == '0005' and vid.upper() == '0000054C' and pid.upper() in ('00000CE6', '00000DF2') \
                and ev.get('HID_PHYS') != 'kyty-host-tunnel':
            return node, ev, int(vid, 16), int(pid, 16)
    sys.exit('No Bluetooth DualSense found (pair it first).')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(Path.home() / '.local/share/kyty-dev/dualsense-bt.json'))
    ap.add_argument('--seconds', type=float, default=2.0)
    a = ap.parse_args()
    node, ev, vid, pid = find()
    fd = os.open('/dev/' + node.name, os.O_RDWR)
    features = {}
    for number in (0x05, 0x09, 0x20):   # 0x05 first: reading calibration switches to enhanced mode
        buf = bytearray(64); buf[0] = number
        try:
            n = fcntl.ioctl(fd, hidiocgfeature(len(buf)), buf, True)
            features[str(number)] = bytes(buf[:n]).hex()
        except OSError as e:
            print(f'feature 0x{number:02x}: {e}', file=sys.stderr)
    reports, end = [], time.monotonic() + a.seconds
    print('Hold the controller still; move sticks gently for the last second.')
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.1)[0]:
            reports.append(os.read(fd, 1024).hex())
    os.close(fd)
    out = Path(a.out); out.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    data = {'vid': vid, 'pid': pid, 'name': ev.get('HID_NAME', ''), 'mac': ev.get('HID_UNIQ', '').lower(),
            'descriptor': (node / 'device/report_descriptor').read_bytes().hex(),
            'features': features, 'input': reports}
    out.write_text(json.dumps(data)); out.chmod(0o600)
    print(f'wrote {out}: {len(reports)} reports, features {sorted(features)}')

if __name__ == '__main__':
    main()

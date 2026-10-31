#!/usr/bin/env python3
"""Install the built TV/phone apps and provision a public phone identity locally.

Run after turning on the TV and reconnecting the phone's USB debugging connection.
This tool never launches a game, copies a private key, or changes Bluetooth pairing.
"""
import argparse
import base64
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import ssl
import stat
import subprocess
import sys
import time

PHONE_PACKAGE = 'io.kyty.controller.debug'
TV_PACKAGE = 'io.kyty.streaming.tv.debug'


def public_der(pem):
    if (not isinstance(pem, str) or len(pem.encode()) > 16384 or 'PRIVATE KEY' in pem
            or pem.count('-----BEGIN CERTIFICATE-----') != 1
            or pem.count('-----END CERTIFICATE-----') != 1):
        raise ValueError('Expected exactly one public certificate')
    return ssl.PEM_cert_to_DER_cert(pem.strip())


def private_json(path):
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_mode & 0o077 or info.st_size > 16384):
        raise ValueError('Management credentials must be an owner-only regular file')
    return json.loads(path.read_text())


def management_request(profile, port, method, route, body, *, expected_pin=None):
    """Validate exact TLS identity before sending any local management credentials."""
    profile = Path(profile)
    certificate = profile / 'host-cert.pem'
    der = public_der(certificate.read_text())
    wanted = expected_pin or hashlib.sha256(der).hexdigest()
    credentials = private_json(profile / 'controller-auth.json')
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.check_hostname = False  # Exact leaf pin below authenticates this local host.
    context.load_verify_locations(cafile=str(certificate))
    context.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
    connection = http.client.HTTPSConnection('127.0.0.1', port, context=context, timeout=5)
    try:
        connection.connect()
        actual = hashlib.sha256(connection.sock.getpeercert(binary_form=True)).hexdigest()
        if actual != wanted:
            raise ssl.SSLCertVerificationError('Local host certificate pin mismatch')
        auth = base64.b64encode((credentials['username'] + ':' + credentials['password']).encode()).decode()
        payload = json.dumps(body, allow_nan=False).encode() if body is not None else None
        connection.request(method, route, body=payload,
                           headers={'Authorization': 'Basic ' + auth,
                                    'Content-Type': 'application/json'})
        response = connection.getresponse()
        raw = response.read(65537)
        if len(raw) > 65536:
            raise ValueError('Oversized management response')
        result = json.loads(raw)
        if response.status != 200 or result.get('ok') is not True:
            raise ValueError('Phone registration rejected: ' + str(result.get('error', response.status)))
        return result
    finally:
        connection.close()


def run(arguments, *, input_bytes=None, timeout=30, required=True):
    result = subprocess.run([str(x) for x in arguments], input=input_bytes,
                            capture_output=True, timeout=timeout, check=False)
    if required and result.returncode:
        detail = (result.stderr or result.stdout).decode(errors='replace')[-400:].strip()
        raise RuntimeError('Device command failed: ' + detail)
    return result


def provision(args):
    address = ipaddress.ip_address(args.host)
    if address.version != 4 or not address.is_private or address.is_loopback:
        raise ValueError('Use the PC private LAN IPv4 address')
    if not 1024 <= args.http_port <= 65514:
        raise ValueError('Invalid GameStream base port')
    for path in (args.adb, args.phone_apk, args.tv_apk):
        if not path.is_file():
            raise ValueError('Missing setup input: ' + str(path))
    public_der((args.profile / 'host-cert.pem').read_text())
    if args.check_only:
        print('Setup inputs validated; no device or host changes made.')
        return
    # Check availability before installing either application.
    run([args.adb, '-s', args.phone, 'get-state'])
    if not args.skip_tv:
        run([args.adb, 'connect', args.tv], timeout=8)
        run([args.adb, '-s', args.tv, 'get-state'])
        run([args.adb, '-s', args.tv, 'install', '-r', '-t', args.tv_apk], timeout=120)
    run([args.adb, '-s', args.phone, 'install', '-r', '-t', args.phone_apk], timeout=120)
    run([args.adb, '-s', args.phone, 'shell', 'am', 'start', '-W', '-n',
         PHONE_PACKAGE + '/io.kyty.controller.ControllerActivity'])
    pem = None
    for _ in range(30):
        result = run([args.adb, '-s', args.phone, 'exec-out', 'run-as', PHONE_PACKAGE,
                      'cat', 'files/client-certificate.pem'], required=False, timeout=5)
        if result.returncode == 0:
            try:
                candidate = result.stdout.decode()
                public_der(candidate)
            except ValueError:
                # First start can expose the file before its public PEM write completes.
                pass
            else:
                pem = candidate
                break
        time.sleep(0.25)
    if pem is None:
        raise RuntimeError('Phone has not created a complete public certificate; reopen Kyty Controller and retry')
    registration = management_request(args.profile, args.http_port + 1, 'POST',
        '/api/kyty/companion/phones', {'name': args.name, 'certificate_pem': pem})
    pin = hashlib.sha256(public_der((args.profile / 'host-cert.pem').read_text())).hexdigest()
    configuration = f'host={address}\nport={args.http_port - 5}\ncertificate_sha256={pin}\n'.encode()
    run([args.adb, '-s', args.phone, 'shell', 'run-as', PHONE_PACKAGE,
         'tee', 'files/host.properties'], input_bytes=configuration)
    run([args.adb, '-s', args.phone, 'shell', 'run-as', PHONE_PACKAGE,
         'chmod', '600', 'files/host.properties'])
    # Reopen to refresh public endpoint configuration; this app has no automatic Play action.
    run([args.adb, '-s', args.phone, 'shell', 'am', 'force-stop', PHONE_PACKAGE])
    run([args.adb, '-s', args.phone, 'shell', 'am', 'start', '-W', '-n',
         PHONE_PACKAGE + '/io.kyty.controller.ControllerActivity'])
    if not args.skip_tv:
        run([args.adb, '-s', args.tv, 'shell', 'monkey', '-p', TV_PACKAGE,
             '-c', 'android.intent.category.LEANBACK_LAUNCHER', '1'])
    print('Apps installed and phone public identity registered. No game launched.')
    print('Open Kyty TV, then choose its TV and a game in Kyty Controller on the phone.')
    if 'id' in registration:
        print('Registered phone ID: ' + str(registration['id']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', type=Path, required=True)
    parser.add_argument('--phone-apk', type=Path, required=True)
    parser.add_argument('--tv-apk', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--phone', default='8TVWJRGIEEEEINP7')
    parser.add_argument('--tv', default='192.168.1.14:5555')
    parser.add_argument('--host', default='192.168.1.3')
    parser.add_argument('--http-port', type=int, default=48189)
    parser.add_argument('--name', default='OPPO Reno11')
    parser.add_argument('--skip-tv', action='store_true', help='Provision phone only; leave TV untouched')
    parser.add_argument('--check-only', action='store_true', help='Validate local files without device/host operations')
    try:
        provision(parser.parse_args())
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        print('Kyty setup: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

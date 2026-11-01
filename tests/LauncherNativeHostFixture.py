#!/usr/bin/python3
"""Owner-management fixture; never runs games, audio or capture helpers."""
import json
import os
from pathlib import Path
import socket
import sys
import time

root = Path(__file__).parent
args = sys.argv[1:]
with (root / 'native-invocations.jsonl').open('a') as output:
    output.write(json.dumps(args) + '\n')
profile = Path(args[1])
profile.mkdir(mode=0o700, parents=True, exist_ok=True)
config_file = profile / 'host.json'
if args[0] == 'init':
    config_file.write_text(json.dumps({'devices': {}, 'device_identities': {}}))
    sys.exit(0)
if args[0] == 'configure':
    config = json.load(sys.stdin)
    old = json.loads(config_file.read_text())
    config['devices'] = old['devices']
    config['device_identities'] = old.get('device_identities', {})
    config_file.write_text(json.dumps(config))
    sys.exit(0)
assert args[0] == 'serve'
config = json.loads(config_file.read_text())
active = dict(config)
(root / 'native-pid').write_text(str(os.getpid()))
if (root / 'never-ready').exists():
    time.sleep(90)
if (root / 'ready-delay').exists():
    time.sleep(float((root / 'ready-delay').read_text()))
endpoint = Path(config['integration']['management_socket'])
endpoint.unlink(missing_ok=True)
server = socket.socket(socket.AF_UNIX)
server.bind(str(endpoint))
os.chmod(endpoint, 0o600)
server.listen(10)
generation = 1
running = None

def safe(value):
    return {k: v for k, v in value.items() if k not in ('devices', 'device_identities', 'games')}

def status():
    state_file = root / 'native-state.json'
    extra = json.loads(state_file.read_text()) if state_file.exists() else {}
    return dict(ok=True, state='ready', generation=generation, address='127.0.0.1:48231',
                running=extra.get('running'), media_state=extra.get('media_state'),
                paired_devices=[{'id': key, 'name': name} for key, name in config['devices'].items()],
                connections=[], active=safe(active), configured=safe(config),
                session_configuration=extra.get('session_configuration'),
                input_capabilities=extra.get('input_capabilities'),
                restart_required=active['bind'] != config['bind'])

while True:
    connection, _ = server.accept()
    try:
        with connection.makefile('rb') as source:
            request = json.loads(source.readline(65537))
        with (root / 'native-requests.jsonl').open('a') as output:
            output.write(json.dumps(request) + '\n')
        op = request['op']
        if op == 'reload':
            latest = json.loads(config_file.read_text())
            config = request.get('config', latest)
            config['devices'] = latest['devices']
            config['device_identities'] = latest.get('device_identities', {})
            config_file.write_text(json.dumps(config))
            generation += 1
        elif op == 'revoke':
            config['devices'].pop(request['device_id'], None)
            config_file.write_text(json.dumps(config))
        elif op == 'close':
            if (root / 'close-delay').exists():
                time.sleep(float((root / 'close-delay').read_text()))
            (root / 'native-state.json').write_text('{}')
        reply = status()
        if op == 'pair-ticket':
            bundle = profile / 'pairing-bundle-fixture.json'
            bundle.write_text('{}')
            os.chmod(bundle, 0o600)
            reply = {'ok': True, 'path': str(bundle), 'expires_in_seconds': 120}
        elif op == 'shutdown':
            reply = {'ok': True, 'state': 'stopped'}
        connection.sendall(json.dumps(reply).encode() + b'\n')
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        connection.close()
    if op == 'shutdown':
        endpoint.unlink(missing_ok=True)
        break

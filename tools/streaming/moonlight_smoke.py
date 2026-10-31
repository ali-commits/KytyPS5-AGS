#!/usr/bin/env python3
"""Pair and stream a private host with an unmodified Moonlight Embedded client.

The fake platform proves network/protocol video delivery, not decoded picture quality.
Use the GPU/codec fixture separately to verify colors, composition and decoding.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import secrets
import ssl
import subprocess
import time
import urllib.error
import urllib.request


def terminate(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--moonlight', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True, help='Fresh private output directory')
    parser.add_argument('--audio-sink', required=True, help='Existing private sink name, never the desktop sink')
    parser.add_argument('--port', type=int, default=48389)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--reconnect', action='store_true', help='Reconnect a second stock client session to the same producer')
    parser.add_argument('--codec', choices=('h264', 'h265'), default='h264')
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--fps', type=int, choices=(30, 60), default=60)
    parser.add_argument('--producer-command', nargs=argparse.REMAINDER, help='Optional fixture command; {socket} is substituted')
    args = parser.parse_args()
    if not (1 <= args.width <= 4096 and 1 <= args.height <= 4096):
        parser.error('Output dimensions must be within1..4096')
    if not args.audio_sink.startswith('kyty_'):
        parser.error('Use a private kyty_ audio sink')
    evidence = args.evidence.expanduser().absolute()
    evidence.mkdir(mode=0o700, parents=True)
    os.chmod(evidence, 0o700)
    socket = evidence/'frame.sock'
    if len(os.fsencode(socket)) > 100:
        parser.error('Choose an evidence directory with a short path for the Unix socket')
    binary_hashes = {}
    for name, path in (('host', args.host), ('moonlight', args.moonlight)):
        with path.open('rb') as source:
            binary_hashes[name] = hashlib.file_digest(source, 'sha256').hexdigest()
    user, password = 'kyty_smoke', secrets.token_urlsafe(32)
    apps = {'env': {}, 'apps': [{'name': 'Kyty GPU Test', 'cmd': '/usr/bin/sleep 120', 'auto-detach': False}]}
    (evidence/'apps.json').write_text(json.dumps(apps))
    config = evidence/'host.conf'
    values = {'port': args.port, 'sunshine_name': 'Kyty Isolated Smoke Test',
        'capture': 'kyty', 'encoder': 'nvenc', 'kyty_socket': socket,
        'kyty_private_audio': 'true', 'kyty_input': 'true', 'management_bind_address': '127.0.0.1',
        'audio_sink': args.audio_sink, 'file_apps': evidence/'apps.json', 'file_state': evidence/'state.json',
        'credentials_file': evidence/'credentials.json', 'pkey': evidence/'host-key.pem',
        'cert': evidence/'host-cert.pem', 'log_path': evidence/'host.log', 'upnp': 'false', 'av1_mode': '1'}
    config.write_text(''.join(f'{key} = {value}\n' for key, value in values.items()))
    config.chmod(0o600)
    with (evidence/'credentials-setup.log').open('wb') as output:
        subprocess.run([str(args.host.resolve()), str(config), '--creds-stdin'],
            input=(json.dumps({'username': user, 'password': password})+'\n').encode(),
            stdout=output, stderr=subprocess.STDOUT, check=True, timeout=20, cwd=evidence)
    auth = base64.b64encode(f'{user}:{password}'.encode()).decode()
    # Certificate trust is scoped to the exact self-signed certificate created by this host.
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_REQUIRED
    trusted = False

    def api(path, data=None):
        nonlocal trusted
        if not trusted:
            context.load_verify_locations(cafile=str(evidence/'host-cert.pem'))
            trusted = True
        request = urllib.request.Request(f'https://127.0.0.1:{args.port+1}{path}',
            data=None if data is None else json.dumps(data).encode(),
            headers={'Authorization': 'Basic '+auth, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, context=context, timeout=3) as response:
            return json.load(response)

    host = producer = pair = stream = None
    logs = []
    result = {'codec': args.codec, 'port': args.port, 'platform': 'stock Moonlight fake',
              'decoded_picture_verified': False, 'requested_width': args.width,
              'requested_height': args.height, 'requested_fps': args.fps, 'binary_sha256': binary_hashes}
    try:
        def launch(name, command, env=None):
            output = (evidence/name).open('wb')
            logs.append(output)
            return subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, cwd=evidence, env=env)
        host = launch('host-console.log', [str(args.host.resolve()), str(config)])
        deadline = time.monotonic()+30
        while time.monotonic() < deadline:
            if host.poll() is not None:
                raise RuntimeError('Private host exited during startup; inspect host-console.log')
            try:
                if isinstance(api('/api/pin').get('pairings'), list):
                    break
            except (OSError, ValueError, ssl.SSLError, urllib.error.URLError):
                time.sleep(0.2)
        else:
            raise RuntimeError('Private management API did not become ready')
        if args.producer_command:
            command = [part.replace('{socket}', str(socket)) for part in args.producer_command]
            producer = launch('producer.log', command)
        client = ['/usr/bin/stdbuf', '-oL', '-eL', str(args.moonlight.resolve())]
        common = ['-keydir', str(evidence/'client-keys'), '-port', str(args.port)]
        pair = launch('pair.log', client+['pair', '-pin', '4826', *common, '127.0.0.1'])
        deadline = time.monotonic()+20
        while time.monotonic() < deadline:
            pending = api('/api/pin').get('pairings', [])
            if pending:
                api('/api/pin', {'pairing_id': pending[0]['id'], 'pin': '4826', 'name': 'Kyty automated test'})
                break
            if pair.poll() is not None:
                raise RuntimeError('Moonlight exited before PIN approval')
            time.sleep(0.2)
        if pair.wait(timeout=20) != 0:
            raise RuntimeError('Moonlight pairing failed; inspect pair.log')
        result['paired'] = True
        with (evidence/'catalog.log').open('wb') as output:
            subprocess.run(client+['list', *common, '127.0.0.1'], stdout=output,
                stderr=subprocess.STDOUT, check=True, timeout=15)
        if 'Kyty GPU Test' not in (evidence/'catalog.log').read_text(errors='replace'):
            raise RuntimeError('The private test application was absent from the Moonlight catalog')
        result['catalog'] = True
        result['capture_sessions'] = []
        for cycle in range(2 if args.reconnect else 1):
            stream_name = 'stream.log' if cycle == 0 else 'stream-reconnect.log'
            stream = launch(stream_name, client+['stream', '-platform', 'fake', '-viewonly', '-verbose',
                '-width', str(args.width), '-height', str(args.height), '-fps', str(args.fps), '-codec', args.codec,
                '-app', 'Kyty GPU Test', *common, '127.0.0.1'])
            deadline = time.monotonic()+args.seconds
            while time.monotonic() < deadline:
                if stream.poll() is not None:
                    raise RuntimeError('Moonlight stream exited early; inspect '+stream_name)
                if host.poll() is not None or (producer and producer.poll() is not None):
                    raise RuntimeError('Host/producer exited while streaming')
                time.sleep(0.2)
            received = 'Received first video packet' in (evidence/stream_name).read_text(errors='replace')
            result['received_video_packets'] = received
            if not received:
                raise RuntimeError('Stock Moonlight did not report receiving video packets')
            status = api('/api/kyty/status')
            result['capture_sessions'].append(status)
            if args.producer_command and (status.get('state') != 'gpu-connected' or status.get('received', 0) < 1 or status.get('copied', 0) < 1 or status.get('viewers') != 1):
                raise RuntimeError('GPU producer was not delivering frames to the active Moonlight viewer')
            terminate(stream)
            stream = None
            if cycle == 0 and args.reconnect:
                time.sleep(1)
        result['reconnected'] = args.reconnect
        result['passed'] = True
    finally:
        terminate(stream)
        if host and host.poll() is None:
            try:
                api('/api/apps/close', {})
            except Exception:
                pass
        terminate(pair)
        terminate(producer)
        terminate(host)
        for output in logs:
            output.close()
        (evidence/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

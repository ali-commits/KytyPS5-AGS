#!/usr/bin/env python3
"""Controller-only Kyty client. Private identity is read only from --config."""
import argparse
import hashlib
import hmac
import http.client
import json
from pathlib import Path
import queue
import ssl
import threading
import time
from controller import Controller, Feedback

class Transport:

    def __init__(self, config):
        self.pin = config['server_sha256'].replace(':', '').lower()
        if len(self.pin) != 64 or any((c not in '0123456789abcdef' for c in self.pin)):
            raise ValueError('server_sha256 must be the SHA256 of the server DER certificate')
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        # Exact DER pin below replaces CA/hostname authentication.
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(config['cert'], config['key'])
        self.connection = http.client.HTTPSConnection(config['host'], int(config.get('port', 47984)), timeout=0.15, context=context)

    def post(self, payload):
        try:
            if self.connection.sock is None:
                self.connection.connect()
            actual = hashlib.sha256(self.connection.sock.getpeercert(binary_form=True)).hexdigest()
            if not hmac.compare_digest(self.pin, actual):
                raise ValueError('Server certificate pin mismatch; check the trusted host fingerprint')
            self.connection.request('POST', '/kyty/v1/relay', json.dumps(payload, allow_nan=False), {'Content-Type': 'application/json'})
            response = self.connection.getresponse()
            raw = response.read(16385)
            if len(raw) > 16384:
                raise ValueError('Host response exceeds size limit')
            if response.status != 200:
                detail = ''
                try:
                    detail = json.loads(raw).get('error', '')
                except (ValueError, AttributeError):
                    pass
                if response.status == 409:
                    raise RuntimeError('Start the Kyty game and TV stream, or disconnect the other laptop. ' + str(detail))
                raise RuntimeError(f'Host returned HTTP {response.status}: {detail}')
            data = json.loads(raw)
            if not isinstance(data, dict) or data.get('ok') is not True:
                raise ValueError('Host rejected relay request')
            return data
        except Exception:
            self.close()
            raise

    def close(self):
        self.connection.close()

class Network:
    """One daemon owns TLS; SDL remains responsive during DNS/TLS/read delays."""

    def __init__(self, config):
        self.requests = queue.Queue()
        self.results = queue.Queue()
        self.config = config
        self.thread = threading.Thread(target=self.run, daemon=True, name='kyty-tls')
        self.thread.start()

    def run(self):
        transport = None
        try:
            transport = Transport(self.config)
            while True:
                item = self.requests.get()
                if item is None:
                    break
                tag, payload = item
                try:
                    self.results.put((tag, transport.post(payload), None, time.monotonic()))
                except Exception as e:
                    self.results.put((tag, None, str(e), time.monotonic()))
        except Exception as e:
            self.results.put(('init', None, str(e), time.monotonic()))
        finally:
            if transport:
                transport.close()

    def send(self, tag, payload):
        self.requests.put((tag, payload))

    def close(self):
        self.requests.put(None)

class Worker:

    def __init__(self, config):
        self.config = config
        self.commands = queue.Queue()
        self.status = queue.Queue(maxsize=1)
        self.thread = threading.Thread(target=self.run, daemon=True, name='kyty-sdl')
        self.thread.start()

    def publish(self, message):
        try:
            self.status.get_nowait()
        except queue.Empty:
            pass
        self.status.put_nowait(message)

    def run(self):
        controller = None
        network = None
        feedback = None
        session = None
        pending = None
        wanted = False
        closing = False
        sequence = 0
        sent = 0
        message = 'Ready. Connect when the game and TV stream are running.'
        latency = 0
        last_publish = 0
        started = 0
        try:
            controller = Controller()
            feedback = Feedback(controller)
            network = Network(self.config)
            while True:
                now = time.monotonic()
                feedback.expire(now)
                try:
                    while True:
                        command = self.commands.get_nowait()
                        if command == 'connect':
                            wanted = True
                        else:
                            wanted = False
                            feedback.stop()
                            if command == 'close':
                                closing = True
                except queue.Empty:
                    pass
                snapshot = controller.snapshot()
                if not snapshot['connected']:
                    feedback.stop()
                try:
                    tag, data, error, finished = network.results.get_nowait()
                    latency = (finished - started) * 1000
                    pending = None
                    if error:
                        feedback.stop()
                        session = None
                        wanted = False
                        message = error
                    elif tag == 'connect':
                        session = data.get('session')
                        if not isinstance(session, str) or not session:
                            raise ValueError('Host omitted session')
                        sequence = 0
                        message = 'Connected'
                    elif tag == 'input' and session:
                        if data.get('session') != session:
                            raise ValueError('Host returned a different session')
                        # Do not let a late reply restart expired feedback.
                        if wanted and snapshot['connected'] and (now - started < 0.2):
                            feedback.apply(data, started)
                            feedback.expire(now)
                        else:
                            feedback.stop()
                    elif tag == 'disconnect':
                        session = None
                        message = 'Disconnected'
                except queue.Empty:
                    pass
                # One request in flight; no stale snapshot queue.
                if pending is None:
                    if session and (not wanted):
                        pending = 'disconnect'
                        payload = {'action': 'disconnect', 'session': session}
                    elif session and wanted:
                        sequence += 1
                        sent += 1
                        pending = 'input'
                        payload = dict(snapshot, action='input', session=session, sequence=sequence)
                    elif wanted:
                        pending = 'connect'
                        payload = {'action': 'connect'}
                        message = 'Connecting…'
                    else:
                        payload = None
                    if payload is not None:
                        started = time.monotonic()
                        network.send(pending, payload)
                if pending and now - started >= 0.2:
                    feedback.stop()
                    wanted = False
                    message = 'Host timed out; effects stopped. Reconnect after checking the host.'
                if closing and (session is None and pending is None or now - started > 0.4):
                    break
                if now - last_publish >= 0.1:
                    self.publish(f"{message}\nController: {controller.battery}\nInput snapshots: {sent} · Last response: {latency:.1f} ms\nFeedback: {('active' if feedback.deadline else 'off')}")
                    last_publish = now
                time.sleep(0.01)
        except Exception as e:
            self.publish(str(e))
        finally:
            if controller:
                controller.close()
            if network:
                network.close()

def load_config(path):
    path = Path(path).expanduser()
    if path.stat().st_mode & 0o077:
        raise ValueError(f'Config must be private: chmod 600 {path}')
    config = json.loads(path.read_text())
    for field in ('host', 'cert', 'key', 'server_sha256'):
        if not isinstance(config.get(field), str) or not config[field]:
            raise ValueError(f'Missing config field: {field}')
    for field in ('cert', 'key'):
        value = Path(config[field]).expanduser()
        if not value.is_absolute():
            value = path.parent / value
        config[field] = str(value)
    if Path(config['key']).stat().st_mode & 0o077:
        raise ValueError('Client key must be private (chmod 600)')
    return config

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, help='Private JSON identity/config file')
    args = parser.parse_args()
    import tkinter as tk
    from tkinter import ttk
    root = tk.Tk()
    root.title('Kyty Controller')
    root.geometry('610x250')
    state = tk.StringVar(value='Loading configuration…')
    ttk.Label(root, text='Kyty · Laptop controller', font=('', 16, 'bold')).pack(pady=12)
    ttk.Label(root, textvariable=state, wraplength=580, justify='left').pack(fill='x', padx=16)
    worker = None
    try:
        worker = Worker(load_config(args.config))
    except Exception as e:
        state.set(str(e))
    row = ttk.Frame(root)
    row.pack(pady=16)

    def command(value):
        if worker:
            worker.commands.put(value)
    ttk.Button(row, text='Connect', command=lambda: command('connect')).pack(side='left', padx=6)
    ttk.Button(row, text='Disconnect', command=lambda: command('disconnect')).pack(side='left', padx=6)

    def poll():
        if worker:
            try:
                state.set(worker.status.get_nowait())
            except queue.Empty:
                pass
        root.after(50, poll)

    def close():
        command('close')

        def finish():
            if worker and worker.thread.is_alive():
                root.after(50, finish)
            else:
                root.destroy()
        finish()
    root.protocol('WM_DELETE_WINDOW', close)
    poll()
    root.mainloop()
if __name__ == '__main__':
    main()

#!/usr/bin/python3
"""Private deterministic process fixtures for the native launcher integration test."""
import http.server
import json
import os
import pathlib
import shutil
import signal
import ssl
import sys
import time

root = pathlib.Path(__file__).parent
if pathlib.Path(sys.argv[0]).name != "pactl" and (root / "ignore-termination").exists():
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
if pathlib.Path(sys.argv[0]).name == "pactl":
    path = root / "modules.json"
    modules = json.loads(path.read_text()) if path.exists() else []
    args = sys.argv[1:]
    with (root / "pactl-commands.jsonl").open("a") as file:
        file.write(json.dumps(args) + "\n")
    behavior_path = root / "pactl-behavior.json"
    behavior = json.loads(behavior_path.read_text()) if behavior_path.exists() else {}
    prefix = behavior.get("prefix", [])
    delayed = bool(prefix) and args[:len(prefix)] == prefix and (not behavior.get("after_load") or bool(modules))
    def finish_command():
        if delayed:
            (root / "command-pending").write_text(json.dumps({"pid": os.getpid(), "args": args}))
            time.sleep(behavior.get("delay", 0.35))
            if behavior.get("fail"):
                sys.exit(1)
    if args == ["get-default-sink"]:
        finish_command()
        print(os.environ.get("KYTY_FIXTURE_DEFAULT_SINK", "original_sink"))
    elif args[:1] == ["load-module"]:
        index = max([item["index"] for item in modules] + [4100]) + 1
        modules.append({"index": index, "name": args[1], "argument": " ".join(args[2:])})
        path.write_text(json.dumps(modules))
        finish_command()
        print(index)
    elif args == ["--format=json", "list", "modules"]:
        print(json.dumps(modules))
    elif args[:1] == ["unload-module"]:
        modules = [item for item in modules if str(item["index"]) != args[1]]
        path.write_text(json.dumps(modules))
    else:
        sys.exit(1)
    sys.exit(0)

if "--kyty-capabilities" in sys.argv:
    if (root / "capability-delay").exists():
        (root / "capability-active").write_text(str(os.getpid()))
        time.sleep(float((root / "capability-delay").read_text()))
    print(json.dumps({"protocol_version": 1, "capture": "kyty", "gpu_transport": "vulkan-buffer-fd", "private_audio": True, "private_input": True, "credentials_stdin": True, "version": "Test host 1"}))
    sys.exit(0)
config = {}
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    if "=" in line:
        key, value = line.split("=", 1)
        config[key.strip()] = value.strip()
if "--creds-stdin" in sys.argv:
    credentials = json.loads(sys.stdin.readline())
    assert credentials["username"] and len(credentials["password"]) >= 32
    active = root / "credential-active"
    if active.exists():
        try:
            os.kill(int(active.read_text()), 0)
            (root / "credential-overlap").write_text("yes")
        except (ProcessLookupError, ValueError):
            pass
    active.write_text(str(os.getpid()))
    if (root / "credential-delay").exists():
        time.sleep(float((root / "credential-delay").read_text()))
    if active.exists() and active.read_text() == str(os.getpid()):
        active.unlink()
    (root / "stdin-credentials-used").write_text("yes")
    sys.exit(0)
if (root / "crash-host").exists():
    sys.exit(42)
shutil.copyfile(root / "cert.pem", config["cert"])
shutil.copyfile(root / "key.pem", config["pkey"])


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/pin":
            self.send_json({"pairings": [{"id": "a" * 32, "name": "TV", "address": "192.0.2.10"}]})
        elif self.path == "/api/clients/list":
            self.send_json({"status": True, "named_certs": [{"name": "TV", "uuid": "client-1", "enabled": True}]})
        else:
            self.send_error(404)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")
        with (root / "api-requests.jsonl").open("a") as file:
            file.write(json.dumps({"path": self.path, "body": body}) + "\n")
        if self.path == "/api/pin" and body.get("pairing_id") != "a" * 32:
            self.send_json({"status": False, "error": "wrong pairing ID"})
        else:
            self.send_json({"status": True})

    def send_json(self, payload):
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


server = http.server.HTTPServer(("127.0.0.1", int(config["port"]) + 1), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(config["cert"], config["pkey"])
server.socket = context.wrap_socket(server.socket, server_side=True)
print("Private test host ready", flush=True)
server.serve_forever()

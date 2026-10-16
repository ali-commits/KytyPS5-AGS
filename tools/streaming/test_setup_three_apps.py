#!/usr/bin/env python3
"""Hardware-free trust-boundary tests for one-time device provisioning."""
import http.server
import json
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import unittest

from setup_three_apps import management_request, private_json, public_der


class SetupTrustTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.storage = tempfile.TemporaryDirectory()
        cls.root = Path(cls.storage.name)
        for prefix in ('host', 'other'):
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                '-days', '1', '-subj', '/CN=Kyty setup fixture', '-keyout',
                str(cls.root / (prefix + '-key.pem')), '-out',
                str(cls.root / (prefix + '-cert.pem'))], check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (cls.root / 'controller-auth.json').write_text(json.dumps(
            {'username': 'fixture', 'password': 'fixture-only-not-real-credentials'}))
        (cls.root / 'controller-auth.json').chmod(0o600)

    @classmethod
    def tearDownClass(cls):
        cls.storage.cleanup()

    def setUp(self):
        self.requests = []
        records = self.requests
        class Handler(http.server.BaseHTTPRequestHandler):
            def handle(self):
                try:
                    super().handle()
                except ConnectionError:
                    # A rejected exact pin closes TLS before any HTTP request is sent.
                    pass
            def do_POST(self):
                records.append((self.path, self.headers.get('Authorization'),
                    json.loads(self.rfile.read(int(self.headers['Content-Length'])))))
                raw = b'{"ok":true,"id":"fixture"}'
                self.send_response(200)
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
            def log_message(self, *args):
                pass
        self.server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(self.root / 'host-cert.pem', self.root / 'host-key.pem')
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def test_valid_pin_sends_only_expected_local_registration(self):
        result = management_request(self.root, self.server.server_port, 'POST',
            '/api/kyty/companion/phones', {'name': 'Test phone', 'certificate_pem': 'public fixture'})
        self.assertTrue(result['ok'])
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.requests[0][0], '/api/kyty/companion/phones')
        self.assertTrue(self.requests[0][1].startswith('Basic '))
        self.assertEqual(self.requests[0][2]['name'], 'Test phone')

    def test_wrong_exact_pin_sends_no_credentials_or_request(self):
        with self.assertRaises(ssl.SSLCertVerificationError):
            management_request(self.root, self.server.server_port, 'POST',
                '/api/kyty/companion/phones', {}, expected_pin='0' * 64)
        self.assertEqual(self.requests, [])

    def test_untrusted_server_sends_no_credentials_or_request(self):
        with tempfile.TemporaryDirectory() as temporary:
            wrong = Path(temporary)
            (wrong / 'host-cert.pem').write_bytes((self.root / 'other-cert.pem').read_bytes())
            (wrong / 'controller-auth.json').write_bytes((self.root / 'controller-auth.json').read_bytes())
            (wrong / 'controller-auth.json').chmod(0o600)
            with self.assertRaises(ssl.SSLCertVerificationError):
                management_request(wrong, self.server.server_port, 'POST',
                    '/api/kyty/companion/phones', {})
        self.assertEqual(self.requests, [])

    def test_rejects_nonprivate_management_credentials(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'credentials.json'
            path.write_text('{}')
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                private_json(path)
            path.unlink()
            path.symlink_to(self.root / 'controller-auth.json')
            with self.assertRaises(ValueError):
                private_json(path)

    def test_public_certificate_rejects_private_or_multiple_certificates(self):
        certificate = (self.root / 'host-cert.pem').read_text()
        self.assertGreater(len(public_der(certificate)), 100)
        for invalid in (certificate * 2, 'PRIVATE KEY', 'x' * 16385, None):
            with self.assertRaises(ValueError):
                public_der(invalid)


if __name__ == '__main__':
    unittest.main()

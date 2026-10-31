#!/usr/bin/env python3
"""Private, hardware-free migration fixtures; no installed profiles are read."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import ssl
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).with_name('migrate_profile.py')


class ProfileMigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.certificates = tempfile.TemporaryDirectory()
        cls.cert_root = Path(cls.certificates.name)
        for name in ('host', 'different'):
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                '-days', '1', '-subj', '/CN=Profile migration fixture',
                '-keyout', str(cls.cert_root / (name + '-key.pem')),
                '-out', str(cls.cert_root / (name + '-cert.pem'))], check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.certificates.cleanup()

    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.source = self.root / 'old-config' / 'streaming'
        self.source.mkdir(mode=0o700, parents=True)
        self.destination = self.root / 'new-config' / 'streaming'
        self.destination.parent.mkdir(mode=0o700)
        self.old_prefix = self.root / 'old-install'
        self.new_prefix = self.root / 'new-install'
        self.write('host-cert.pem', (self.cert_root / 'host-cert.pem').read_bytes())
        self.write('host-key.pem', (self.cert_root / 'host-key.pem').read_bytes())
        self.write('host-state.json', json.dumps({'root': {'uniqueid': 'fixture-host',
            'named_devices': [{'name': name, 'uniqueid': str(i)} for i, name in
                enumerate(('Fixture TV', 'Fixture phone', 'Fixture laptop'))]}}).encode())
        self.secret = 'fixture-only-not-real-secret'
        for name in ('host-auth.json', 'controller-auth.json'):
            self.write(name, json.dumps({'username': 'fixture', 'password': self.secret,
                'opaque_token': str(self.old_prefix / 'do-not-rewrite')}).encode())
        self.write('streaming.ini', ('; preserve comments and formatting\n[host]\n'
            f'backend={self.old_prefix}/libexec/kyty_stream_host\nport=48189\n'
            f'custom_path="{self.source}/custom path"\n'
            '[quality]\ncodec=HEVC\nbitrate=47\nfps=60\nresolution=1440p\n'
            '[audio]\nlocal_monitor=false\n[controller_audio]\nenabled=false\nport=0\n'
            '[display]\nshow_local=false\n[custom]\nunknown_preference=keep exactly\n').encode())
        self.write('companion/settings.json', json.dumps({'receiver_path': str(self.old_prefix / 'bin/receiver'),
            'profile_directory': str(self.source), 'token': str(self.old_prefix / 'opaque'),
            'nested': {'cache_dir': str(self.old_prefix / 'cache'), 'password': self.secret},
            'unrelated_path': str(self.old_prefix) + '-other/cache'}).encode())
        certificate = (self.cert_root / 'host-cert.pem').read_text()
        fingerprint = hashlib.sha256(ssl.PEM_cert_to_DER_cert(certificate)).hexdigest()
        self.write('companion/phones.json', json.dumps([
            {'id': fingerprint, 'name': 'Fixture phone', 'certificate_pem': certificate}]).encode())
        self.write('controller-audio-clients.json', json.dumps({'generation_ceiling': 42, 'receivers': [
            {'id': fingerprint, 'name': 'Fixture receiver', 'parent_client_id': 'fixture-parent',
             'certificate_pem': certificate, 'enabled': False}]}).encode())
        for name in ('launcher.lock', 'session.json', 'launch.json', 'apps.json', 'host.conf', 'host.log',
                     'companion/registry.lock'):
            self.write(name, b'ephemeral fixture, never migrate')

    def write(self, name, data):
        path = self.source / name
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o600)

    def snapshot(self):
        return {str(p.relative_to(self.source)): p.read_bytes()
                for p in self.source.rglob('*') if p.is_file()}

    def run_cli(self):
        return subprocess.run([sys.executable, str(SCRIPT), '--source', str(self.source),
            '--destination', str(self.destination), '--old-prefix', str(self.old_prefix),
            '--new-prefix', str(self.new_prefix)], capture_output=True, text=True, timeout=5)

    def module(self):
        spec = importlib.util.spec_from_file_location('migration_fixture', SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def assert_rejected(self):
        before = self.snapshot()
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(self.secret, result.stdout + result.stderr)
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.snapshot(), before)
        return result

    def test_complete_profile_keeps_pairing_auth_preferences_and_private_permissions(self):
        before = self.snapshot()
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.snapshot(), before)
        files = {str(p.relative_to(self.destination)) for p in self.destination.rglob('*') if p.is_file()}
        self.assertEqual(files, {'host-cert.pem', 'host-key.pem', 'host-state.json', 'host-auth.json',
            'controller-auth.json', 'streaming.ini', 'companion/settings.json', 'companion/phones.json',
            'controller-audio-clients.json'})
        for name in ('host-cert.pem', 'host-key.pem', 'host-state.json', 'host-auth.json',
                     'controller-auth.json', 'companion/phones.json', 'controller-audio-clients.json'):
            self.assertEqual((self.destination / name).read_bytes(), before[name])
        state = json.loads((self.destination / 'host-state.json').read_text())
        self.assertEqual(len(state['root']['named_devices']), 3)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.destination / 'host-cert.pem', self.destination / 'host-key.pem')
        expected_ini = before['streaming.ini'].replace(str(self.old_prefix).encode(), str(self.new_prefix).encode())
        expected_ini = expected_ini.replace(str(self.source).encode(), str(self.destination).encode())
        self.assertEqual((self.destination / 'streaming.ini').read_bytes(), expected_ini)
        settings = json.loads((self.destination / 'companion/settings.json').read_text())
        self.assertEqual(settings['receiver_path'], str(self.new_prefix / 'bin/receiver'))
        self.assertEqual(settings['profile_directory'], str(self.destination))
        self.assertEqual(settings['nested']['cache_dir'], str(self.new_prefix / 'cache'))
        self.assertEqual(settings['token'], str(self.old_prefix / 'opaque'))
        self.assertEqual(settings['nested']['password'], self.secret)
        self.assertEqual(settings['unrelated_path'], str(self.old_prefix) + '-other/cache')
        for path in (self.destination, *self.destination.rglob('*')):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700 if path.is_dir() else 0o600)
        metadata = json.loads(result.stdout)
        self.assertEqual(set(metadata['files']), files)
        self.assertEqual(metadata['files']['host-key.pem']['source_sha256'], metadata['files']['host-key.pem']['destination_sha256'])
        self.assertNotIn(self.secret, result.stdout)
        self.assertNotIn(str(self.root), result.stdout)

    def test_optional_companion_files_can_be_absent(self):
        for name in ('companion/settings.json', 'companion/phones.json', 'controller-audio-clients.json'):
            (self.source / name).unlink()
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(json.loads(result.stdout)['files']), 6)

    def test_missing_required_file_rejects_without_changes(self):
        (self.source / 'controller-auth.json').unlink()
        self.assert_rejected()

    def test_certificate_key_mismatch_cleans_partial_destination(self):
        self.write('host-key.pem', (self.cert_root / 'different-key.pem').read_bytes())
        self.assert_rejected()

    def test_encrypted_key_fails_without_interactive_password_prompts(self):
        encrypted = subprocess.run(['openssl', 'pkey', '-in', str(self.cert_root / 'host-key.pem'),
            '-aes256', '-passout', 'pass:fixture-key-password'], check=True, capture_output=True).stdout
        self.write('host-key.pem', encrypted)
        result = self.assert_rejected()
        self.assertNotIn('Enter PEM pass phrase', result.stderr)

    def test_existing_empty_or_nonempty_destination_is_never_replaced(self):
        self.destination.mkdir(mode=0o700)
        for populated in (False, True):
            if populated:
                (self.destination / 'keep').write_bytes(b'existing content')
            result = self.run_cli()
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(self.destination.is_dir())
            if populated:
                self.assertEqual((self.destination / 'keep').read_bytes(), b'existing content')

    def test_symlinked_file_directory_or_source_is_rejected(self):
        path = self.source / 'host-auth.json'
        original = path.read_bytes()
        outside = self.root / 'outside.json'
        outside.write_bytes(original)
        path.unlink()
        path.symlink_to(outside)
        self.assert_rejected()
        path.unlink()
        self.write('host-auth.json', original)
        companion = self.source / 'companion'
        moved = self.root / 'moved-companion'
        companion.rename(moved)
        companion.symlink_to(moved, target_is_directory=True)
        self.assert_rejected()
        companion.unlink()
        moved.rename(companion)
        real = self.source
        self.source = self.root / 'source-link'
        self.source.symlink_to(real, target_is_directory=True)
        self.assert_rejected()

    def test_group_readable_or_unowned_source_is_rejected(self):
        (self.source / 'host-key.pem').chmod(0o640)
        self.assert_rejected()
        (self.source / 'host-key.pem').chmod(0o600)
        module = self.module()
        with patch.object(module.os, 'getuid', return_value=os.getuid() + 1):
            with self.assertRaises(module.MigrationError):
                module.migrate(self.source, self.destination, self.old_prefix, self.new_prefix)
        self.assertFalse(self.destination.exists())

    def test_fifo_source_is_rejected_without_waiting_for_a_writer(self):
        (self.source / 'host-auth.json').unlink()
        os.mkfifo(self.source / 'host-auth.json', 0o600)
        self.assert_rejected()

    def test_malformed_settings_json_cleans_destination_without_leaking_values(self):
        self.write('companion/settings.json', b'{"token":"' + self.secret.encode())
        result = self.assert_rejected()
        self.assertNotIn(self.secret, result.stderr)

    def test_source_change_during_copy_is_detected_and_partial_output_removed(self):
        module = self.module()
        write = module._write_private
        changed = False
        def change_source_after_write(path, data):
            nonlocal changed
            write(path, data)
            if not changed:
                changed = True
                self.write('host-state.json', b'{"root":{"uniqueid":"changed externally"}}')
        with patch.object(module, '_write_private', side_effect=change_source_after_write):
            with self.assertRaises(module.MigrationError):
                module.migrate(self.source, self.destination, self.old_prefix, self.new_prefix)
        self.assertFalse(self.destination.exists())
        self.assertIn(b'changed externally', (self.source / 'host-state.json').read_bytes())


if __name__ == '__main__':
    unittest.main()

"""Black-box checks for the dormant, prebuilt-binary user installer."""

import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().with_name("install-user.sh")


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.prefix = self.root / "fresh v3%q"
        self.xdg = self.root / "xdg"
        self.config = self.root / "private config"
        self.source = self.root / "prebuilt"
        self.source.write_text(
            '#!/bin/sh\nprintf "invoked\\n" >> "$INSTALL_TEST_EXEC_LOG"\nexit 99\n'
        )
        self.source.chmod(0o755)
        self.stub = self.root / "stub"
        self.stub.mkdir()
        systemctl = self.stub / "systemctl"
        systemctl.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$*" >> "$INSTALL_TEST_LOG"\n'
        )
        systemctl.chmod(0o755)
        self.env = dict(
            os.environ,
            XDG_CONFIG_HOME=str(self.xdg),
            PATH=str(self.stub) + os.pathsep + os.environ["PATH"],
            INSTALL_TEST_LOG=str(self.root / "calls"),
            INSTALL_TEST_EXEC_LOG=str(self.root / "executions"),
        )
        self.unit = self.xdg / "systemd/user/kyty-controller-audio-v3.service"

    def invoke(self):
        return subprocess.run(
            [
                "bash", str(SCRIPT), "--binary", str(self.source),
                "--prefix", str(self.prefix), "--config-dir", str(self.config),
            ],
            env=self.env,
            text=True,
            capture_output=True,
            timeout=15,
        )

    def test_install_is_private_dormant_and_idempotent(self):
        first = self.invoke()
        self.assertEqual(first.returncode, 0, first.stderr)
        binary = self.prefix / "bin/controller-audio-receiver"
        for path, mode in (
            (self.prefix, 0o700),
            (self.prefix / "bin", 0o700),
            (self.config, 0o700),
            (binary, 0o755),
            (self.prefix / ".controller-audio-install", 0o600),
            (self.unit, 0o600),
        ):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), mode, str(path))
        self.assertEqual(binary.read_bytes(), self.source.read_bytes())
        text = self.unit.read_text()
        self.assertIn("ExecCondition=", text)
        self.assertIn("--status", text)
        self.assertIn("ConditionPathExists=", text)
        self.assertIn("fresh v3%%q", text)
        self.assertNotIn("--sink usb", text)
        self.assertFalse((self.config / "config.json").exists())
        self.assertFalse((self.root / "executions").exists())
        second = self.invoke()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.unit.read_text(), text)
        self.assertEqual(binary.read_bytes(), self.source.read_bytes())
        self.assertFalse((self.root / "executions").exists())
        self.assertEqual(
            (self.root / "calls").read_text().splitlines(),
            ["--user daemon-reload"] * 2,
        )

    @unittest.skipUnless(shutil.which("systemd-analyze"), "systemd-analyze unavailable")
    def test_systemd_unit_syntax(self):
        installed = self.invoke()
        self.assertEqual(installed.returncode, 0, installed.stderr)
        result = subprocess.run(
            ["systemd-analyze", "--user", "verify", str(self.unit)],
            text=True,
            capture_output=True,
            timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout + result.stderr, "")

    def test_refuses_replacement_binary(self):
        installed = self.invoke()
        self.assertEqual(installed.returncode, 0, installed.stderr)
        binary = self.prefix / "bin/controller-audio-receiver"
        original = binary.read_bytes()
        original_unit = self.unit.read_bytes()
        self.source.write_text("#!/bin/sh\nexit 98\n")
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertEqual(binary.read_bytes(), original)
        self.assertEqual(self.unit.read_bytes(), original_unit)
        self.assertEqual(
            (self.root / "calls").read_text().splitlines(), ["--user daemon-reload"]
        )

    def test_refuses_symlink_prefix(self):
        target = self.root / "target"
        target.mkdir()
        self.prefix.symlink_to(target)
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertEqual(list(target.iterdir()), [])
        self.assertFalse(self.unit.exists())

    def test_refuses_shared_config_directory(self):
        self.config.mkdir(mode=0o755)
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o755)
        self.assertEqual(list(self.config.iterdir()), [])
        self.assertFalse(self.unit.exists())

    def test_refuses_unrelated_existing_prefix(self):
        self.prefix.mkdir(mode=0o700)
        unrelated = self.prefix / "unrelated"
        unrelated.write_text("preserve")
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertEqual(unrelated.read_text(), "preserve")
        self.assertFalse(self.unit.exists())


if __name__ == "__main__":
    unittest.main()

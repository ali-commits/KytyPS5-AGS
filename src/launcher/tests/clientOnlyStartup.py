#!/usr/bin/env python3
"""Offscreen installed-layout fixture. Run with a private /dev (e.g. bwrap).
Never selects a real host/profile or launches a game.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

if Path("/dev/input").exists() or any(Path("/dev").glob("hidraw*")):
    raise SystemExit("Run this fixture with a private /dev (for example bwrap --dev /dev).")

with tempfile.TemporaryDirectory(prefix="ags-client-only-") as directory:
    root = Path(directory)
    binary = root / "launcher"
    shutil.copy2(sys.argv[1], binary)
    for name in ("runtime", "config", "home"):
        (root / name).mkdir(mode=0o700)
    # Even explicitly configured local PS5 folders must not force emulator probing
    # in client-only mode. The executable would leave a marker if invoked.
    (root / "Kyty.ini").write_text("[Launcher]\ngame_dirs=/nonexistent-fixture-game\nsteam_roots=/nonexistent-fixture-steam\n")
    emulator = root / "kyty_emulator"
    emulator.write_text("#!/bin/sh\ntouch '" + str(root / "unexpected-emulator") + "'\nexit 1\n")
    emulator.chmod(0o700)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_RUNTIME_DIR=str(root / "runtime"),
               XDG_CONFIG_HOME=str(root / "config"), HOME=str(root / "home"),
               SDL_AUDIO_DRIVER="dummy", SDL_JOYSTICK_HIDAPI="0", QT_FORCE_STDERR_LOGGING="1")
    env.pop("KYTY_HOST_SOCKET", None)
    env["KYTY_STREAMING_CONFIG_DIR"] = str(root / "host-profile")
    log = (root / "launcher.log").open("wb")
    process = subprocess.Popen([str(binary), "--client-only", "--local"], cwd=root, env=env, stdout=log, stderr=log)
    try:
        time.sleep(1)
        assert process.poll() is None, "client-only launcher exited during setup"
        assert not (root / "unexpected-emulator").exists(), "receiver mode ran the PS5 interpreter"
        # A blocking setup modal would prevent clean shutdown confirmation.
        result = subprocess.run([str(binary), "--quit"], cwd=root, env=env, capture_output=True, timeout=8)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        assert b"shutdown confirmed" in result.stdout, result.stdout
        process.wait(timeout=5)
        assert process.returncode == 0, f"launcher exit {process.returncode}"
        assert not (root / "host-profile").exists(), "receiver mode initialized a local host profile"
        assert not list((root / "runtime").rglob("control.sock")), "receiver mode started the local host daemon"
        print("client-only startup and --quit passed without emulator/host setup")
    except BaseException:
        log.flush()
        print((root / "launcher.log").read_text(errors="replace"), file=sys.stderr)
        print(subprocess.run(["ps", "-o", "pid,ppid,stat,wchan,cmd", "-p", str(process.pid)], capture_output=True, text=True).stdout, file=sys.stderr)
        raise
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        log.close()

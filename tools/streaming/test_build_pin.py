#!/usr/bin/env python3
"""Exercise the build preflight with real local Git repositories, without building."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


BUILD_SCRIPT = Path(__file__).with_name('build.sh')


class BuildPinTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1',
                        GIT_CONFIG_GLOBAL='/dev/null')
        self.source = self.root / 'kyty' / 'tools' / 'streaming'
        self.source.mkdir(parents=True)
        shutil.copyfile(BUILD_SCRIPT, self.source / 'build.sh')
        self.pin = self.source / 'host-source.rev'
        self.host = self.repository('host')
        (self.host / 'CMakeLists.txt').write_text('# test host\n')
        (self.host / '.gitignore').write_text('cmake-build-*/\n')
        self.commit(self.host)
        self.required = self.git(self.host, 'rev-parse', 'HEAD')
        self.pin.write_text(self.required + '\n')
        self.probe = self.root / 'cmake-reached'
        bin_dir = self.root / 'bin'
        bin_dir.mkdir()
        cmake = bin_dir / 'cmake'
        cmake.write_text('#!/bin/sh\nprintf "%s\\n" "$*" > "$CMAKE_PROBE"\nexit 91\n')
        cmake.chmod(0o755)
        self.env.update(PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
                        CMAKE_PROBE=str(self.probe),
                        KYTY_STREAM_HOST_SOURCE=str(self.host))

    def git(self, repo, *args):
        return subprocess.check_output(['git', '-C', str(repo), *args],
                                       env=self.env, stderr=subprocess.PIPE,
                                       text=True).strip()

    def repository(self, name):
        repo = self.root / name
        repo.mkdir()
        self.git(repo, 'init', '-q')
        self.git(repo, 'config', 'user.email', 'test@example.invalid')
        self.git(repo, 'config', 'user.name', 'Build preflight test')
        return repo

    def commit(self, repo):
        self.git(repo, 'add', '-A')
        self.git(repo, 'commit', '-qm', 'fixture')

    def add_dependency(self, nested=False):
        dependency = self.repository('dependency')
        (dependency / 'source.c').write_text('/* pinned dependency */\n')
        self.commit(dependency)
        if nested:
            child = self.repository('nested')
            (child / 'source.c').write_text('/* recursive dependency */\n')
            self.commit(child)
            self.git(dependency, '-c', 'protocol.file.allow=always', 'submodule',
                     'add', '-q', str(child), 'nested')
            self.commit(dependency)
        self.git(self.host, '-c', 'protocol.file.allow=always', 'submodule',
                 'add', '-q', str(dependency), 'deps/core')
        self.git(self.host, '-c', 'protocol.file.allow=always', 'submodule',
                 'update', '--init', '--recursive')
        self.commit(self.host)
        self.required = self.git(self.host, 'rev-parse', 'HEAD')
        self.pin.write_text(self.required + '\n')
        return self.host / 'deps' / 'core'

    def run_build(self):
        self.probe.unlink(missing_ok=True)
        return subprocess.run(['sh', str(self.source / 'build.sh')],
                              env=self.env, text=True, capture_output=True)

    def assert_rejected(self):
        result = self.run_build()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.probe.exists(), result.stdout + result.stderr)
        self.assertTrue(result.stderr.strip(), 'Expected an actionable error')
        return result

    def assert_accepted(self):
        result = self.run_build()
        self.assertEqual(result.returncode, 91, result.stdout + result.stderr)
        self.assertTrue(self.probe.exists(), result.stdout + result.stderr)
        self.assertIn('-S ' + str(self.source.parent.parent), self.probe.read_text())

    def test_clean_pinned_checkout_reaches_first_cmake(self):
        self.assert_accepted()

    def test_wrong_head_is_rejected_before_cmake(self):
        (self.host / 'source.c').write_text('/* different revision */\n')
        self.commit(self.host)
        result = self.assert_rejected()
        self.assertIn(self.required, result.stderr)

    def test_unstaged_source_change_is_rejected(self):
        (self.host / 'CMakeLists.txt').write_text('# modified\n')
        self.assert_rejected()

    def test_staged_source_change_is_rejected(self):
        (self.host / 'CMakeLists.txt').write_text('# staged\n')
        self.git(self.host, 'add', 'CMakeLists.txt')
        self.assert_rejected()

    def test_untracked_source_is_rejected(self):
        (self.host / 'injected.c').write_text('/* untracked source */\n')
        self.assert_rejected()

    def test_ignored_build_artifacts_are_allowed(self):
        build = self.host / 'cmake-build-kyty'
        build.mkdir()
        (build / 'artifact.o').write_bytes(b'build artifact')
        self.assert_accepted()

    def test_missing_revision_file_is_rejected(self):
        self.pin.unlink()
        self.assert_rejected()

    def test_malformed_revision_file_is_rejected(self):
        for revision in ('', 'a' * 39, 'g' * 40, self.required + '\nextra'):
            with self.subTest(revision=revision):
                self.pin.write_text(revision)
                self.assert_rejected()

    def test_initialized_pinned_dependency_is_allowed(self):
        self.add_dependency()
        self.assert_accepted()

    def test_uninitialized_dependency_is_rejected(self):
        self.add_dependency()
        self.git(self.host, 'submodule', 'deinit', '-f', '--all')
        self.assert_rejected()

    def test_uninitialized_recursive_dependency_is_rejected(self):
        dependency = self.add_dependency(nested=True)
        self.git(dependency, 'submodule', 'deinit', '-f', '--all')
        self.assert_rejected()

    def test_wrong_dependency_revision_is_rejected(self):
        dependency = self.add_dependency()
        self.git(dependency, 'config', 'user.email', 'test@example.invalid')
        self.git(dependency, 'config', 'user.name', 'Build preflight test')
        (dependency / 'source.c').write_text('/* wrong revision */\n')
        self.commit(dependency)
        self.assert_rejected()

    def test_dirty_dependency_is_rejected(self):
        dependency = self.add_dependency()
        (dependency / 'source.c').write_text('/* modified dependency */\n')
        self.assert_rejected()


if __name__ == '__main__':
    unittest.main()

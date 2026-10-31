#!/usr/bin/env python3
"""Copy one consistent private streaming profile into a fresh upgrade profile.

Run only after the owning host is idle/stopped. This does not operate any host,
launcher, game or device. Identity and authentication files are copied verbatim.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import stat


REQUIRED = ('host-cert.pem', 'host-key.pem', 'host-state.json', 'host-auth.json',
            'controller-auth.json', 'streaming.ini')
OPTIONAL = ('companion/settings.json', 'companion/phones.json', 'controller-audio-clients.json')
MAX_FILE_BYTES = 16 * 1024 * 1024


class MigrationError(Exception):
    pass


def _absolute(path):
    return Path(os.path.abspath(Path(path).expanduser()))


def _no_symlinks(path):
    for component in (path, *path.parents):
        if component.is_symlink():
            raise MigrationError('Profile paths cannot contain symbolic links')


def _validate(info, directory, name, *, private=True):
    kind = stat.S_ISDIR if directory else stat.S_ISREG
    forbidden = 0o077 if private else 0o022
    if not kind(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & forbidden:
        raise MigrationError('Expected an owned, safely permissioned ' + name)
    if not directory and info.st_nlink != 1:
        raise MigrationError('Private profile files cannot have multiple hard links: ' + name)


def _signature(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def _snapshot(source):
    """Open each selected file without following links and detect changes while reading."""
    result = {'directories': {}, 'files': {}}
    root = os.open(source, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    companion = None
    try:
        info = os.fstat(root)
        _validate(info, True, 'source directory')
        result['directories']['.'] = _signature(info)
        try:
            companion = os.open('companion', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root)
        except FileNotFoundError:
            pass
        if companion is not None:
            info = os.fstat(companion)
            _validate(info, True, 'companion directory')
            result['directories']['companion'] = _signature(info)
        for name in REQUIRED + OPTIONAL:
            parent = companion if name.startswith('companion/') else root
            if parent is None:
                continue
            try:
                descriptor = os.open(name.rsplit('/', 1)[-1],
                                     os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
            except FileNotFoundError:
                if name in REQUIRED:
                    raise MigrationError('Missing required profile file: ' + name) from None
                continue
            try:
                before = os.fstat(descriptor)
                _validate(before, False, name)
                if before.st_size > MAX_FILE_BYTES:
                    raise MigrationError('Oversized private profile file: ' + name)
                with os.fdopen(os.dup(descriptor), 'rb') as stream:
                    data = stream.read(MAX_FILE_BYTES + 1)
                if len(data) > MAX_FILE_BYTES or _signature(before) != _signature(os.fstat(descriptor)):
                    raise MigrationError('Source changed while reading: ' + name)
                result['files'][name] = (data, _signature(before))
            finally:
                os.close(descriptor)
    finally:
        if companion is not None:
            os.close(companion)
        os.close(root)
    return result


def _opaque_key(key):
    lower = key.lower()
    return any(word in lower for word in ('token', 'password', 'secret', 'credential', 'bearer')) or \
        'auth' in re.split(r'[^a-z0-9]+', lower)


def _path_key(key):
    key = re.split(r'[/\\]', key.lower())[-1]
    endings = ('path', 'file', 'dir', 'directory', 'root', 'paths', 'files', 'dirs', 'directories', 'roots')
    return not _opaque_key(key) and (key in ('backend', 'profile', *endings) or
                                      key.endswith(tuple('_' + item for item in endings)))


def _relocate(value, relocations):
    for old, new in relocations:
        if value == old or value.startswith(old + '/'):
            return new + value[len(old):]
    return value


def _ini(data, relocations):
    lines = data.decode('utf-8').splitlines(keepends=True)
    for i, line in enumerate(lines):
        if '=' not in line or line.lstrip().startswith((';', '#', '[')):
            continue
        key, value = line.split('=', 1)
        if not _path_key(key.strip()):
            continue
        # Preserve comments, ordering, whitespace, quotes and all unrelated preferences.
        match = re.fullmatch(r'(\s*["\']?)(.*?)(["\']?\s*)', value, re.DOTALL)
        relocated = _relocate(match[2], relocations)
        lines[i] = key + '=' + match[1] + relocated + match[3]
    return ''.join(lines).encode('utf-8')


def _json_paths(value, relocations, path_context=False):
    if isinstance(value, dict):
        return {key: child if _opaque_key(key) else _json_paths(child, relocations, path_context or _path_key(key))
                for key, child in value.items()}
    if isinstance(value, list):
        return [_json_paths(child, relocations, path_context) for child in value]
    return _relocate(value, relocations) if path_context and isinstance(value, str) else value


def _write_private(path, data):
    path.parent.mkdir(mode=0o700, exist_ok=True)
    _validate(path.parent.lstat(), True, 'destination directory')
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'wb') as stream:
        os.fchmod(stream.fileno(), 0o600)
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def migrate(source, destination, old_prefix, new_prefix):
    source, destination, old_prefix, new_prefix = map(_absolute, (source, destination, old_prefix, new_prefix))
    _no_symlinks(source)
    _no_symlinks(destination)
    if destination.exists() or source == destination or source in destination.parents:
        raise MigrationError('A fresh destination outside the source profile is required')
    _validate(destination.parent.stat(), True, 'destination parent', private=False)
    snapshot = _snapshot(source)
    relocations = sorted(((str(source), str(destination)), (str(old_prefix), str(new_prefix))),
                         key=lambda item: len(item[0]), reverse=True)
    output = {}
    for name, (data, _) in snapshot['files'].items():
        try:
            if name == 'streaming.ini':
                data = _ini(data, relocations)
            elif name.endswith('.json'):
                value = json.loads(data)
                if not isinstance(value, (dict, list) if name in OPTIONAL else dict):
                    raise ValueError()
                if name in OPTIONAL:
                    relocated = _json_paths(value, relocations)
                    if relocated != value:
                        data = (json.dumps(relocated, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
        except (ValueError, UnicodeError):
            raise MigrationError('Invalid profile settings: ' + name) from None
        output[name] = data
    owned = None
    try:
        destination.mkdir(mode=0o700)
        destination.chmod(0o700)
        owned = destination.stat()
        for name, data in output.items():
            _write_private(destination / name, data)
        try:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(destination / 'host-cert.pem', destination / 'host-key.pem',
                                    password=lambda: b'')
        except ssl.SSLError:
            raise MigrationError('Host certificate and private key are not a valid matching pair') from None
        copied = _snapshot(destination)['files']
        if {name: value[0] for name, value in copied.items()} != output:
            raise MigrationError('Destination verification failed')
        if _snapshot(source) != snapshot:
            raise MigrationError('Source profile changed during migration; retry with its host stopped')
        digest = lambda data: hashlib.sha256(data).hexdigest()
        return {'files': {name: {'source_sha256': digest(snapshot['files'][name][0]),
                                 'destination_sha256': digest(data)} for name, data in output.items()}}
    except BaseException:
        if owned is not None and not destination.is_symlink():
            current = destination.stat()
            if (current.st_dev, current.st_ino) == (owned.st_dev, owned.st_ino):
                shutil.rmtree(destination)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'destination', 'old-prefix', 'new-prefix'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        result = migrate(args.source, args.destination, args.old_prefix, args.new_prefix)
    except MigrationError as error:
        parser.exit(1, 'Profile migration: ' + str(error) + '\n')
    except OSError:
        parser.exit(1, 'Profile migration: cannot safely access the private profile files\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

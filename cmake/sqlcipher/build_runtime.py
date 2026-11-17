"""Rebuild the pinned Windows x64 SQLCipher runtime in an isolated directory.

Run from an x64 MSVC Developer PowerShell with Python 3.10+ and CMake 3.28+.
This writes a reviewable candidate, never the checked-in runtime or a user DB.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--cmake', default='cmake.exe')
    args = parser.parse_args()
    recipe = Path(__file__).resolve().parent
    lock = json.loads((recipe / 'sources.lock.json').read_bytes())
    assert os.name == 'nt' and c.sizeof(c.c_void_p) == 8
    assert shutil.which('cl.exe') and shutil.which('nmake.exe'), 'Enter the x64 MSVC developer shell first'
    work, output, cache = (p.resolve() for p in (args.work_directory, args.output, args.cache))
    assert not work.exists() and not output.exists(), 'Preserve previous build/output evidence'
    work.mkdir(parents=True)
    cache.mkdir(parents=True, exist_ok=True)
    logs = work / 'logs'
    logs.mkdir()
    for source in lock['archives']:
        archive = cache / source['file']
        if not archive.exists():
            temporary = archive.with_suffix(archive.suffix + '.pending')
            with urllib.request.urlopen(source['url'], timeout=60) as incoming, temporary.open('xb') as out:
                shutil.copyfileobj(incoming, out)
            assert sha(temporary) == source['sha256'], 'Downloaded source/tool differs from its pin'
            temporary.rename(archive)
        assert sha(archive) == source['sha256'], 'Cached source/tool differs from its pin'
        destination = work / source['extractionDirectory']
        destination.mkdir(exist_ok=True)
        if archive.suffix == '.zip':
            with zipfile.ZipFile(archive) as bundle:
                for item in bundle.infolist():
                    assert (destination / item.filename).resolve().is_relative_to(destination)
                    if source['kind'] != 'perl' or item.filename.startswith(('perl/', 'c/bin/')) or '/' not in item.filename:
                        bundle.extract(item, destination)
        else:
            with tarfile.open(archive) as bundle:
                for item in bundle.getmembers():
                    assert (destination / item.name).resolve().is_relative_to(destination)
                    assert item.isfile() or item.isdir(), 'No archive links are required by this recipe'
                bundle.extractall(destination)

    environment = os.environ.copy()
    environment.update(LC_ALL='C', LC_CTYPE='C', LANG='C')
    environment['PATH'] = ';'.join(str(work / p) for p in
        ('perl/perl/bin', 'perl/c/bin', 'nasm/nasm-2.16.03')) + ';' + environment['PATH']

    def run(name, command, directory, extra=None):
        env = dict(environment, **(extra or {}))
        with (logs / (name + '.log')).open('xb') as log:
            result = subprocess.run([str(v) for v in command], cwd=directory, env=env,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        assert result.returncode == 0, f'{name} failed; inspect {logs / (name + ".log")}'

    openssl_source = work / 'openssl-3.5.9'
    openssl_build = work / 'openssl-build'
    openssl_build.mkdir()
    run('openssl-configure', [work / 'perl/perl/bin/perl.exe', openssl_source / 'Configure',
                             'VC-WIN64A', 'no-shared', 'no-module', 'no-legacy', 'no-engine'], openssl_build)
    run('openssl-build', ['nmake.exe', '/NOLOGO'], openssl_build)
    run('openssl-tests', ['nmake.exe', '/NOLOGO', 'test'], openssl_build,
        dict(TESTS='test_evp test_hmac test_rand test_evp_kdf test_ciphername'))
    sqlcipher_source = work / 'sqlcipher-355d4db92d3404e71faa8088d071a71d767807f4'
    amalgamation = work / 'amalgamation'
    amalgamation.mkdir()

    def short(path):
        # The bundled upstream JimTcl currently mishandles spaces when it
        # starts mksourceid.exe. Use an existing short name without changing
        # volume settings, drive mappings, or the developer's environment.
        if ' ' not in str(path):
            return str(path)
        kernel = c.WinDLL('kernel32', use_last_error=True)
        kernel.GetShortPathNameW.argtypes = [c.c_wchar_p, c.c_wchar_p, c.c_uint]
        buffer = c.create_unicode_buffer(32768)
        count = kernel.GetShortPathNameW(str(path), buffer, len(buffer))
        assert 0 < count < len(buffer) and ' ' not in buffer.value, 'Use a build work directory without spaces'
        return buffer.value

    run('sqlcipher-amalgamation', ['nmake.exe', '/NOLOGO', '/F', short(sqlcipher_source) + '\\Makefile.msc',
        'TOP=' + short(sqlcipher_source), 'EXT_FEATURE_FLAGS=-DSQLITE_TEMP_STORE=2 -DSQLITE_HAS_CODEC', 'sqlite3.c'], short(amalgamation))
    binary_build = work / 'runtime-build'
    run('runtime-configure', [args.cmake, '-S', recipe, '-B', binary_build, '-A', 'x64',
        '-DSQLCIPHER_AMALGAMATION=' + str(amalgamation), '-DOPENSSL_SOURCE_DIR=' + str(openssl_source),
        '-DOPENSSL_BUILD_DIR=' + str(openssl_build)], work)
    run('runtime-build', [args.cmake, '--build', binary_build, '--config', 'Release', '--parallel', '2'], work)
    dll = binary_build / 'Release/e_sqlcipher.dll'
    manifest = dict(status='BUILT_PENDING_PRODUCT_COMPATIBILITY_REVIEW', sources=lock,
        files=[dict(path='e_sqlcipher.dll', sha256=sha(dll), bytes=dll.stat().st_size)],
        amalgamationSha256=sha(amalgamation / 'sqlite3.c'),
        staticCryptoLibrarySha256=sha(openssl_build / 'libcrypto.lib'),
        buildRecipeSha256=sha(Path(__file__)), cmakeRecipeSha256=sha(recipe / 'CMakeLists.txt'),
        compatibilitySourceSha256=sha(recipe / 'win32_compat.c'),
        runtime='Windows x64, static CRT/OpenSSL, no external OpenSSL modules required',
        releaseApproved=False)
    output.mkdir(parents=True)
    shutil.copy2(dll, output / dll.name)
    (output / 'sqlcipher-runtime.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(dict(status=manifest['status'], output=str(output), sha256=sha(dll))))


if __name__ == '__main__':
    main()

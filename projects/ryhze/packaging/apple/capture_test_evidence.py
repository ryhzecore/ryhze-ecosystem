"""Preserve strict Flutter golden failures and opt-in geometry with their paths."""

import hashlib
import json
from pathlib import Path
import zipfile


def capture(root, output, *, byte_limit=100 * 1024 * 1024):
    root = Path(root).resolve()
    output = Path(output)
    files = []
    total = 0
    sources = [('test/failures', '.png'), ('test/golden-diagnostics', '.json'),
               ('build/apple-unsigned-validation/golden-candidates', '.png'),
               ('build/apple-unsigned-validation/golden-candidates', '.json')]
    for relative, suffix in sources:
        directory = root / relative
        if directory.is_symlink():
            raise ValueError('Linked test evidence directory')
        if not directory.exists():
            continue
        for path in sorted(directory.rglob('*')):
            if path.is_symlink():
                raise ValueError('Linked test evidence file')
            if not path.is_file() or path.suffix != suffix:
                continue
            total += path.stat().st_size
            if total > byte_limit:
                raise ValueError('Test evidence exceeds bounded archive size')
            files.append(path)
    # Preserve an earlier archive if someone reruns locally in the same directory.
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = {'format': 1, 'files': [], 'purpose': 'Diagnostic capture, not visual acceptance'}
    archive = zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED)
    actual_total = 0
    try:
        with archive:
            for path in files:
                # Recheck actual bytes: a file may grow after preflight stat.
                with path.open('rb') as stream:
                    data = stream.read(byte_limit - actual_total + 1)
                actual_total += len(data)
                if actual_total > byte_limit:
                    raise ValueError('Test evidence exceeds bounded archive size')
                name = path.relative_to(root).as_posix()
                archive.writestr(name, data)
                manifest['files'].append({'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
            archive.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n')
    except BaseException:
        # Only this newly created archive is removed; preexisting outputs fail
        # at the exclusive constructor above and never reach this handler.
        output.unlink(missing_ok=True)
        raise
    return manifest


if __name__ == '__main__':
    root = Path.cwd()
    output = root / 'build/apple-unsigned-validation/flutter-test-evidence.zip'
    manifest = capture(root, output)
    print(f'Preserved {len(manifest["files"])} visual diagnostic files in {output.relative_to(root)}')

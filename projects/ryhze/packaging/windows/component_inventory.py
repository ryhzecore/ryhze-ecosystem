"""Offline deterministic dependency/license inventory from locked build inputs.

This inventories dependencies and notices, not license permission or binary
provenance. Native runtime redistributables still require release-owner review.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlparse


def digest(data):
    return hashlib.sha256(data).hexdigest()


def locked_packages(text):
    result = {}
    current = None
    for line in text.splitlines():
        match = re.fullmatch(r"  ([a-zA-Z0-9_]+):", line)
        if match:
            current = result.setdefault(match[1], {})
        elif current is not None:
            match = re.fullmatch(r"    (source|version): (.+)", line)
            archive = re.fullmatch(r"      sha256: (.+)", line)
            if match:
                current[match[1]] = match[2].strip('"')
            elif archive:
                current['archive_sha256'] = archive[1].strip('"')
    return result


def make_inventory(config_path, deps_path, lock_path, sdk_path, fonts_dir, notices_path):
    config = json.loads(config_path.read_text(encoding='utf-8-sig'))
    deps = json.loads(deps_path.read_text(encoding='utf-8-sig'))
    sdk = json.loads(sdk_path.read_text(encoding='utf-8-sig'))
    locked = locked_packages(lock_path.read_text(encoding='utf-8'))
    packages = {p['name']: p for p in deps['packages']}
    roots = {p['name']: p['rootUri'] for p in config['packages']}
    if len(packages) != len(deps['packages']) or len(roots) != len(config['packages']):
        raise ValueError('Duplicate package identity')
    root = packages[deps['root']]
    runtime = set()
    queue = list(root['directDependencies'])
    while queue:
        name = queue.pop()
        if name in runtime:
            continue
        runtime.add(name)
        queue.extend(packages[name]['dependencies'])
    components = []
    notices = []
    for name, package in sorted(packages.items()):
        if name == deps['root']:
            continue
        expected = locked.get(name)
        if not expected or any(expected.get(k) != package[k] for k in ('version', 'source')):
            raise ValueError('Dependency graph does not match lockfile')
        if package['source'] == 'hosted' and not re.fullmatch(r'[0-9a-f]{64}', expected.get('archive_sha256', '')):
            raise ValueError('Missing locked hosted archive digest')
        uri = urlparse(roots[name])
        if uri.scheme not in ('', 'file') or uri.netloc:
            raise ValueError('Only local package roots may supply notices')
        path = unquote(uri.path)
        # file:///C:/... uses a leading slash that pathlib on Windows must omit.
        if re.match(r'^/[A-Za-z]:/', path):
            path = path[1:]
        base = Path(path) if uri.scheme else config_path.parent / path
        matches = [base / n for n in ('LICENSE', 'LICENSE.md', 'LICENSE.txt', 'COPYING') if (base / n).is_file()]
        if not matches and package['source'] == 'sdk':
            sdk_root = Path(sdk['flutterRoot']).resolve()
            if base.resolve().is_relative_to(sdk_root / 'packages') and (sdk_root / 'LICENSE').is_file():
                matches = [sdk_root / 'LICENSE']
        if not matches or any(p.is_symlink() for p in matches):
            raise ValueError(f'Missing or linked license notice: {name}')
        license_files = []
        for path in matches:
            data = path.read_bytes()
            if not data or len(data) > 2 * 1024 * 1024:
                raise ValueError('Invalid license notice size')
            text = data.decode('utf-8')
            license_files.append({'file': path.name, 'sha256': digest(data)})
            notices.append(f'===== {name} {package["version"]} / {path.name} =====\n{text.rstrip()}\n')
        components.append({'name': name, 'version': package['version'],
                           'source': package['source'], 'scope': 'runtime' if name in runtime else 'development',
                           'dependencies': sorted(package['dependencies']),
                           **({'archive_sha256': expected['archive_sha256']} if 'archive_sha256' in expected else {}),
                           'license_notices': license_files})
    assets = []
    for path in sorted(fonts_dir.glob('*-LICENSE.txt')):
        if path.is_symlink():
            raise ValueError('Linked font notice')
        data = path.read_bytes()
        notices.append(f'===== Font notice: {path.name} =====\n{data.decode("utf-8").rstrip()}\n')
        assets.append({'file': path.name, 'sha256': digest(data)})
    if not assets:
        raise ValueError('Font notices are required')
    if not notices_path.is_file() or notices_path.is_symlink():
        raise ValueError('Built Flutter notices are required')
    receipt = {'format': 1, 'application': {'name': root['name'], 'version': root['version']},
               'lock_sha256': digest(lock_path.read_bytes()),
               'sdk': {key: sdk[key] for key in ('frameworkRevision', 'engineRevision', 'dartSdkVersion')},
               'components': components, 'font_notices': assets,
               'built_notices_sha256': digest(notices_path.read_bytes()),
               'native_redistributable_review': 'required; this inventory does not infer distribution rights'}
    return receipt, '\n'.join(notices)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('package-config', 'dependencies', 'lockfile', 'sdk', 'fonts', 'notices', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    result, notices = make_inventory(args.package_config, args.dependencies, args.lockfile,
                                      args.sdk, args.fonts, args.notices)
    args.output.mkdir(exist_ok=False, parents=True)
    (args.output / 'components.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    (args.output / 'THIRD_PARTY_NOTICES.txt').write_text(notices, encoding='utf-8')


if __name__ == '__main__':
    main()

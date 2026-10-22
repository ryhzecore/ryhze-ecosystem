"""Read-only admission check for explicitly reviewed, repeated Mac PNG bytes.

This does not approve images, extract archives, update goldens, or establish any
native/release gate. Review/build attribution comes from the supplied records;
run-evidence mode also checks bundled source and toolchain records. Receipt hashes
identify the supplied files only;
they do not assert byte identity with an unavailable original Library document.
"""

import argparse
from dataclasses import dataclass
import hashlib
import io
import json
import math
from pathlib import Path
import re
import stat
import struct
import zipfile
import zlib


EXPECTED_COUNT = 74
EXPECTED_LINUX_COUNT = 100
MAX_ARCHIVE_BYTES = 100 * 1024 * 1024
MAX_MEMBER_BYTES = 16 * 1024 * 1024
MAX_UNPACKED_BYTES = 100 * 1024 * 1024
LINUX_MANIFEST = 'docs/evidence/apple-host-goldens-2026-10-07/source-inputs.json'
LINUX_EVIDENCE = 'docs/evidence/apple-mac-ci-followup-2026-10-07/linux-golden-evidence.zip'
CANDIDATE_ROOT = 'build/apple-unsigned-validation/golden-candidates/'
DIAGNOSTIC_ROOT = 'test/golden-diagnostics/'
CANDIDATE_STATUS = 'unreviewed candidate; not an approved reference'
APPROVAL_STATUS = 'visual_content_approved_for_host_specific_reference_admission'
RUN_ROOT = 'build/apple-unsigned-validation/'
RUN_METADATA_PATHS = frozenset(RUN_ROOT + name for name in (
    'acceptance-limits.txt', 'analyze.log', 'apple-packaging.log',
    'component-inventory/ios/THIRD_PARTY_NOTICES.txt',
    'component-inventory/ios/components.json',
    'component-inventory/macos/THIRD_PARTY_NOTICES.txt',
    'component-inventory/macos/components.json',
    'dependencies.json', 'dependencies.log', 'dependency-graph.log',
    'flutter-tests.log', 'flutter-version.json', 'font-hashes.json',
    'format.log', 'ios-inventory.log', 'ios-package.log', 'ios-unsigned.log',
    'macos-bundle.log', 'macos-config.log', 'macos-inventory.log',
    'macos-privacy.log', 'macos-unsigned.log', 'macos.log', 'phase-status.json',
    'sdk-pin.log', 'sdk.log', 'shared-packaging.log', 'source-commit.txt',
    'source-status.txt', 'xcode.log',
))
SDK_PINS = {
    'frameworkVersion': '3.47.6',
    'flutterVersion': '3.47.6',
    'dartSdkVersion': '3.13.5',
    'frameworkRevision': '5fc346839b5d0eef006ed8404392afb4dfae428d',
    'engineRevision': '692136cb6582dbfc5af3fb33c2515a069f2f66d0',
    'engineContentHash': 'b8c8d3d8d5d0095127057f8a29ca8cc53da2167c',
}
FONT_PINS = {
    'Inter': 'c4ca027783cf047288e72f973dba8a5a2eec6fbde4d3c32f8692f860cd6c3870',
    'SpaceGrotesk': '9399a925822d52f14d1ac5dbec3c3b59a44d839df00817cd31b75091226bb1c0',
    'MaterialIcons': 'd9865b671a09d683d13a863089d8825e0f61a37696ce5d7d448bc8023aa62453',
}


@dataclass(frozen=True)
class Admission:
    report: dict
    images: dict


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _hash(value):
    _require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value),
             'Invalid SHA-256 value')
    return value


def _object(value):
    _require(type(value) is dict, 'Expected JSON object')
    return value


def _integer(value, expected=None):
    _require(type(value) is int and value >= 0, 'Expected nonnegative integer')
    if expected is not None:
        _require(value == expected, 'Unexpected integer value')
    return value


def _text(value):
    _require(isinstance(value, str) and bool(value.strip()), 'Expected nonempty text')
    return value


def _json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            _require(key not in result, 'Duplicate JSON key')
            result[key] = value
        return result

    def constant(_):
        raise ValueError('Non-finite JSON number')

    def finite_float(value):
        number = float(value)
        _require(math.isfinite(number), 'Non-finite JSON number')
        return number

    try:
        return _object(json.loads(data.decode('utf-8'), object_pairs_hook=pairs,
                                  parse_constant=constant, parse_float=finite_float))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError('Malformed JSON evidence') from error


def _path(name):
    _require(isinstance(name, str) and 0 < len(name) <= 512
             and re.fullmatch(r'[A-Za-z0-9_./-]+', name)
             and all(part not in ('', '.', '..') for part in name.split('/')),
             'Unsafe evidence path')
    return name


def _canonical(key):
    _path(key)
    _require(key.startswith('goldens/') and key.endswith('.png')
             and not key.startswith('goldens/hosts/'), 'Invalid canonical golden key')
    return key


def _read(path, limit):
    path = Path(path)
    _require(not path.is_symlink() and path.is_file(), 'Missing or linked evidence file')
    _require(path.stat().st_size <= limit, 'Evidence file exceeds size limit')
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    _require(len(data) <= limit, 'Evidence file exceeds size limit')
    return data


def _repo_file(root, relative):
    _path(relative)
    current = root
    for part in relative.split('/'):
        current = current / part
        _require(not current.is_symlink(), 'Linked repository evidence path')
    return current


def _archive(path, expected_sha=None, *, archive_format='capture-manifest'):
    _require(archive_format in ('capture-manifest', 'run-evidence'), 'Unknown archive format')
    raw = _read(path, MAX_ARCHIVE_BYTES)
    digest = _sha(raw)
    if expected_sha is not None:
        _require(digest == _hash(expected_sha), 'Archive SHA-256 does not match supplied pin')
    members = {}
    folded = set()
    path_spellings = {}
    directories = set()
    total = 0
    try:
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            _require(len(archive.infolist()) <= 4096, 'Too many ZIP entries')
            for entry in archive.infolist():
                is_directory = entry.is_dir()
                name = _path(entry.filename[:-1] if is_directory else entry.filename)
                _require(entry.filename == entry.orig_filename, 'Ambiguous ZIP filename')
                _require(name.casefold() not in folded, 'Duplicate or case-colliding ZIP entry')
                folded.add(name.casefold())
                parts = name.split('/')
                for index in range(1, len(parts) + 1):
                    prefix = '/'.join(parts[:index])
                    previous = path_spellings.setdefault(prefix.casefold(), prefix)
                    _require(previous == prefix, 'Case-colliding ZIP path component')
                mode = stat.S_IFMT(entry.external_attr >> 16)
                _require(not entry.flag_bits & 1
                         and entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED),
                         'Unsupported ZIP encoding')
                _require(0 <= entry.file_size <= MAX_MEMBER_BYTES, 'ZIP member exceeds size limit')
                if is_directory:
                    _require(archive_format == 'run-evidence' and mode in (0, stat.S_IFDIR)
                             and entry.file_size == 0, 'Invalid or disallowed ZIP directory')
                    directories.add(name)
                    continue
                _require(mode in (0, stat.S_IFREG), 'Linked or special ZIP entry')
                total += entry.file_size
                _require(total <= MAX_UNPACKED_BYTES, 'ZIP exceeds unpacked size limit')
                with archive.open(entry) as stream:
                    data = stream.read(MAX_MEMBER_BYTES + 1)
                _require(len(data) == entry.file_size, 'ZIP member size mismatch')
                members[name] = data
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError, zlib.error) as error:
        raise ValueError('Invalid ZIP evidence') from error
    for name in set(members) | directories:
        parts = name.split('/')
        _require(all('/'.join(parts[:index]) not in members for index in range(1, len(parts))),
                 'ZIP file/directory path conflict')
    _require(all(any(name.startswith(directory + '/') for name in members)
                 for directory in directories), 'Unrelated empty ZIP directory')
    if archive_format == 'run-evidence':
        # These transferred original run bundles have no internal hash manifest.
        # Their external digest and reviewed PNG hashes are separate checks.
        return members, digest
    _require('manifest.json' in members, 'Missing ZIP hash manifest')
    manifest = _json(members.pop('manifest.json'))
    _integer(manifest.get('format'), 1)
    records = manifest.get('files')
    _require(type(records) is list, 'Missing ZIP manifest file list')
    seen = set()
    for record in records:
        record = _object(record)
        name = _path(record.get('path'))
        _require(name not in seen and name in members, 'Duplicate or missing manifest member')
        seen.add(name)
        _integer(record.get('bytes'), len(members[name]))
        _require(_hash(record.get('sha256')) == _sha(members[name]), 'ZIP manifest SHA mismatch')
    _require(seen == set(members), 'Unlisted ZIP member')
    return members, digest


def _run_evidence(members, expected_source):
    captures = {name: data for name, data in members.items()
                if name.startswith(CANDIDATE_ROOT) or name.startswith(DIAGNOSTIC_ROOT)}
    _require(set(members) - set(captures) == RUN_METADATA_PATHS,
             'Unexpected or missing run evidence metadata file')
    _require(members[RUN_ROOT + 'source-commit.txt'] == (expected_source + '\n').encode('ascii'),
             'Run evidence source commit mismatch')
    try:
        status_lines = members[RUN_ROOT + 'source-status.txt'].decode('ascii').splitlines()
    except UnicodeError as error:
        raise ValueError('Invalid run source status') from error
    expected_status = {
        '# branch.oid ' + expected_source,
        '# branch.head ci/apple-unsigned-validation-2026-10-07',
        '# branch.upstream origin/ci/apple-unsigned-validation-2026-10-07',
        '# branch.ab +0 -0',
    }
    _require(len(status_lines) == 4 and set(status_lines) == expected_status,
             'Run evidence source status is dirty or does not match its source')
    sdk = _json(members[RUN_ROOT + 'flutter-version.json'])
    _require(all(sdk.get(name) == value for name, value in SDK_PINS.items()),
             'Run evidence SDK pin mismatch')
    fonts = _json(members[RUN_ROOT + 'font-hashes.json'])
    _require(fonts == FONT_PINS, 'Run evidence font pin mismatch')
    _require(members[RUN_ROOT + 'xcode.log'] == b'Xcode 26.6\nBuild version 17F113\n',
             'Run evidence Xcode pin mismatch')
    return captures, {
        'source_commit': expected_source,
        'recorded_source_status_clean': True,
        'sdk_pins': dict(SDK_PINS),
        'font_hashes': fonts,
        'xcode': '26.6 (17F113)',
        'metadata_file_sha256': {name: _sha(members[name]) for name in sorted(RUN_METADATA_PATHS)},
    }


def _png(data):
    """Check PNG framing, all chunk CRCs, and dimensions without rewriting bytes."""
    _require(data.startswith(b'\x89PNG\r\n\x1a\n'), 'Invalid PNG signature')
    offset = 8
    dimensions = None
    image_data = False
    ended = False
    while offset < len(data):
        _require(offset + 12 <= len(data), 'Truncated PNG chunk')
        length, kind = struct.unpack_from('>I4s', data, offset)
        end = offset + 12 + length
        _require(end <= len(data), 'Truncated PNG payload')
        payload = data[offset + 8:end - 4]
        crc = struct.unpack_from('>I', data, end - 4)[0]
        _require(zlib.crc32(kind + payload) & 0xffffffff == crc, 'PNG chunk CRC mismatch')
        if dimensions is None:
            _require(kind == b'IHDR' and length == 13, 'Missing PNG header')
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            _require(0 < width <= 32768 and 0 < height <= 32768
                     and width * height <= 100_000_000, 'Invalid PNG dimensions')
            depths = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}
            _require(depth in depths.get(color, ()) and compression == filtering == 0
                     and interlace in (0, 1), 'Invalid PNG header')
            dimensions = (width, height)
        else:
            _require(kind != b'IHDR', 'Duplicate PNG header')
        if kind == b'IDAT':
            image_data = True
        if kind == b'IEND':
            _require(length == 0 and end == len(data) and image_data, 'Invalid PNG end')
            ended = True
        offset = end
    _require(ended and dimensions is not None, 'Incomplete PNG')
    return dimensions


def _linux(root, manifest_path, evidence_path):
    raw = _read(manifest_path, MAX_MEMBER_BYTES)
    files = _object(_json(raw).get('files'))
    originals = {name: _hash(value) for name, value in files.items()
                 if name.startswith('test/goldens/') and name.endswith('.png')
                 and not name.startswith('test/goldens/hosts/')}
    _require(len(originals) == EXPECTED_LINUX_COUNT, 'Original Linux reference inventory changed')
    current = {path.relative_to(root).as_posix()
               for path in _repo_file(root, 'test/goldens').rglob('*.png')
               if not path.relative_to(root).as_posix().startswith('test/goldens/hosts/')}
    _require(current == set(originals), 'Original Linux reference paths changed')
    for name, expected in originals.items():
        _require(_sha(_read(_repo_file(root, name), MAX_MEMBER_BYTES)) == expected,
                 'Original Linux reference SHA mismatch')
    members, evidence_sha = _archive(evidence_path)
    keys = set()
    for name, data in members.items():
        diagnostic = _json(data)
        key = _canonical(diagnostic.get('goldenKey'))
        _require(name == DIAGNOSTIC_ROOT + key + '.json'
                 and diagnostic.get('goldenPath') == 'test/' + key
                 and _object(diagnostic.get('host')).get('os') == 'linux'
                 and key not in keys and 'test/' + key in originals,
                 'Invalid Linux active reference inventory')
        _integer(diagnostic.get('schema'), 1)
        keys.add(key)
    _require(len(keys) == EXPECTED_COUNT, 'Expected 74 active Linux reference keys')
    return originals, keys, _sha(raw), evidence_sha


def _build_url(value):
    _require(isinstance(value, str)
             and re.fullmatch(r'https://codemagic\.io/app/[0-9a-f]{24}/build/[0-9a-f]{24}', value),
             'Invalid build attribution URL')
    return value


def _receipts(approval, repeat, source, keys, originals):
    _require(isinstance(source, str) and re.fullmatch(r'[0-9a-f]{40}', source), 'Invalid source commit')
    for receipt in (approval, repeat):
        _integer(receipt.get('schema'), 1)
        _require(receipt.get('source_commit') == source, 'Source attribution mismatch')
        _require(receipt.get('baseline_mutations_performed') is False, 'Receipt reports reference mutations')
    _require(approval.get('status') == APPROVAL_STATUS, 'Visual approval is missing')
    _integer(approval.get('candidate_count'), EXPECTED_COUNT)
    first_build = _build_url(approval.get('build_url'))
    _require(_build_url(repeat.get('first_build_url')) == first_build, 'First build attribution mismatch')
    repeat_build = _build_url(repeat.get('repeat_build_url'))
    _require(repeat_build != first_build and repeat_build.split('/build/')[0] == first_build.split('/build/')[0],
             'Repeat must identify a distinct build of the same app')
    for name in ('candidate_count_first', 'candidate_count_repeat', 'byte_identical_count'):
        _integer(repeat.get(name), EXPECTED_COUNT)
    _require(repeat.get('signing_or_distribution_performed') is False, 'Unexpected distribution receipt')
    approved = {}
    candidates = approval.get('candidates')
    _require(type(candidates) is list, 'Missing approval entries')
    for row in candidates:
        row = _object(row)
        key = _canonical(row.get('canonical_key'))
        _require(key in keys and key not in approved, 'Unexpected or duplicate approved key')
        _require(row.get('mac_candidate_path') == 'macos/' + key.removeprefix('goldens/')
                 and row.get('reference_host') == row.get('capture_host') == 'macos',
                 'Approval host or path mismatch')
        _require(row.get('visual_review') == 'approved'
                 and row.get('candidate_status') == CANDIDATE_STATUS
                 and row.get('image_dimensions_match') is True
                 and row.get('geometry_collection_capped') is False, 'Incomplete visual approval')
        _text(row.get('test_description'))
        _require(_integer(row.get('width')) > 0 and _integer(row.get('height')) > 0,
                 'Invalid approved dimensions')
        _hash(row.get('candidate_sha256'))
        _require(_hash(row.get('linux_reference_sha256')) == originals['test/' + key],
                 'Approved Linux reference SHA mismatch')
        approved[key] = row
    _require(set(approved) == keys, 'Missing visual approval entries')
    repeats = repeat.get('candidates')
    _require(type(repeats) is list, 'Missing repeat entries')
    seen = set()
    for row in repeats:
        row = _object(row)
        candidate = _path(row.get('candidate'))
        _require(candidate.startswith('macos/'), 'Invalid repeated candidate host')
        key = _canonical('goldens/' + candidate.removeprefix('macos/'))
        _require(key in approved and key not in seen, 'Unexpected or duplicate repeated key')
        seen.add(key)
        _require(row.get('byte_identical') is True
                 and _hash(row.get('reference_sha256')) == approved[key]['candidate_sha256']
                 and _hash(row.get('repeat_sha256')) == approved[key]['candidate_sha256'],
                 'Repeat SHA does not match visual approval')
    _require(seen == keys, 'Missing repeat entries')
    return approved, first_build, repeat_build


def _candidate_archive(members, approved):
    expected = set()
    images = {}
    for key, row in approved.items():
        name = CANDIDATE_ROOT + row['mac_candidate_path']
        metadata_name = name + '.json'
        geometry_name = DIAGNOSTIC_ROOT + key + '.json'
        expected.update((name, metadata_name, geometry_name))
        _require(all(path in members for path in (name, metadata_name, geometry_name)),
                 'Missing candidate, metadata or geometry')
        data = members[name]
        _require(_sha(data) == row['candidate_sha256'], 'Candidate PNG differs from approved SHA')
        dimensions = (row['width'], row['height'])
        _require(_png(data) == dimensions, 'Candidate PNG dimensions mismatch')
        metadata = _json(members[metadata_name])
        _integer(metadata.get('schema'), 1)
        _require(metadata.get('status') == CANDIDATE_STATUS
                 and metadata.get('canonicalGoldenKey') == key
                 and metadata.get('strictReferenceKey') == 'goldens/hosts/' + row['mac_candidate_path']
                 and metadata.get('referenceHost') == metadata.get('capturedOnHost') == 'macos'
                 and metadata.get('testDescription') == row['test_description'],
                 'Candidate metadata attribution mismatch')
        _integer(metadata.get('pngBytes'), len(data))
        _text(metadata.get('dartVersion'))
        geometry = _json(members[geometry_name])
        _integer(geometry.get('schema'), 1)
        _require(geometry.get('goldenKey') == key and geometry.get('goldenPath') == 'test/' + key
                 and _object(geometry.get('host')).get('os') == 'macos'
                 and geometry.get('testDescription') == row['test_description']
                 and geometry.get('collectionCapped') is False, 'Geometry attribution mismatch')
        capture = _object(geometry.get('captureTarget'))
        size = _object(capture.get('size'))
        _require(capture.get('available') is True
                 and type(size.get('width')) in (int, float)
                 and type(size.get('height')) in (int, float)
                 and (size['width'], size['height']) == dimensions, 'Geometry capture dimensions mismatch')
        images['test/goldens/hosts/' + row['mac_candidate_path']] = data
    # The producer may retain expected strict-comparison failure PNGs as well.
    for name in set(members) - expected:
        _require(name.startswith('test/failures/') and name.endswith('.png'),
                 'Unexpected candidate archive member')
        _png(members[name])
    return images


def validate_admission(root, first_zip, repeat_zip, approval_path, repeatability_path,
                       *, expected_source, linux_manifest=None, linux_evidence=None,
                       first_archive_sha256=None, repeat_archive_sha256=None,
                       archive_format='capture-manifest'):
    """Return a report and exact reviewed PNG bytes; never mutate any input/tree."""
    try:
        root = Path(root).resolve(strict=True)
        originals, keys, linux_manifest_sha, linux_evidence_sha = _linux(
            root, linux_manifest or _repo_file(root, LINUX_MANIFEST),
            linux_evidence or _repo_file(root, LINUX_EVIDENCE))
        approval_bytes = _read(approval_path, MAX_MEMBER_BYTES)
        repeat_bytes = _read(repeatability_path, MAX_MEMBER_BYTES)
        approved, first_build, repeat_build = _receipts(
            _json(approval_bytes), _json(repeat_bytes), expected_source, keys, originals)
        first, first_sha = _archive(first_zip, first_archive_sha256, archive_format=archive_format)
        repeated, repeat_sha = _archive(repeat_zip, repeat_archive_sha256, archive_format=archive_format)
        run_evidence = None
        if archive_format == 'run-evidence':
            first, first_provenance = _run_evidence(first, expected_source)
            repeated, repeat_provenance = _run_evidence(repeated, expected_source)
            run_evidence = {'first': first_provenance, 'repeat': repeat_provenance}
        images = _candidate_archive(first, approved)
        repeat_images = _candidate_archive(repeated, approved)
        _require(images == repeat_images, 'Repeated PNG bytes are not identical')
        for key, row in approved.items():
            _require(_png(_read(_repo_file(root, 'test/' + key), MAX_MEMBER_BYTES))
                     == (row['width'], row['height']), 'Linux PNG dimensions mismatch')
        report = {
            'schema': 1,
            'status': 'validated_for_explicit_host_reference_admission',
            'source_commit': expected_source,
            'source_attribution': 'Supplied approval and repeatability records; candidate archives do not independently attest source.',
            'archive_format': archive_format,
            'internal_archive_manifest_present': archive_format == 'capture-manifest',
            'first_build_url': first_build,
            'repeat_build_url': repeat_build,
            'candidate_count': len(images),
            'linux_originals_verified': len(originals),
            'mutations_performed': False,
            'inputs': {
                'first_archive_sha256': first_sha,
                'repeat_archive_sha256': repeat_sha,
                'first_archive_externally_pinned': first_archive_sha256 is not None,
                'repeat_archive_externally_pinned': repeat_archive_sha256 is not None,
                'supplied_approval_file_sha256': _sha(approval_bytes),
                'supplied_repeatability_file_sha256': _sha(repeat_bytes),
                'linux_original_manifest_sha256': linux_manifest_sha,
                'linux_active_inventory_archive_sha256': linux_evidence_sha,
            },
            'receipt_identity_note': 'Receipt hashes identify supplied local files, including reconstructed logical reads; they do not establish original Library document byte identity.',
            'release_acceptance': 'Not established. Unsigned compilation, native XCTest, signed Keychain access, device acceptance and distribution remain separate gates.',
            'candidates': [
                {'canonical_key': key,
                 'destination': 'test/goldens/hosts/' + approved[key]['mac_candidate_path'],
                 'sha256': approved[key]['candidate_sha256'],
                 'linux_reference_sha256': approved[key]['linux_reference_sha256'],
                 'width': approved[key]['width'], 'height': approved[key]['height']}
                for key in sorted(keys)
            ],
        }
        if run_evidence is not None:
            report['source_attribution'] = (
                'Supplied approval and repeatability records agree with source-commit and clean '
                'source-status records in both original run bundles. These are recorded provenance, '
                'not cryptographic builder attestation.')
            report['run_evidence'] = run_evidence
            report['archive_integrity_note'] = (
                'The original run bundles contain no internal hash manifest. Measured or externally '
                'pinned whole-archive SHA-256 values and approved/repeated per-PNG hashes are checked '
                'separately; no original internal manifest is claimed.')
        return Admission(report=report, images=images)
    except OSError as error:
        raise ValueError('Could not read admission evidence') from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--first-zip', type=Path, required=True)
    parser.add_argument('--repeat-zip', type=Path, required=True)
    parser.add_argument('--approval', type=Path, required=True)
    parser.add_argument('--repeatability', type=Path, required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--linux-manifest', type=Path)
    parser.add_argument('--linux-evidence', type=Path)
    parser.add_argument('--first-archive-sha256')
    parser.add_argument('--repeat-archive-sha256')
    parser.add_argument('--archive-format', choices=('capture-manifest', 'run-evidence'),
                        default='capture-manifest')
    args = parser.parse_args()
    try:
        result = validate_admission(
            args.root, args.first_zip, args.repeat_zip, args.approval, args.repeatability,
            expected_source=args.source, linux_manifest=args.linux_manifest,
            linux_evidence=args.linux_evidence,
            first_archive_sha256=args.first_archive_sha256,
            repeat_archive_sha256=args.repeat_archive_sha256,
            archive_format=args.archive_format)
    except ValueError as error:
        parser.exit(1, f'Admission rejected: {error}\n')
    print(json.dumps(result.report, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()

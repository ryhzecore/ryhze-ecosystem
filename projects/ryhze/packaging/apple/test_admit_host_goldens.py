"""Admission boundary tests with disposable, synthetic evidence only."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile
import zlib

import admit_host_goldens as admission


SOURCE = '6' * 40
BASELINE_SOURCE = '1' * 40
FIRST_BUILD = 'https://codemagic.io/app/' + 'a' * 24 + '/build/' + 'b' * 24
REPEAT_BUILD = 'https://codemagic.io/app/' + 'a' * 24 + '/build/' + 'c' * 24
CANDIDATE_ROOT = 'build/apple-unsigned-validation/golden-candidates/'
CANDIDATE_STATUS = 'unreviewed candidate; not an approved reference'
APPROVED_STATUS = 'visual_content_approved_for_host_specific_reference_admission'
RUN_ROOT = 'build/apple-unsigned-validation/'
# The original run-3 and run-4 ZIPs have 30 receipt/log files, in addition to
# captures and geometry. These names were inspected from Git commit 08cf6aba.
RUN_METADATA_FILES = (
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
)
RUN_SDK = {
    'frameworkVersion': '3.47.6',
    'flutterVersion': '3.47.6',
    'frameworkRevision': '5fc346839b5d0eef006ed8404392afb4dfae428d',
    'dartSdkVersion': '3.13.5',
    'engineRevision': '692136cb6582dbfc5af3fb33c2515a069f2f66d0',
    'engineContentHash': 'b8c8d3d8d5d0095127057f8a29ca8cc53da2167c',
}
RUN_FONTS = {
    'Inter': 'c4ca027783cf047288e72f973dba8a5a2eec6fbde4d3c32f8692f860cd6c3870',
    'SpaceGrotesk': '9399a925822d52f14d1ac5dbec3c3b59a44d839df00817cd31b75091226bb1c0',
    'MaterialIcons': 'd9865b671a09d683d13a863089d8825e0f61a37696ce5d7d448bc8023aa62453',
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encode(value):
    return json.dumps(value, separators=(',', ':'), allow_nan=False).encode()


def png(seed=0, width=2, height=3):
    """A complete RGB PNG, including compressed scanlines and chunk CRCs."""
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))

    pixel = bytes((seed % 256, (seed * 13) % 256, (seed * 29) % 256))
    rows = (b'\x00' + pixel * width) * height
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def geometry(key, description, host):
    return {
        'schema': 1,
        'goldenKey': key,
        'goldenPath': 'test/' + key,
        'host': {'os': host},
        'testDescription': description,
        'collectionCapped': False,
        'captureTarget': {
            'available': True,
            'size': {'width': 2, 'height': 3},
        },
    }


def archive(path, members, *, attributes=None, manifest_mutator=None):
    manifest = {
        'format': 1,
        'purpose': 'Diagnostic capture, not visual acceptance',
        'files': [
            {'path': name, 'bytes': len(data), 'sha256': digest(data)}
            for name, data in sorted(members.items())
        ],
    }
    if manifest_mutator is not None:
        manifest_mutator(manifest)
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as output:
        for name, data in sorted(members.items()):
            if attributes and name in attributes:
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = attributes[name] << 16
                output.writestr(info, data)
            else:
                output.writestr(name, data)
        output.writestr('manifest.json', encode(manifest))


def run_evidence_members(fixture):
    """Equivalent original ZIP structure; never invent an internal manifest."""
    members = dict(fixture.members)
    members.update({RUN_ROOT + name: b'Synthetic receipt/log fixture.\n'
                    for name in RUN_METADATA_FILES})
    members[RUN_ROOT + 'source-commit.txt'] = (SOURCE + '\n').encode()
    members[RUN_ROOT + 'source-status.txt'] = (
        f'# branch.oid {SOURCE}\n'
        '# branch.head ci/apple-unsigned-validation-2026-10-07\n'
        '# branch.upstream origin/ci/apple-unsigned-validation-2026-10-07\n'
        '# branch.ab +0 -0\n').encode()
    members[RUN_ROOT + 'flutter-version.json'] = encode(RUN_SDK)
    members[RUN_ROOT + 'font-hashes.json'] = encode(RUN_FONTS)
    members[RUN_ROOT + 'xcode.log'] = b'Xcode 26.6\nBuild version 17F113\n'
    return members


def run_archive(path, members, *, extra_directories=(), attributes=None):
    directories = set(extra_directories)
    for name in members:
        parts = name.split('/')
        directories.update('/'.join(parts[:index]) + '/'
                           for index in range(1, len(parts)))
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as output:
        for name in sorted(directories):
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            mode = (attributes or {}).get(name, stat.S_IFDIR | 0o755)
            info.external_attr = (mode << 16) | 0x10
            output.writestr(info, b'')
        for name, data in sorted(members.items()):
            if attributes and name in attributes:
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = attributes[name] << 16
                output.writestr(info, data)
            else:
                output.writestr(name, data)


class AdmissionFixture:
    def __init__(self, base):
        self.root = base / 'repo'
        self.root.mkdir()
        inputs = base / 'inputs'
        inputs.mkdir()
        self.first_zip = inputs / 'first.zip'
        self.repeat_zip = inputs / 'repeat.zip'
        self.approval_path = inputs / 'approval.json'
        self.repeatability_path = inputs / 'repeatability.json'
        self.linux_manifest = inputs / 'linux-inputs.json'
        self.linux_evidence = inputs / 'linux-evidence.zip'
        self.keys = [f'goldens/synthetic/capture-{index:02}.png' for index in range(74)]
        self.members = {}
        self.images = {}
        self.linux_hashes = {}
        linux_diagnostics = {}
        approvals = []
        repeats = []
        for index, key in enumerate(self.keys):
            description = f'Synthetic capture {index} (variant: TargetPlatform.iOS)'
            image = png(index + 1)
            linux_image = png(index + 101)
            linux_path = self.root / 'test' / key
            linux_path.parent.mkdir(parents=True, exist_ok=True)
            linux_path.write_bytes(linux_image)
            self.linux_hashes['test/' + key] = digest(linux_image)
            linux_diagnostics['test/golden-diagnostics/' + key + '.json'] = encode(
                geometry(key, description, 'linux'))
            suffix = key.removeprefix('goldens/')
            candidate = 'macos/' + suffix
            name = CANDIDATE_ROOT + candidate
            self.members[name] = image
            self.members[name + '.json'] = encode({
                'schema': 1,
                'status': CANDIDATE_STATUS,
                'canonicalGoldenKey': key,
                'strictReferenceKey': 'goldens/hosts/' + candidate,
                'referenceHost': 'macos',
                'capturedOnHost': 'macos',
                'dartVersion': 'Synthetic fixture',
                'testDescription': description,
                'pngBytes': len(image),
                'sourceEvidence': 'Synthetic fixture; no native execution claimed.',
            })
            self.members['test/golden-diagnostics/' + key + '.json'] = encode(
                geometry(key, description, 'macos'))
            self.images['test/goldens/hosts/' + candidate] = image
            approvals.append({
                'canonical_key': key,
                'mac_candidate_path': candidate,
                'candidate_sha256': digest(image),
                'linux_reference_sha256': digest(linux_image),
                'image_dimensions_match': True,
                'width': 2,
                'height': 3,
                'changed_pixels': 6,
                'changed_percent': 100.0,
                'candidate_status': CANDIDATE_STATUS,
                'reference_host': 'macos',
                'capture_host': 'macos',
                'test_description': description,
                'geometry_collection_capped': False,
                'matches_previous_mac_run_pixels': True,
                'visual_review': 'approved',
                'linux_reference_visual_comparison':
                    'completed; no Mac-specific material layout regression',
                'geometry_context_review':
                    'completed; expected text scaling and deliberate scroll framing considered',
                'review_group': 'synthetic',
            })
            repeats.append({
                'candidate': candidate,
                'reference_sha256': digest(image),
                'repeat_sha256': digest(image),
                'byte_identical': True,
            })
        for index in range(26):
            path = f'test/goldens/historical/unused-{index:02}.png'
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(png(index + 201))
            self.linux_hashes[path] = digest(target.read_bytes())
        self.approval = {
            'schema': 1,
            'source_commit': SOURCE,
            'build_url': FIRST_BUILD,
            'scope': 'Synthetic receipt using the supplied visual-review schema.',
            'candidate_count': 74,
            'linux_reference_git_blob_verification':
                'All74 match exact source repository Git blob SHA',
            'candidates': approvals,
            'status': APPROVED_STATUS,
            'reviewed_at_utc': '2026-10-08T00:23:00Z',
            'baseline_mutations_performed': False,
            'independent_exact_source_mac_repeatability': 'required; not run',
            'release_acceptance': 'not established',
            'method': ['Synthetic test evidence, not visual approval of real pixels.'],
            'summary': 'Synthetic fixture.',
            'nonblocking_inherited_findings': [],
        }
        self.repeatability = {
            'schema': 1,
            'source_commit': SOURCE,
            'first_build_url': FIRST_BUILD,
            'repeat_build_url': REPEAT_BUILD,
            'comparison':
                'Exact bytes of every quarantined candidate PNG; no decoding, transformations or tolerance',
            'candidate_count_first': 74,
            'candidate_count_repeat': 74,
            'byte_identical_count': 74,
            'overall_build_status': 'failed: missing/unapproved host-reference gate retained',
            'test_counts': {'success': 253, 'failure': 15},
            'phase_status': {'schema': 1, 'phases': [], 'overall_exit_code': 1,
                             'scope': 'Synthetic fixture.'},
            'baseline_mutations_performed': False,
            'signing_or_distribution_performed': False,
            'remaining_gates': ['Strict host golden validation'],
            'candidates': repeats,
        }
        self.linux_manifest.write_bytes(encode({
            'source': BASELINE_SOURCE,
            'scope': 'Synthetic original reference inventory',
            'files': self.linux_hashes,
        }))
        archive(self.linux_evidence, linux_diagnostics)
        self.write_receipts()
        archive(self.first_zip, self.members)
        archive(self.repeat_zip, self.members)

    def write_receipts(self):
        self.approval_path.write_bytes(encode(self.approval))
        self.repeatability_path.write_bytes(encode(self.repeatability))

    def validate(self, **overrides):
        options = {
            'expected_source': SOURCE,
            'linux_manifest': self.linux_manifest,
            'linux_evidence': self.linux_evidence,
        }
        options.update(overrides)
        return admission.validate_admission(
            self.root, self.first_zip, self.repeat_zip,
            self.approval_path, self.repeatability_path, **options)


class AdmitHostGoldensTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.fixture = AdmissionFixture(self.base)

    def snapshot(self):
        return {str(path.relative_to(self.base)): digest(path.read_bytes())
                for path in self.base.rglob('*') if path.is_file()}

    def reject_without_writes(self, *, match=None, **options):
        before = self.snapshot()
        rejection = (self.assertRaisesRegex(ValueError, match) if match is not None
                     else self.assertRaises(ValueError))
        with rejection:
            self.fixture.validate(**options)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.fixture.root / 'test/goldens/hosts').exists())

    def members_with_image(self, name, image):
        metadata = json.loads(self.fixture.members[name + '.json'])
        metadata['pngBytes'] = len(image)
        return {**self.fixture.members, name: image, name + '.json': encode(metadata)}

    def test_valid_admission_returns_exact_bytes_without_writing_references(self):
        before = self.snapshot()
        result = self.fixture.validate(
            first_archive_sha256=digest(self.fixture.first_zip.read_bytes()),
            repeat_archive_sha256=digest(self.fixture.repeat_zip.read_bytes()))
        self.assertIsInstance(result, admission.Admission)
        self.assertEqual(result.images, self.fixture.images)
        self.assertEqual(len(result.images), 74)
        self.assertTrue(all(type(value) is bytes for value in result.images.values()))
        self.assertIsInstance(result.report, dict)
        self.assertEqual(result.report['schema'], 1)
        self.assertEqual(result.report['status'], 'validated_for_explicit_host_reference_admission')
        self.assertEqual(result.report['source_commit'], SOURCE)
        self.assertEqual(result.report['first_build_url'], FIRST_BUILD)
        self.assertEqual(result.report['repeat_build_url'], REPEAT_BUILD)
        self.assertEqual(result.report['candidate_count'], 74)
        self.assertEqual(result.report['linux_originals_verified'], 100)
        self.assertIs(result.report['mutations_performed'], False)
        self.assertEqual({row['canonical_key'] for row in result.report['candidates']},
                         set(self.fixture.keys))
        for row in result.report['candidates']:
            self.assertEqual(row['sha256'], digest(result.images[row['destination']]))
            self.assertEqual(row['linux_reference_sha256'],
                             self.fixture.linux_hashes['test/' + row['canonical_key']])
            self.assertEqual((row['width'], row['height']), (2, 3))
        json.dumps(result.report, allow_nan=False)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.fixture.root / 'test/goldens/hosts').exists())

    def test_archive_digest_pins_are_optional_but_enforced_when_supplied(self):
        self.assertEqual(len(self.fixture.validate().images), 74)
        for option in ['first_archive_sha256', 'repeat_archive_sha256']:
            with self.subTest(option=option):
                self.reject_without_writes(**{option: '0' * 64})

    def test_changed_active_and_inactive_linux_originals_are_rejected(self):
        for relative in ['test/' + self.fixture.keys[0],
                         'test/goldens/historical/unused-00.png']:
            with self.subTest(path=relative):
                target = self.fixture.root / relative
                original = target.read_bytes()
                target.write_bytes(png(255))
                self.reject_without_writes()
                target.write_bytes(original)

    def test_missing_linux_original_is_rejected(self):
        (self.fixture.root / 'test' / self.fixture.keys[0]).unlink()
        self.reject_without_writes()

    def test_linux_manifest_cannot_replace_the_active_key_inventory(self):
        self.fixture.approval['candidates'][0]['canonical_key'] = 'goldens/unknown.png'
        self.fixture.write_receipts()
        self.reject_without_writes()

    def test_receipt_duplicate_json_properties_are_rejected(self):
        for path in [self.fixture.approval_path, self.fixture.repeatability_path]:
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(b'{"schema":1,' + original[1:])
                self.reject_without_writes()
                path.write_bytes(original)

    def test_receipt_numeric_literals_cannot_overflow_to_nonfinite_floats(self):
        fields = [
            (self.fixture.approval_path, b'"changed_percent":100.0'),
            (self.fixture.repeatability_path, b'"overall_exit_code":1'),
        ]
        for path, field in fields:
            original = path.read_bytes()
            self.assertIn(field, original)
            for number in [b'1e999', b'-1e999']:
                with self.subTest(receipt=path.name, literal=number):
                    replacement = field.split(b':', 1)[0] + b':' + number
                    path.write_bytes(original.replace(field, replacement, 1))
                    self.reject_without_writes(match='Non-finite JSON number')
            path.write_bytes(original)

    def test_archive_duplicate_entry_is_rejected_even_when_bytes_match(self):
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(self.fixture.first_zip, 'a') as output:
                output.writestr(name, self.fixture.members[name])
        self.reject_without_writes()

    def test_case_colliding_zip_names_cannot_alias_a_candidate_on_macos(self):
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        alias = name.replace('capture-00.png', 'Capture-00.png')
        archive(self.fixture.first_zip,
                {**self.fixture.members, alias: self.fixture.members[name]})
        self.reject_without_writes(match='Duplicate or case-colliding ZIP entry')

    def test_zip_paths_cannot_escape_or_alias_a_canonical_member(self):
        for name in ['../escaped.png', '/absolute.png', 'C:/absolute.png',
                     'build\\escape.png', 'build/../escape.png',
                     'build//escape.png']:
            with self.subTest(path=name):
                archive(self.fixture.first_zip, {**self.fixture.members, name: png()})
                self.reject_without_writes()

    def test_zip_symlink_member_is_rejected(self):
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        archive(self.fixture.first_zip, self.fixture.members,
                attributes={name: stat.S_IFLNK | 0o777})
        self.reject_without_writes()

    def test_archive_reader_enforces_archive_member_and_unpacked_limits(self):
        # Exercise the bounded ZIP reader directly: a small member limit on the
        # full admission API would instead reject the earlier Linux JSON input.
        path = self.base / 'bounded.zip'
        members = {'evidence/first.bin': b'x' * 4096,
                   'evidence/second.bin': b'y' * 4096}
        archive(path, members)
        accepted, expected_digest = admission._archive(path)
        self.assertEqual(accepted, members)
        self.assertEqual(expected_digest, digest(path.read_bytes()))
        limits = [
            ('MAX_ARCHIVE_BYTES', path.stat().st_size - 1,
             'Evidence file exceeds size limit'),
            ('MAX_MEMBER_BYTES', 4095, 'ZIP member exceeds size limit'),
            ('MAX_UNPACKED_BYTES', 8191, 'ZIP exceeds unpacked size limit'),
        ]
        before = self.snapshot()
        for name, limit, message in limits:
            with self.subTest(limit=name), patch.object(admission, name, limit):
                with self.assertRaisesRegex(ValueError, message):
                    admission._archive(path)
                self.assertEqual(self.snapshot(), before)

    def test_manifest_hash_size_and_coverage_must_match_actual_archive(self):
        mutations = {
            'wrong hash': lambda value: value['files'][0].update(sha256='0' * 64),
            'wrong size': lambda value: value['files'][0].update(bytes=1),
            'missing entry': lambda value: value['files'].pop(),
            'duplicate entry': lambda value: value['files'].append(value['files'][0]),
        }
        for name, mutation in mutations.items():
            with self.subTest(case=name):
                archive(self.fixture.first_zip, self.fixture.members,
                        manifest_mutator=mutation)
                self.reject_without_writes()

    def test_self_consistent_archive_cannot_substitute_unapproved_pixels(self):
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        changed = self.members_with_image(name, png(255))
        archive(self.fixture.first_zip, changed)
        archive(self.fixture.repeat_zip, changed)
        self.reject_without_writes()

    def test_actual_repeat_bytes_override_a_false_identical_receipt_claim(self):
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        archive(self.fixture.repeat_zip, self.members_with_image(name, png(255)))
        self.reject_without_writes()

    def test_png_dimensions_are_checked_even_when_both_receipts_agree(self):
        # Both geometry records and approval claim transposed dimensions. The
        # complete, hash-approved PNG still encodes 2x3, so it must be rejected.
        self.fixture.approval['candidates'][0].update(width=3, height=2)
        self.fixture.write_receipts()
        name = 'test/golden-diagnostics/' + self.fixture.keys[0] + '.json'
        document = json.loads(self.fixture.members[name])
        document['captureTarget']['size'] = {'width': 3, 'height': 2}
        members = {**self.fixture.members, name: encode(document)}
        archive(self.fixture.first_zip, members)
        archive(self.fixture.repeat_zip, members)
        self.reject_without_writes(match='Candidate PNG dimensions mismatch')

    def test_png_crc_is_checked_after_all_supplied_hashes_are_updated(self):
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        damaged = bytearray(self.fixture.members[name])
        # Flip only an IHDR CRC byte; retain signature, dimensions and chunks.
        damaged[29] ^= 1
        damaged = bytes(damaged)
        new_hash = digest(damaged)
        self.fixture.approval['candidates'][0]['candidate_sha256'] = new_hash
        self.fixture.repeatability['candidates'][0].update(
            reference_sha256=new_hash, repeat_sha256=new_hash)
        self.fixture.write_receipts()
        members = self.members_with_image(name, damaged)
        archive(self.fixture.first_zip, members)
        archive(self.fixture.repeat_zip, members)
        self.reject_without_writes(match='PNG chunk CRC mismatch')

    def test_receipt_schema_counts_and_booleans_require_exact_types(self):
        original_approval = copy.deepcopy(self.fixture.approval)
        original_repeat = copy.deepcopy(self.fixture.repeatability)
        mutations = [
            ('approval', None, 'schema', True),
            ('approval', None, 'schema', 1.0),
            ('approval', None, 'candidate_count', 74.0),
            ('approval', None, 'baseline_mutations_performed', 0),
            ('approval', 0, 'width', True),
            ('approval', 0, 'width', 2.0),
            ('approval', 0, 'image_dimensions_match', 1),
            ('approval', 0, 'geometry_collection_capped', 0),
            ('repeatability', None, 'schema', True),
            ('repeatability', None, 'byte_identical_count', 74.0),
            ('repeatability', 0, 'byte_identical', 1),
        ]
        for document, index, field, value in mutations:
            with self.subTest(document=document, field=field, value=value):
                self.fixture.approval = copy.deepcopy(original_approval)
                self.fixture.repeatability = copy.deepcopy(original_repeat)
                record = getattr(self.fixture, document)
                if index is not None:
                    record = record['candidates'][index]
                record[field] = value
                self.fixture.write_receipts()
                self.reject_without_writes()

    def test_candidate_paths_cannot_be_swapped_between_approved_keys(self):
        first, second = self.fixture.approval['candidates'][:2]
        first['mac_candidate_path'], second['mac_candidate_path'] = (
            second['mac_candidate_path'], first['mac_candidate_path'])
        self.fixture.write_receipts()
        self.reject_without_writes()

    def test_partial_unapproved_or_duplicate_approval_is_rejected(self):
        original = copy.deepcopy(self.fixture.approval)
        mutations = {
            'partial': lambda value: value['candidates'].pop(),
            'duplicate key': lambda value: value['candidates'].__setitem__(
                1, copy.deepcopy(value['candidates'][0])),
            'unapproved entry': lambda value: value['candidates'][0].update(
                visual_review='pending'),
            'unapproved header': lambda value: value.update(status='pending'),
            'wrong count': lambda value: value.update(candidate_count=73),
        }
        for name, mutation in mutations.items():
            with self.subTest(case=name):
                self.fixture.approval = copy.deepcopy(original)
                mutation(self.fixture.approval)
                self.fixture.write_receipts()
                self.reject_without_writes()

    def test_source_and_independent_build_attribution_must_agree(self):
        original_approval = copy.deepcopy(self.fixture.approval)
        original_repeat = copy.deepcopy(self.fixture.repeatability)
        mutations = [
            ('approval source', 'approval', 'source_commit', '7' * 40),
            ('repeat source', 'repeatability', 'source_commit', '7' * 40),
            ('different first build', 'repeatability', 'first_build_url', REPEAT_BUILD),
            ('same build', 'repeatability', 'repeat_build_url', FIRST_BUILD),
            ('same build with fragment', 'repeatability', 'repeat_build_url',
             FIRST_BUILD + '#another-run'),
            ('same build with query', 'repeatability', 'repeat_build_url',
             FIRST_BUILD + '?another-run=1'),
        ]
        for name, document, field, value in mutations:
            with self.subTest(case=name):
                self.fixture.approval = copy.deepcopy(original_approval)
                self.fixture.repeatability = copy.deepcopy(original_repeat)
                getattr(self.fixture, document)[field] = value
                self.fixture.write_receipts()
                self.reject_without_writes()

    def test_incomplete_or_inconsistent_repeatability_receipt_is_rejected(self):
        original = copy.deepcopy(self.fixture.repeatability)
        mutations = {
            'partial': lambda value: value['candidates'].pop(),
            'duplicate': lambda value: value['candidates'].__setitem__(
                1, copy.deepcopy(value['candidates'][0])),
            'not identical': lambda value: value['candidates'][0].update(byte_identical=False),
            'claimed different digest': lambda value: value['candidates'][0].update(
                repeat_sha256='0' * 64),
            'Linux digest is not a candidate digest': lambda value:
                value['candidates'][0].update(
                    reference_sha256=self.fixture.approval['candidates'][0]['linux_reference_sha256'],
                    repeat_sha256=self.fixture.approval['candidates'][0]['linux_reference_sha256']),
            'wrong count': lambda value: value.update(byte_identical_count=73),
        }
        for name, mutation in mutations.items():
            with self.subTest(case=name):
                self.fixture.repeatability = copy.deepcopy(original)
                mutation(self.fixture.repeatability)
                self.fixture.write_receipts()
                self.reject_without_writes()

    def test_metadata_and_geometry_must_describe_the_approved_capture(self):
        prefix = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        diagnostic = 'test/golden-diagnostics/' + self.fixture.keys[0] + '.json'
        mutations = [
            (prefix + '.json', 'referenceHost', 'linux'),
            (prefix + '.json', 'capturedOnHost', 'linux'),
            (prefix + '.json', 'status', 'approved'),
            (prefix + '.json', 'canonicalGoldenKey', self.fixture.keys[1]),
            (prefix + '.json', 'strictReferenceKey', self.fixture.keys[0]),
            (prefix + '.json', 'pngBytes', 1),
            (prefix + '.json', 'testDescription', 'Unrelated capture'),
            (diagnostic, 'collectionCapped', True),
            (diagnostic, 'goldenKey', self.fixture.keys[1]),
            (diagnostic, 'goldenPath', 'test/' + self.fixture.keys[1]),
            (diagnostic, 'host', {'os': 'linux'}),
            (diagnostic, 'captureTarget', {'size': {'width': 3, 'height': 2}}),
        ]
        for name, field, value in mutations:
            with self.subTest(member=name, field=field):
                document = json.loads(self.fixture.members[name])
                document[field] = value
                members = {**self.fixture.members, name: encode(document)}
                archive(self.fixture.first_zip, members)
                self.reject_without_writes()

    def test_missing_and_additional_candidate_pngs_are_rejected(self):
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        for label, members in [
            ('missing', {key: value for key, value in self.fixture.members.items()
                         if key != name}),
            ('additional', {**self.fixture.members,
                            CANDIDATE_ROOT + 'macos/synthetic/extra.png': png()}),
        ]:
            with self.subTest(case=label):
                archive(self.fixture.first_zip, members)
                self.reject_without_writes()

    def test_run_evidence_is_explicit_readonly_and_has_no_internal_manifest(self):
        members = run_evidence_members(self.fixture)
        self.assertEqual(len(RUN_METADATA_FILES), 30)
        run_archive(self.fixture.first_zip, members)
        run_archive(self.fixture.repeat_zip, members)
        with zipfile.ZipFile(self.fixture.first_zip) as original:
            self.assertNotIn('manifest.json', original.namelist())
            self.assertTrue(any(entry.is_dir() for entry in original.infolist()))
        before = self.snapshot()
        result = self.fixture.validate(
            archive_format='run-evidence',
            first_archive_sha256=digest(self.fixture.first_zip.read_bytes()),
            repeat_archive_sha256=digest(self.fixture.repeat_zip.read_bytes()))
        self.assertEqual(result.images, self.fixture.images)
        self.assertEqual(result.report['archive_format'], 'run-evidence')
        self.assertIs(result.report['internal_archive_manifest_present'], False)
        self.assertEqual(set(result.report['run_evidence']), {'first', 'repeat'})
        self.assertEqual(result.report['linux_originals_verified'], 100)
        self.assertIs(result.report['mutations_performed'], False)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.fixture.root / 'test/goldens/hosts').exists())

    def test_archive_formats_cannot_be_autodetected_or_substituted(self):
        # The original default remains manifest-driven; selecting run evidence
        # must not quietly relax manifest requirements on a capture archive.
        self.reject_without_writes(archive_format='run-evidence')
        self.reject_without_writes(archive_format='auto')
        members = run_evidence_members(self.fixture)
        run_archive(self.fixture.first_zip, members)
        run_archive(self.fixture.repeat_zip, members)
        self.reject_without_writes()
        self.reject_without_writes(archive_format='capture-manifest')

    def test_cli_run_evidence_option_is_required_and_reports_without_writing(self):
        members = run_evidence_members(self.fixture)
        run_archive(self.fixture.first_zip, members)
        run_archive(self.fixture.repeat_zip, members)
        arguments = [
            'admit_host_goldens.py',
            '--root', str(self.fixture.root),
            '--first-zip', str(self.fixture.first_zip),
            '--repeat-zip', str(self.fixture.repeat_zip),
            '--approval', str(self.fixture.approval_path),
            '--repeatability', str(self.fixture.repeatability_path),
            '--source', SOURCE,
            '--linux-manifest', str(self.fixture.linux_manifest),
            '--linux-evidence', str(self.fixture.linux_evidence),
        ]
        before = self.snapshot()
        standard, errors = io.StringIO(), io.StringIO()
        with patch('sys.argv', arguments), redirect_stdout(standard), redirect_stderr(errors):
            with self.assertRaises(SystemExit) as rejected:
                admission.main()
        self.assertEqual(rejected.exception.code, 1)
        self.assertEqual(standard.getvalue(), '')
        self.assertIn('Admission rejected:', errors.getvalue())
        with patch('sys.argv', arguments + ['--archive-format', 'run-evidence']), \
                redirect_stdout(standard), redirect_stderr(errors):
            admission.main()
        report = json.loads(standard.getvalue())
        self.assertEqual(report['archive_format'], 'run-evidence')
        self.assertEqual(report['candidate_count'], 74)
        self.assertIs(report['mutations_performed'], False)
        self.assertEqual(self.snapshot(), before)

    def test_run_evidence_requires_exact_metadata_files_without_nested_archives(self):
        original = run_evidence_members(self.fixture)
        run_archive(self.fixture.repeat_zip, original)
        cases = {
            'missing receipt': {name: data for name, data in original.items()
                                if name != RUN_ROOT + 'analyze.log'},
            'extra receipt': {**original, RUN_ROOT + 'additional.log': b'additional'},
            'invented manifest': {**original, 'manifest.json': encode({'format': 1})},
            'nested capture zip': {**original, RUN_ROOT + 'flutter-test-evidence.zip': b'PK'},
        }
        for name, members in cases.items():
            with self.subTest(case=name):
                run_archive(self.fixture.first_zip, members)
                self.reject_without_writes(archive_format='run-evidence')

    def test_run_evidence_source_receipts_cannot_spoof_a_clean_expected_source(self):
        original = run_evidence_members(self.fixture)
        run_archive(self.fixture.repeat_zip, original)
        status = original[RUN_ROOT + 'source-status.txt']
        cases = [
            ('different commit', 'source-commit.txt', ('7' * 40 + '\n').encode()),
            ('missing commit newline', 'source-commit.txt', SOURCE.encode()),
            ('different status OID', 'source-status.txt',
             status.replace(SOURCE.encode(), b'7' * 40)),
            ('duplicate status OID', 'source-status.txt',
             status + f'# branch.oid {SOURCE}\n'.encode()),
            ('untracked file', 'source-status.txt', status + b'? untracked.txt\n'),
            ('modified tracked file', 'source-status.txt',
             status + b'1 .M N... 100644 100644 100644 abc abc lib/app.dart\n'),
            ('wrong branch', 'source-status.txt',
             status.replace(b'# branch.head ci/', b'# branch.head other/')),
            ('ahead of source', 'source-status.txt', status.replace(b'+0 -0', b'+1 -0')),
        ]
        for name, path, data in cases:
            with self.subTest(case=name):
                members = {**original, RUN_ROOT + path: data}
                run_archive(self.fixture.first_zip, members)
                self.reject_without_writes(archive_format='run-evidence')

    def test_run_evidence_pins_sdk_engine_fonts_and_xcode_in_each_run(self):
        original = run_evidence_members(self.fixture)
        cases = []
        for key in RUN_SDK:
            sdk = {**RUN_SDK, key: 'unexpected'}
            cases.append(('SDK ' + key, 'flutter-version.json', encode(sdk)))
        for font in RUN_FONTS:
            fonts = {**RUN_FONTS, font: '0' * 64}
            cases.append(('font ' + font, 'font-hashes.json', encode(fonts)))
        cases.extend([
            ('missing font', 'font-hashes.json', encode({'Inter': RUN_FONTS['Inter']})),
            ('extra font', 'font-hashes.json', encode({**RUN_FONTS, 'Unknown': '0' * 64})),
            ('Xcode build', 'xcode.log', b'Xcode 26.6\nBuild version OTHER\n'),
        ])
        for changed_zip in [self.fixture.first_zip, self.fixture.repeat_zip]:
            for name, path, data in cases:
                with self.subTest(run=changed_zip.name, case=name):
                    run_archive(self.fixture.first_zip, original)
                    run_archive(self.fixture.repeat_zip, original)
                    run_archive(changed_zip, {**original, RUN_ROOT + path: data})
                    self.reject_without_writes(archive_format='run-evidence')

    def test_run_evidence_still_requires_approved_identical_candidate_bytes(self):
        original = run_evidence_members(self.fixture)
        run_archive(self.fixture.first_zip, original)
        name = CANDIDATE_ROOT + 'macos/synthetic/capture-00.png'
        changed_capture = self.members_with_image(name, png(255))
        run_archive(self.fixture.repeat_zip, {**original, **changed_capture})
        self.reject_without_writes(archive_format='run-evidence')

    def test_run_archive_pin_detects_alteration_of_an_allowed_log(self):
        original = run_evidence_members(self.fixture)
        run_archive(self.fixture.first_zip, original)
        run_archive(self.fixture.repeat_zip, original)
        expected_hash = digest(self.fixture.first_zip.read_bytes())
        run_archive(self.fixture.first_zip,
                    {**original, RUN_ROOT + 'analyze.log': b'Altered diagnostic bytes.\n'})
        self.reject_without_writes(archive_format='run-evidence',
                                   first_archive_sha256=expected_hash)

    def test_run_zip_directory_and_file_names_share_one_unambiguous_namespace(self):
        path = self.base / 'directory-conflict.zip'
        cases = [
            [('root/', b'', stat.S_IFDIR), ('root', b'x', stat.S_IFREG)],
            [('Root/', b'', stat.S_IFDIR), ('root/item', b'x', stat.S_IFREG)],
            [('root', b'x', stat.S_IFREG), ('root/item', b'y', stat.S_IFREG)],
            [('root//', b'', stat.S_IFDIR), ('root/item', b'x', stat.S_IFREG)],
            [('root/./', b'', stat.S_IFDIR), ('root/item', b'x', stat.S_IFREG)],
            [('../escape/', b'', stat.S_IFDIR)],
            [('root/', b'', stat.S_IFREG), ('root/item', b'x', stat.S_IFREG)],
            [('root', b'', stat.S_IFDIR), ('root/item', b'x', stat.S_IFREG)],
            [('root/', b'nonempty', stat.S_IFDIR), ('root/item', b'x', stat.S_IFREG)],
            [('root/', b'', stat.S_IFLNK), ('root/item', b'x', stat.S_IFREG)],
            [('root/item', b'x', stat.S_IFIFO)],
            [('unrelated/', b'', stat.S_IFDIR), ('root/item', b'x', stat.S_IFREG)],
        ]
        for index, entries in enumerate(cases):
            for reverse in [False, True]:
                with self.subTest(case=index, reverse=reverse):
                    with zipfile.ZipFile(path, 'w') as output:
                        for name, data, mode in (reversed(entries) if reverse else entries):
                            info = zipfile.ZipInfo(name)
                            info.create_system = 3
                            info.external_attr = (mode | 0o755) << 16
                            output.writestr(info, data)
                    before = self.snapshot()
                    with self.assertRaises(ValueError):
                        admission._archive(path, archive_format='run-evidence')
                    self.assertEqual(self.snapshot(), before)


if __name__ == '__main__':
    unittest.main()

"""Bounded current-launch Flutter evidence; not a pixel/physical-device verdict."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import struct
import sys
import time
import zlib


MARKER = Path('Library/Caches/ryhze-first-frame-v1.json')
NONCE = re.compile(r'[0-9a-f]{32}')
FAILURES = {'bootstrap-degraded', 'bootstrap-failed', 'flutter-error',
            'async-error', 'shell-layout', 'raster-timeout', 'probe-failed'}
class ProbeFailure(ValueError):
    """Fixed diagnostic code; never constructed from raw input."""


FIELDS = {'schemaVersion', 'nonce', 'processID', 'state', 'bootstrap',
          'shellLayout', 'firstFrameRasterized', 'failureCode'}


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ProbeFailure('ambiguous_evidence')
        value[key] = item
    return value


def read_regular(path, limit):
    """Never block on a special file or read unbounded diagnostic contents."""
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > limit:
            raise ProbeFailure('invalid_evidence_file')
        data = os.read(descriptor, limit + 1)
        if len(data) > limit:
            raise ProbeFailure('invalid_evidence_file')
        return data
    finally:
        os.close(descriptor)


def prepare(path):
    nonce = secrets.token_hex(16)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as output:
        json.dump({'schemaVersion': 1, 'nonce': nonce}, output)
        output.write('\n')
    return nonce


def context(request, launch_log, container_file):
    value = json.loads(read_regular(request, 4096), object_pairs_hook=unique_object)
    if (not isinstance(value, dict) or set(value) != {'schemaVersion', 'nonce'}
            or type(value['schemaVersion']) is not int or value['schemaVersion'] != 1
            or not isinstance(value['nonce'], str) or not NONCE.fullmatch(value['nonce'])):
        raise ProbeFailure('invalid_request')
    launch = read_regular(launch_log, 65536).decode('utf-8')
    matches = re.findall(r'^com\.ryhze\.ryhze: ([1-9][0-9]*)\s*$', launch, re.MULTILINE)
    if len(matches) != 1 or int(matches[0]) > 2147483647:
        raise ProbeFailure('invalid_launch_identity')
    container = read_regular(container_file, 16384).decode('utf-8').strip()
    if not container or '\n' in container or '\r' in container or '\x00' in container:
        raise ProbeFailure('invalid_container')
    directory = Path(container)
    if not directory.is_absolute() or not directory.is_dir():
        raise ProbeFailure('invalid_container')
    directory = directory.resolve(strict=True)
    marker = directory / MARKER
    for part in (marker.parent.parent, marker.parent):
        if part.is_symlink():
            raise ProbeFailure('invalid_marker_path')
    return value['nonce'], int(matches[0]), marker


def inspect_marker(path, nonce, process_id):
    try:
        value = json.loads(read_regular(path, 4096), object_pairs_hook=unique_object)
    except FileNotFoundError:
        return 'missing', None
    if (not isinstance(value, dict) or set(value) != FIELDS
            or type(value['schemaVersion']) is not int or value['schemaVersion'] != 1
            or not isinstance(value['nonce'], str) or not NONCE.fullmatch(value['nonce'])
            or type(value['processID']) is not int or not 0 < value['processID'] <= 2147483647
            or value['state'] not in ('starting', 'ready', 'failed')
            or value['bootstrap'] not in ('pending', 'healthy', 'degraded')
            or type(value['shellLayout']) is not bool
            or type(value['firstFrameRasterized']) is not bool
            or value['failureCode'] not in (None, *FAILURES)):
        raise ProbeFailure('invalid_marker')
    if value['nonce'] != nonce or value['processID'] != process_id:
        return 'stale', None
    if value['state'] == 'failed':
        raise ProbeFailure('app_' + (value['failureCode'] or 'probe-failed'))
    if value['state'] == 'ready':
        if (value['bootstrap'] != 'healthy' or not value['shellLayout']
                or not value['firstFrameRasterized'] or value['failureCode'] is not None):
            raise ProbeFailure('inconsistent_readiness')
        return 'ready', value
    if value['failureCode'] is not None:
        raise ProbeFailure('inconsistent_readiness')
    return 'starting', None


def wait_ready(request, launch_log, container_file, timeout, *, clock=time.monotonic,
               pause=time.sleep):
    if not 0 < timeout <= 80:
        raise ProbeFailure('invalid_wait_bound')
    nonce, process_id, marker = context(request, launch_log, container_file)
    deadline = clock() + timeout
    while True:
        state, value = inspect_marker(marker, nonce, process_id)
        if state == 'ready':
            return readiness_report(value)
        remaining = deadline - clock()
        if remaining <= 0:
            raise TimeoutError('readiness_timeout_' + state)
        # Condition polling only, with a monotonic deadline; never an acceptance
        # sleep. The outer run_phase bound clips this further to the job budget.
        pause(min(0.1, remaining))


def readiness_report(value):
    return {'schemaVersion': 1, 'state': 'ready', 'processID': value['processID'],
            'launchBound': True, 'bootstrap': value['bootstrap'],
            'shellLayout': True, 'firstFrameRasterized': True,
            'visualReviewRequired': True, 'physicalDeviceAcceptance': False}


def capture_report(request, launch_log, container_file, screenshot):
    nonce, process_id, marker = context(request, launch_log, container_file)
    state, value = inspect_marker(marker, nonce, process_id)
    if state != 'ready':
        raise ProbeFailure('readiness_lost_after_capture')
    data = read_regular(screenshot, 64 * 1024 * 1024)
    if (len(data) < 45 or data[:8] != b'\x89PNG\r\n\x1a\n'
            or data[8:16] != b'\x00\x00\x00\rIHDR'
            or zlib.crc32(data[12:29]) != struct.unpack('>I', data[29:33])[0]):
        raise ProbeFailure('invalid_capture')
    width, height = struct.unpack('>II', data[16:24])
    if not 0 < width <= 16384 or not 0 < height <= 16384:
        raise ProbeFailure('invalid_capture')
    return {**readiness_report(value), 'captureBytes': len(data),
            'captureSHA256': hashlib.sha256(data).hexdigest(),
            'captureWidth': width, 'captureHeight': height,
            'captureCheck': 'PNG header and identity only; content needs visual review'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('prepare').add_argument('request', type=Path)
    for name in ('wait', 'capture'):
        command = sub.add_parser(name)
        for field in ('request', 'launch_log', 'container_file'):
            command.add_argument(field, type=Path)
        command.add_argument('--report', type=Path, required=True)
        if name == 'wait':
            command.add_argument('--timeout', type=float, default=80)
        else:
            command.add_argument('screenshot', type=Path)
    args = parser.parse_args()
    report, status = {}, 0
    try:
        if args.action == 'prepare':
            print(prepare(args.request))
            return 0
        if args.action == 'wait':
            report = wait_ready(args.request, args.launch_log, args.container_file, args.timeout)
        else:
            report = capture_report(args.request, args.launch_log, args.container_file, args.screenshot)
    except TimeoutError as error:
        report, status = {'state': 'blocked', 'reason': str(error)}, 124
    except ProbeFailure as error:
        report, status = {'state': 'blocked', 'reason': str(error)}, 2
    except (OSError, ValueError, TypeError, KeyError, UnicodeError, RecursionError):
        # Input/library exceptions may embed paths or marker data. Publish only
        # this fixed code. ProbeFailure carries only explicitly allowlisted reasons.
        report, status = {'state': 'blocked', 'reason': 'invalid_or_failed_ui_evidence'}, 2
    if hasattr(args, 'report'):
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    return status


if __name__ == '__main__':
    sys.exit(main())

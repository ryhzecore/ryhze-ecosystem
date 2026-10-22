#!/usr/bin/env python3
"""Process-level checks of real Scene publication, CPU particles and offline PCM."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
args, rest = parser.parse_known_args()


class EffectsWorkflow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='race-effects-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.output = Path(cls.directory.name)
        cls.process = cls.run_binary('--output-dir', str(cls.output))
        if cls.process.returncode:
            raise AssertionError(cls.process.stdout + cls.process.stderr)
        cls.rows = [json.loads(line) for line in cls.process.stdout.splitlines()]

    @staticmethod
    def run_binary(*arguments):
        return subprocess.run([args.binary, *arguments], capture_output=True, text=True, timeout=30)

    def test_scene_publication_and_observable_cosmetic_exhaustion(self):
        self.assertEqual(len(self.rows), 3)
        baseline, rebased, result = self.rows
        for row in (baseline, rebased):
            self.assertEqual(row['ticks'], 120)
            self.assertEqual(row['tick_rate'], 60)
            self.assertEqual(row['scene_publications'], 121)
            self.assertEqual(row['scene_reads_released'], 120)
            self.assertEqual(row['audio_rejected'], 120)
            self.assertEqual(row['events_dropped'], 120)
            self.assertGreater(row['particles_dropped'], 0)
            self.assertGreater(row['particles_spawned'], 16)
            self.assertGreater(row['particles_expired'], 0)
            self.assertEqual(row['peak_particles'], 16)
            self.assertEqual(row['pcm_frames'], 96000)
            self.assertTrue(row['lifecycle_clean'])
            self.assertEqual(row['audio_completed'], 1)
            self.assertEqual(row['audio_clipped_samples'], 0)
            self.assertEqual(row['tracked_live_bytes_after_stop'], 0)
        self.assertEqual(rebased['scene_generation'], baseline['scene_generation'] + 1)
        self.assertEqual((baseline['origin_revision'], rebased['origin_revision']), (1, 2))
        self.assertEqual((baseline['rebases'], rebased['rebases']), (0, 1))
        for field in ('particles_spawned', 'particles_expired', 'particles_dropped', 'particle_samples'):
            self.assertEqual(baseline[field], rebased[field])
        self.assertTrue(result['effects_headless_passed'])
        self.assertTrue(result['pcm_exact'])
        self.assertTrue(result['particle_identity_exact'])
        self.assertLessEqual(result['particle_max_error_metres'], 1e-10)
        self.assertEqual(result['particle_tolerance_metres'], 1e-10)
        self.assertGreater(result['tracked_peak_bytes'], 0)
        self.assertEqual(result['tracked_live_bytes_after_stop'], 0)
        self.assertTrue(result['offline_owner_thread'])
        self.assertFalse(result['device_playback'])
        self.assertFalse(result['gpu_rendering'])

    def test_real_pcm_waves_and_rebase_continuity(self):
        first = (self.output / 'baseline.wav').read_bytes()
        second = (self.output / 'rebased.wav').read_bytes()
        self.assertEqual(len(first), 384044)
        self.assertEqual(hashlib.sha256(first).digest(), hashlib.sha256(second).digest())
        with wave.open(str(self.output / 'baseline.wav'), 'rb') as audio:
            self.assertEqual((audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getnframes()),
                             (2, 2, 48000, 96000))
            pcm = struct.unpack('<192000h', audio.readframes(96000))
        self.assertGreater(max(pcm), 1000)
        self.assertLess(min(pcm), -1000)
        # The authoritative emitter moves from left to right while its waveform loops.
        start = pcm[:1600]
        end = pcm[-1600:]
        self.assertGreater(sum(abs(x) for x in start[0::2]), sum(abs(x) for x in start[1::2]))
        self.assertLess(sum(abs(x) for x in end[0::2]), sum(abs(x) for x in end[1::2]))

    def test_repeatable_execution(self):
        repeated = self.run_binary()
        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertEqual([json.loads(line) for line in repeated.stdout.splitlines()], self.rows)

    def test_identity_and_invalid_arguments(self):
        identity = self.run_binary('--identity')
        self.assertEqual(identity.returncode, 0, identity.stderr)
        record = json.loads(identity.stdout)
        self.assertEqual(len(record['source_commit']), 40)
        self.assertEqual(len(record['authored_input_sha256']), 64)
        for arguments in (('--unknown',), ('--output-dir',), ('--output-dir', ''), ('--identity', 'extra')):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.run_binary(*arguments).returncode, 2)
        occupied = self.output / 'not-a-directory'
        occupied.write_bytes(b'preserve existing file')
        self.assertNotEqual(self.run_binary('--output-dir', str(occupied)).returncode, 0)
        self.assertEqual(occupied.read_bytes(), b'preserve existing file')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *rest])

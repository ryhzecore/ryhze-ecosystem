"""Shell result-accounting simulations; these do not execute Apple commands."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class VerificationStatusTests(unittest.TestCase):
    helper = Path(__file__).with_name('verification_status.sh').resolve()

    def run_fixture(self, body):
        with tempfile.TemporaryDirectory() as temporary:
            script = '''set -euo pipefail
evidence="$1"
source "$2"
trap 'record_final_status "$?"' EXIT
run_phase() { shift 2; "$@"; }
''' + body + '\nfinish_verification\n'
            result = subprocess.run(
                ['bash', '-c', script, 'fixture', temporary, str(self.helper)],
                capture_output=True, text=True,
            )
            status = json.loads((Path(temporary) / 'phase-status.json').read_text())
            return result, status

    def test_failed_visual_gate_does_not_hide_later_diagnostic_result(self):
        result, status = self.run_fixture('''
run_recorded visual-fixture 5 bash -c 'exit 7' || true
run_recorded independent-fixture 5 bash -c 'echo simulated-check-ran' || true
''')
        self.assertEqual(result.returncode, 1)
        self.assertIn('simulated-check-ran', result.stdout)
        self.assertEqual(status['overall_exit_code'], 1)
        self.assertEqual([(p['conclusion'], p['exit_code']) for p in status['phases']],
                         [('failed', 7), ('passed', 0)])

    def test_dependent_check_is_marked_not_run_after_failed_prerequisite(self):
        result, status = self.run_fixture('''
if run_recorded prerequisite-fixture 5 bash -c 'exit 9'; then
  run_recorded dependent-fixture 5 bash -c 'echo SHOULD-NOT-RUN'
else
  skip_phase dependent-fixture 'Prerequisite failed.'
fi
run_recorded unrelated-fixture 5 true || true
''')
        self.assertEqual(result.returncode, 1)
        self.assertNotIn('SHOULD-NOT-RUN', result.stdout)
        self.assertEqual(status['phases'][1], {
            'name': 'dependent-fixture', 'conclusion': 'not_run',
            'exit_code': None, 'reason': 'Prerequisite failed.',
        })
        self.assertEqual(status['phases'][2]['conclusion'], 'passed')

    def test_all_passed_checks_are_success_but_skip_cannot_be_success(self):
        result, status = self.run_fixture('run_recorded passing-fixture 5 true')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(status['overall_exit_code'], 0)
        result, status = self.run_fixture("skip_phase absent-fixture 'Missing prerequisite.'")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status['overall_exit_code'], 1)

    def test_exhausted_budget_records_not_run_and_failure(self):
        result, status = self.run_fixture('''
run_phase() { phase_not_started=1; return 124; }
run_recorded budget-fixture 5 true || true
''')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status['phases'][0]['conclusion'], 'not_run')
        self.assertEqual(status['phases'][0]['exit_code'], 124)
        self.assertEqual(status['overall_exit_code'], 1)

    def test_final_archive_failure_overrides_earlier_success(self):
        result, status = self.run_fixture('''
run_recorded passing-fixture 5 true
trap 'record_phase evidence-archive failed 1; record_final_status 1; exit 1' EXIT
''')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(status['phases'][0]['conclusion'], 'passed')
        self.assertEqual(status['phases'][1]['conclusion'], 'failed')
        self.assertEqual(status['overall_exit_code'], 1)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Independent small-fixture checks of the Scene measuring executable."""
import argparse,json,subprocess,unittest
from measure_scene import FAMILIES,SCALES,variant_order
parser=argparse.ArgumentParser();parser.add_argument('--binary',required=True);args,rest=parser.parse_known_args()
BINARY=args.binary
class Workloads(unittest.TestCase):
    def run_workload(self,*arguments):
        return subprocess.run([BINARY,*arguments],capture_output=True,text=True,timeout=60)
    def test_all_families(self):
        n=64;repeats=131072//n
        depth_sum=0
        for i in range(n):
            depth=1
            while i:i=(i-1)//4;depth+=1
            depth_sum+=depth
        checks={'iteration':(repeats*(n//2)*(n//2-1))^(repeats*2*(n//4)*(n//4-1)), 'structural':n,'transform':depth_sum,'loading':(n//2)*(n//2-1),'template':n+(n//32)*10,'retained':9}
        for family,checksum in checks.items():
            with self.subTest(family=family):
                result=self.run_workload('--family',family,'--objects',str(n),'--warmups','0','--trials','1')
                self.assertEqual(result.returncode,0,result.stderr)
                rows=[json.loads(line) for line in result.stdout.splitlines()]
                self.assertEqual(len(rows),3)
                identity,trial,complete=rows
                self.assertEqual(identity['family'],family);self.assertEqual(identity['capacity'],2*n)
                self.assertEqual(len(identity['source_commit']),40);self.assertEqual(len(identity['authored_input_sha256']),64)
                self.assertFalse(identity['caller_executes_jobs'])
                self.assertTrue(trial['correct']);self.assertFalse(trial['warmup']);self.assertEqual(trial['checksum'],checksum)
                self.assertGreaterEqual(trial['full_trial_ns'],trial['setup_ns']+trial['verification_ns']+trial['teardown_ns'])
                self.assertGreater(trial['tracked_pool_bytes'],0);self.assertGreater(trial['sampled_rss_bytes'],0)
                names=[p['name'] for p in trial['phases']];self.assertEqual(len(names),len(set(names)))
                for phase in trial['phases']:
                    self.assertGreater(phase['elapsed_ns'],0);self.assertGreater(phase['items'],0)
                if family=='loading':self.assertGreater(trial['json_bytes'],trial['cooked_bytes'])
                if family=='iteration':self.assertEqual(identity['scheduler_workers'],2)
                if family=='retained':self.assertIn('collect_command_receipts',names)
                self.assertTrue(complete['correct']);self.assertEqual(complete['records'],1)
    def test_rejects_invalid_inputs(self):
        base=['--family','iteration','--objects','64','--warmups','0','--trials','1']
        for extra in [['--objects','0'],['--objects','63'],['--objects','16385'],['--trials','0'],['--warmups','17'],['--family','unknown'],['--unknown','x'],['--objects','-1'],['--objects','1e3']]:
            with self.subTest(extra=extra):self.assertNotEqual(self.run_workload(*base,*extra).returncode,0)
    def test_counterbalanced_rotating_schedule(self):
        cases=[(f,n) for f in FAMILIES for n in SCALES]
        orders={case:[] for case in cases}
        visits=[]
        for repetition in range(8):
            shift=(repetition*5)%len(cases)
            for case in cases[shift:]+cases[:shift]:
                names=variant_order(repetition,case,cases,['reference','candidate'])
                self.assertEqual(set(names),{'reference','candidate'})
                orders[case].append(names[0]);visits.extend((repetition,case,v) for v in names)
                self.assertEqual(variant_order(repetition,case,cases,['reference']),['reference'])
        self.assertEqual(len(visits),288);self.assertEqual(len(set(visits)),288)
        for starts in orders.values():
            self.assertEqual(starts.count('reference'),4)
            self.assertEqual(starts.count('candidate'),4)
            self.assertTrue(all(a!=b for a,b in zip(starts,starts[1:])))
    def test_identity(self):
        result=self.run_workload('--identity');self.assertEqual(result.returncode,0);self.assertEqual(len(json.loads(result.stdout)['source_commit']),40)
if __name__=='__main__':unittest.main(argv=[__file__,*rest])

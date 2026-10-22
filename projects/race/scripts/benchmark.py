"""Record the repeatable optimized workload, exact artifact and noisy Deck context."""
from output_paths import evidence_directory
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
sys.dont_write_bytecode = True
from source_manifest import ROOT, authored_inputs, file_sha256, git_state, write_json
from validate import hardware, compile_configuration
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, help="new Evidence directory; allocated automatically when omitted")
args = parser.parse_args()
output = evidence_directory("benchmark", args.output, allow_empty=True)
build = ROOT / "out/release"
identity = json.loads(subprocess.check_output([str(build / "race_headless"), "--identity"], text=True))
source = git_state()
digest = authored_inputs()["sha256"]
assert identity == {"source_commit":source["commit"], "authored_input_sha256":digest, "configuration":"Release", "sanitizer":"none"}, "release artifact is stale or wrong configuration"
binary = build / "race_benchmarks"
executable_sha = file_sha256(binary)
before = hardware()
before["measurement_context"] = "Unpinned Steam Deck Desktop Mode, unchanged governors/power mode; uncontrolled background activity. Run without parallel builds. No thermal stabilization or performance budget is claimed."
started = datetime.now(timezone.utc).isoformat()
environment = dict(os.environ, LC_ALL="C")
with (output / "benchmarks.jsonl").open("w") as stdout, (output / "stderr.log").open("w") as stderr:
    result = subprocess.run(["/usr/bin/time", "-v", "-o", str(output / "resource-use.txt"), str(binary)],
                            cwd=ROOT, stdout=stdout, stderr=stderr, env=environment, timeout=180)
rows = [json.loads(line) for line in (output / "benchmarks.jsonl").read_text().splitlines()]
assert result.returncode == 0 and rows
for row in rows:
    if row["name"] == "workload_checksum": continue
    assert row["source_commit"] == source["commit"] and row["authored_input_sha256"] == digest
assert authored_inputs()["sha256"] == digest and file_sha256(binary) == executable_sha
usage = (output / "resource-use.txt").read_text()
rss = re.search(r"Maximum resident set size \(kbytes\): (\d+)",usage)
receipt = {"schema":1,"kind":"race-baseline-receipt","source_git":source,
           "authored_input_sha256":digest,"started_utc":started,"completed_utc":datetime.now(timezone.utc).isoformat(),
           "executable":{"path":str(binary),"sha256":executable_sha},"build":compile_configuration(build),
           "hardware_before":before,"hardware_after":hardware(),"exit_code":result.returncode,
           "rows":len(rows),"maximum_resident_kib":int(rss.group(1)) if rss else None,
           "files":[{"path":str(path),"sha256":file_sha256(path)} for path in sorted(output.iterdir()) if path.is_file()],
           "method":"Each named workload has 2 warmup and 9 measured trials. Raw times, medians, p95 and population standard deviations are retained. Latency definitions are per row; no regression threshold or performance improvement is inferred."}
write_json(output / "baseline-receipt.json",receipt)
print(json.dumps({"receipt":str(output / "baseline-receipt.json"),"source_commit":source["commit"],"rows":len(rows),"maximum_resident_kib":receipt["maximum_resident_kib"]},indent=2))

import os
if os.name != "nt":
    import resource
import signal
import subprocess
import sys
if os.name != "nt":
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
expected = 3 if os.name == "nt" else -signal.SIGABRT
result = subprocess.run([sys.argv[1], "fatal"], capture_output=True, timeout=10)
assert result.returncode == expected, (result.returncode, result.stderr.decode())
print("invalid Result access: controlled abort exit, execution did not continue")
result = subprocess.run([sys.argv[1], "fatal_diagnostics"], capture_output=True, timeout=10)
assert result.returncode == expected, (result.returncode, result.stderr.decode())
assert b"controlled invariant fixture" in result.stderr and len(result.stderr) < 256
print("fatal diagnostics: bounded emergency sink output and controlled abort exit")

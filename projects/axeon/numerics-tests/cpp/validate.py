#!/usr/bin/env python3
"""Bounded independent C++ comparator smoke/transport tests, not a benchmark."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CLANG = Path("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang")
FLAGS = ["-std=c++17", "-march=x86-64", "-mno-fma", "-fno-fast-math", "-ffp-contract=off",
         "-Wall", "-Wextra", "-Werror", "-pedantic"]
VALID = "v1 zero sum 0 1 0000000000000000 0 0\n"


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    resource.setrlimit(resource.RLIMIT_CPU, (30, 30))


def run(command, payload=None):
    return subprocess.run(command, input=payload, text=True, capture_output=True,
                          timeout=30, preexec_fn=limits, check=False)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if not __debug__:
        raise RuntimeError("validation requires Python assertions; run without -O or PYTHONOPTIMIZE")
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", type=Path, default=CLANG)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    version = run([str(args.clang), "--version"])
    assert version.returncode == 0 and "19.1.7" in version.stdout.split(), version
    malformed = {
        "version": VALID.replace("v1", "v2"),
        "identifier": VALID.replace("zero", "bad-id"),
        "operation": VALID.replace("sum", "fma"),
        "rank_cap": "v1 bad sum 5\n",
        "negative_integer": "v1 bad sum -1\n",
        "integer_overflow": "v1 bad sum 18446744073709551616\n",
        "signed_integer": "v1 bad sum +0\n",
        "nondecimal_integer": "v1 bad sum 0x0\n",
        "element_cap": "v1 bad sum 1 4097 4097\n",
        "uppercase_hex": VALID.replace("0000000000000000", "7FF8000000000000"),
        "short_hex": VALID.replace("0000000000000000", "0000"),
        "nonhex": VALID.replace("0000000000000000", "z000000000000000"),
        "hex_prefix": VALID.replace("0000000000000000", "0x0000000000000000"),
        "transform_cap": "v1 bad sum 0 1 0000000000000000 17\n",
        "transform_kind": "v1 bad sum 0 1 0000000000000000 1 b\n",
        "permutation_cap": "v1 bad sum 0 1 0000000000000000 1 p 5\n",
        "binary_without_rhs": VALID.replace("sum", "add"),
        "unary_with_rhs": VALID.rstrip()[:-1] + "1\n",
        "trailing_token": VALID.rstrip() + " garbage\n",
        "blank_line": "\n",
        "truncated": "v1 bad sum 0 1\n",
        "non_ascii_space": VALID.replace("v1 ", "v1\u00a0"),
        "line_cap": VALID.rstrip() + " " * (256 * 1024) + "\n",
        "input_cap": " " * (1024 * 1024 + 1),
        "fixture_cap": VALID * 513,
    }
    semantic = [
        ("v1 mismatch sum 1 2 1 0000000000000000 0 0", "v1 mismatch err ShapeMismatch"),
        ("v1 extent sum 2 0 65537 0 0 0", "v1 extent err ExtentLimit"),
        ("v1 count sum 2 257 256 0 0 0", "v1 count err ElementLimit"),
        ("v1 bounds sum 1 1 1 0000000000000000 1 s 1 0 1 0 0", "v1 bounds err Bounds"),
        ("v1 step sum 1 1 1 0000000000000000 1 s 0 0 1 0 0", "v1 step err InvalidStep"),
        ("v1 nan sigmoid 0 1 fff0000000000001 0 0", "v1 nan err NonFiniteInput"),
        ("v1 minuszero logsumexp 1 1 1 8000000000000000 0 0", "v1 minuszero ok 0 1 8000000000000000"),
        ("v1 zero sum 0 1 0000000000000000 0 0", "v1 zero ok 0 1 0000000000000000"),
    ]
    receipt = {
        "schema_version": 1,
        "purpose": "independent C++ comparator correctness smoke; not performance measurement",
        "clang_version": version.stdout,
        "clang_sha256": digest(args.clang),
        "source_sha256": {path.name: digest(path) for path in sorted(HERE.glob("*"))
                          if path.suffix in (".cpp", ".hpp", ".py")},
        "limits": {"address_space_bytes": 2 * 1024**3, "cpu_seconds": 30, "wall_seconds": 30},
        "optimization_runs": [],
    }
    with tempfile.TemporaryDirectory(prefix="axeon-cpp-reference-") as directory:
        out = Path(directory)
        for opt in (0, 2, 3):
            row = {"optimization": opt, "build_commands": [], "malformed_transport_cases": []}
            for source in ("driver", "selftest"):
                binary = out / f"{source}-o{opt}"
                command = [str(args.clang), *FLAGS, f"-O{opt}", str(HERE / f"{source}.cpp"),
                           "-lstdc++", "-lm", "-o", str(binary)]
                compiled = run(command)
                assert compiled.returncode == 0, compiled.stderr
                row["build_commands"].append(command)
            selftest = run([str(out / f"selftest-o{opt}")])
            assert selftest.returncode == 0 and not selftest.stderr, selftest
            row["selftest_stdout"] = selftest.stdout
            driver = [str(out / f"driver-o{opt}")]
            empty = run(driver, "")
            assert empty.returncode == 0 and not empty.stdout and not empty.stderr, empty
            actual = run(driver, "\n".join(source for source, _ in semantic) + "\n")
            expected = "\n".join(target for _, target in semantic) + "\n"
            assert actual.returncode == 0 and actual.stdout == expected and not actual.stderr, actual
            row["semantic_transport_cases"] = len(semantic)
            for name, payload in malformed.items():
                actual = run(driver, payload)
                assert actual.returncode == 2 and actual.stderr, (name, actual)
                row["malformed_transport_cases"].append(name)
            receipt["optimization_runs"].append(row)
        sanitized = out / "selftest-ubsan"
        sanitizer_command = [str(args.clang), "--driver-mode=g++", *FLAGS, "-O2", "-fsanitize=undefined",
                             "-fno-sanitize-recover=all", str(HERE / "selftest.cpp"),
                             "-lstdc++", "-lm", "-o", str(sanitized)]
        compiled = run(sanitizer_command)
        assert compiled.returncode == 0, compiled.stderr
        sanitizer_test = run([str(sanitized)])
        assert sanitizer_test.returncode == 0 and not sanitizer_test.stderr, sanitizer_test
        receipt["undefined_behavior_sanitizer"] = {
            "build_command": sanitizer_command, "stdout": sanitizer_test.stdout, "status": "passed"
        }
    receipt["status"] = "passed"
    serialized = json.dumps(receipt, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized)
    print("C++ reference: O0/O2/O3 passed; 42 checks + 8 semantic + 25 malformed transport cases/profile; UBSan passed")


if __name__ == "__main__":
    main()

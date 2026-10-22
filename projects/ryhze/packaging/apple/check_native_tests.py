"""Require expected XCTest methods to pass, not merely a successful build."""

import pathlib
import re
import sys


EXPECTED = {
    "core": {"testKeychainRoundTripAndScopeIsolation", "testDurableSnapshotReopensAndRejectsStaleWrite", "testUnsupportedSchemaIsPreserved", "testStateSymlinksAreRejected", "testAtomicMigrationPreservesBackupAndReopens", "testInterruptedMigrationRollsBackBothWrites", "testFutureDocumentSchemaIsPreserved"},
    "seed": {"testKeychainSeed"},
    "restore": {"testKeychainRestoreAfterProcessRestart"},
}


def verify(phase, text):
    passed = set(re.findall(r"Test Case '-\[[^\]]+ (test\w+)\]' passed", text))
    if "** TEST SUCCEEDED **" not in text or not EXPECTED[phase].issubset(passed):
        raise ValueError(f"Native {phase} tests did not all execute and pass")


if __name__ == "__main__":
    verify(sys.argv[1], pathlib.Path(sys.argv[2]).read_text(errors="replace"))
    print(f"Verified expected native {sys.argv[1]} XCTest methods passed")

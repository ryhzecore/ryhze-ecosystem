"""Require the app-container metadata reason used by native lstat checks.

This narrow source/artifact check does not replace Xcode's aggregate privacy
report, dependency review, or owner-reviewed App Store privacy declarations.
"""

import plistlib
import sys


def verify(manifest):
    entries = manifest.get("NSPrivacyAccessedAPITypes", [])
    if not any(isinstance(entry, dict)
               and entry.get("NSPrivacyAccessedAPIType") == "NSPrivacyAccessedAPICategoryFileTimestamp"
               and "C617.1" in entry.get("NSPrivacyAccessedAPITypeReasons", [])
               for entry in entries):
        raise ValueError("Missing app-container file metadata reason C617.1")


if __name__ == "__main__":
    with open(sys.argv[1], "rb") as file:
        verify(plistlib.load(file))
    print("App-container file metadata reason present; aggregate privacy acceptance unverified")

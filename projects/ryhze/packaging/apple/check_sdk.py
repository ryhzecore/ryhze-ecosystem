"""Fail closed when an Apple build uses a different shared Flutter SDK."""

import json
import sys


EXPECTED = {
    "frameworkVersion": "3.47.6",
    "frameworkRevision": "5fc346839b5d0eef006ed8404392afb4dfae428d",
    "dartSdkVersion": "3.13.5",
}


def verify(metadata):
    if not isinstance(metadata, dict):
        raise ValueError("Flutter version response must be an object")
    for key, expected in EXPECTED.items():
        if metadata.get(key) != expected:
            raise ValueError(f"SDK mismatch: {key} must be {expected}")


if __name__ == "__main__":
    verify(json.load(sys.stdin))
    print("Verified Flutter 3.47.6 / Dart 3.13.5 and framework revision")

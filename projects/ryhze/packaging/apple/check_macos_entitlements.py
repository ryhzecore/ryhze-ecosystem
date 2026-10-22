"""Preflight signed macOS Keychain claims against the embedded existing profile.

This checks decoded metadata only. The caller must also verify the actual code
signature and run the native tests. Never log the profile's device identifiers.
"""

import datetime
import json
import plistlib
import sys


BUNDLE = "com.ryhze.ryhze"
TEAM = "B45M4C7CDW"


def permits(pattern, claim):
    return isinstance(pattern, str) and (
        pattern == claim or (pattern.endswith("*") and "*" not in pattern[:-1]
                             and claim.startswith(pattern[:-1])))


def verify(entitlements, profile, *, now=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    expiry = profile.get("ExpirationDate")
    if not isinstance(expiry, datetime.datetime):
        raise ValueError("Profile expiration is missing")
    if expiry.replace(tzinfo=datetime.timezone.utc) <= now:
        raise ValueError("Existing provisioning profile has expired")
    if TEAM not in profile.get("TeamIdentifier", []) or "OSX" not in profile.get("Platform", []):
        raise ValueError("Existing profile does not identify the expected macOS team")
    if entitlements.get("com.apple.developer.team-identifier") != TEAM:
        raise ValueError("Built application has no matching team entitlement")
    application_id = entitlements.get("com.apple.application-identifier")
    prefixes = profile.get("ApplicationIdentifierPrefix", [])
    if not any(application_id == prefix + "." + BUNDLE for prefix in prefixes):
        raise ValueError("Built application identity is not authorized by the profile prefix")
    groups = entitlements.get("keychain-access-groups")
    if groups != [application_id]:
        raise ValueError("Built application must claim only its expected Keychain group")
    granted = profile.get("Entitlements", {})
    if not permits(granted.get("com.apple.application-identifier"), application_id):
        raise ValueError("Existing profile does not authorize this application")
    if not any(permits(pattern, application_id) for pattern in granted.get("keychain-access-groups", [])):
        raise ValueError("Existing profile does not authorize the Keychain group")
    if entitlements.get("com.apple.security.app-sandbox") is not True:
        raise ValueError("Built application is missing its sandbox entitlement")
    return {"metadata_preflight": "passed", "bundle": BUNDLE, "team": TEAM,
            "native_keychain_execution": "still_required"}


if __name__ == "__main__":
    with open(sys.argv[1], "rb") as file:
        entitlements = plistlib.load(file)
    # security cms decodes the existing embedded profile directly into stdin.
    # Store only the sanitized result, not certificates or device identifiers.
    result = verify(entitlements, plistlib.load(sys.stdin.buffer))
    print(json.dumps(result, sort_keys=True))

"""Resolve optional local output defaults without changing explicit CLI paths.

This module has no import-time filesystem effects. Source scripts load it by
its sibling path so direct execution, runpy and file-based imports agree.
"""
import datetime
import itertools
import json
import os
from pathlib import Path
import re
import subprocess


class LayoutError(ValueError):
    pass


def project_root():
    override = os.environ.get("AXEON_PROJECT_ROOT")
    if override is not None:
        root = Path(override)
        if not override or not root.is_absolute():
            raise LayoutError("AXEON_PROJECT_ROOT must be an existing absolute project directory; "
                              "otherwise supply --output-dir explicitly")
    else:
        root = Path("C:/Projects/Axeon" if os.name == "nt" else "/mnt/c/Projects/Axeon")
    if not root.is_dir():
        raise LayoutError("No local Axeon project root is available; supply --output-dir explicitly "
                          "or set AXEON_PROJECT_ROOT to an existing absolute project directory")
    return root.resolve(strict=True)


def checked_output(path):
    """Do not let a default output escape through an existing link or junction."""
    if path.resolve() != path:
        raise LayoutError(f"Default output contains a redirected path: {path}; supply --output-dir explicitly")
    return path


def reserve_evidence_output(run_name):
    """Atomically reserve the first unused attempt; preserve failed attempts."""
    if re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", run_name) is None:
        raise LayoutError("Evidence run names must use lowercase kebab case")
    date = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    parent = checked_output(project_root() / "Evidence" / date / run_name)
    parent.mkdir(parents=True, exist_ok=True)
    for number in itertools.count(1):
        attempt = parent / f"attempt-{number:02d}"
        try:
            attempt.mkdir()
        except FileExistsError:
            continue
        return attempt


def candidate_package_output(source_root):
    """Choose a candidate path only; the caller retains exclusive creation."""
    source_root = Path(source_root)
    manifest = source_root / ".snapshot-manifest.json"
    if manifest.is_file():
        try:
            metadata = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise LayoutError("Cannot read the source snapshot identity; supply --output-dir explicitly") from error
        commit = metadata.get("source_commit") if isinstance(metadata, dict) else None
    else:
        try:
            result = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=source_root,
                                    capture_output=True, text=True, check=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as error:
            raise LayoutError("Cannot determine the source commit for a candidate package; "
                              "supply --output-dir explicitly") from error
        commit = result.stdout.strip()
    if not isinstance(commit, str) or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise LayoutError("A complete source commit is required for a candidate package; "
                          "supply --output-dir explicitly")
    return checked_output(project_root() / "Releases" / "candidates" / commit / "package")

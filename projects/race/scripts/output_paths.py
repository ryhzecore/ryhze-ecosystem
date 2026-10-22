"""Shared locations for retained run evidence and local release candidates.

Build caches stay in the checkout. Historical evidence is never rewritten.
"""
from __future__ import annotations

import argparse
from datetime import date
import os
from pathlib import Path
import re
import stat

_RUN_NAME = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")
_CANDIDATE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
_RESERVED = {"con", "prn", "aux", "nul", *("com" + str(n) for n in range(1, 10)),
             *("lpt" + str(n) for n in range(1, 10))}


def project_root() -> Path:
    configured = os.environ.get("RACE_PROJECT_ROOT")
    if configured:
        path = Path(configured).expanduser()
        if not path.is_absolute():
            raise ValueError("RACE_PROJECT_ROOT must be an absolute project directory")
        return path
    for parent in Path(__file__).resolve().parents:
        if parent.name.lower() == "race":
            return parent
    raise ValueError("Cannot locate the enclosing RACE project; set absolute RACE_PROJECT_ROOT for CI")


def _regular_path(path: Path) -> Path:
    """Reject redirection before resolution, including broken links and junctions."""
    path = Path(os.path.abspath(path.expanduser()))
    for part in (path, *path.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Output path cannot contain a symlink or reparse point: {part}")
    return path.resolve()


def _relative_output(path: Path, root: Path) -> tuple[Path, tuple[str, ...]]:
    root = _regular_path(root)
    path = _regular_path(path)
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise ValueError(f"Output must be below {root}: {path}") from error
    for part in relative.parts:
        if part.endswith((" ", ".")) or ":" in part or part.split(".")[0].lower() in _RESERVED:
            raise ValueError(f"Nonportable output path component: {part}")
    return path, relative.parts


def evidence_path(path: Path, *, root: Path | None = None) -> Path:
    base = (root or project_root()) / "Evidence"
    result, parts = _relative_output(Path(path), base)
    if len(parts) < 2:
        raise ValueError("Evidence output must be below Evidence/YYYY-MM-DD/lowercase-kebab-run")
    try:
        parsed = date.fromisoformat(parts[0])
    except ValueError as error:
        raise ValueError("Evidence date must use YYYY-MM-DD") from error
    if parsed.isoformat() != parts[0] or not _RUN_NAME.fullmatch(parts[1]):
        raise ValueError("Evidence output requires YYYY-MM-DD and a lowercase-kebab run name")
    return result


def candidate_path(path: Path, *, root: Path | None = None) -> Path:
    base = (root or project_root()) / "Releases" / "candidates"
    result, parts = _relative_output(Path(path), base)
    if len(parts) < 2 or not _CANDIDATE_NAME.fullmatch(parts[0]):
        raise ValueError("Package output must be below Releases/candidates/<commit-or-version>/")
    return result


def _attempt(parent: Path) -> Path:
    parent.mkdir(parents=True, exist_ok=True)
    for index in range(1, 1000000):
        path = parent / f"attempt-{index:02d}"
        try:
            path.mkdir()
            return path
        except FileExistsError:
            continue
    raise ValueError(f"No fresh attempt name available under {parent}")


def evidence_directory(run_name: str, requested: Path | None = None, *,
                       allow_empty: bool = False, parent: Path | None = None,
                       root: Path | None = None) -> Path:
    if not _RUN_NAME.fullmatch(run_name):
        raise ValueError("Run name must use lowercase kebab case")
    if requested is not None and parent is not None:
        raise ValueError("Choose an explicit directory or an attempt parent, not both")
    if requested is not None:
        output = evidence_path(requested, root=root)
        if allow_empty and output.is_dir() and not any(output.iterdir()):
            return output
        output.mkdir(parents=True, exist_ok=False)
        return output
    base = evidence_path(parent, root=root) if parent is not None else evidence_path(
        (root or project_root()) / "Evidence" / date.today().isoformat() / run_name, root=root)
    return _attempt(base)


def evidence_report(run_name: str, requested: Path | None = None, *,
                    filename: str = "report.json", root: Path | None = None) -> Path:
    output = evidence_path(requested, root=root) if requested is not None else (
        evidence_directory(run_name, root=root) / filename)
    evidence_path(output.parent, root=root)
    if output.exists():
        raise ValueError(f"Evidence report already exists; preserve it and choose a new path: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    return output


def candidate_output(name: str, requested: Path | None = None, *,
                     leaf: str = "desktop-preview", root: Path | None = None) -> Path:
    if not _CANDIDATE_NAME.fullmatch(name) or name in {".", ".."}:
        raise ValueError("Candidate name must identify a commit or version")
    output = requested if requested is not None else (
        (root or project_root()) / "Releases" / "candidates" / name / leaf)
    return candidate_path(output, root=root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    evidence = modes.add_parser("evidence")
    evidence.add_argument("--run", required=True)
    evidence.add_argument("--output", type=Path)
    candidate = modes.add_parser("candidate")
    candidate.add_argument("--name", required=True)
    candidate.add_argument("--output", type=Path)
    candidate.add_argument("--leaf", default="desktop-preview")
    args = parser.parse_args()
    try:
        if args.mode == "evidence":
            output = evidence_directory(args.run, args.output)
        else:
            output = candidate_output(args.name, args.output, leaf=args.leaf)
        print(output)
        return 0
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())

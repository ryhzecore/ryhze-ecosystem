"""Advisory physical source sizes; warnings never fail a build or split code.

Use --json to retain a deterministic snapshot, then --baseline that.json for
file-level comparison. Exit 1 means an incomplete/invalid report, not a breach.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import tokenize
import io

from source_manifest import ROOT, SOURCE_DIRECTORIES, SOURCE_SUFFIXES

LIMITS = {"file_lines": 800, "function_lines": 80}
EXCLUDED = frozenset({
    "out", "build", "evidence", "generated", "third_party", "third-party",
    "vendor", "vendored", "external", "dependencies", "__pycache__", ".git",
})
SCOPE = {
    "directories": list(SOURCE_DIRECTORIES),
    "suffixes": sorted(SOURCE_SUFFIXES - {".json"}),
    "additional_names": ["CMakeLists.txt", "*.hpp.in"],
    "excluded_directory_names_case_insensitive": sorted(EXCLUDED),
    "inventory": "Git tracked plus nonignored untracked working-tree files; deduplicated",
    "boundary": "First-party code, scripts, shaders, CMake and header templates in the "
                "authored-source directories, plus root CMakeLists.txt. JSON data, "
                "docs, fonts and other suffixes are excluded. Generated/third-party "
                "trees are excluded by the listed directory names; ignored untracked "
                "files are excluded by Git. No provenance inference beyond this boundary.",
    "line_count": "Physical LF-delimited lines, including blank/comment lines; an "
                  "unterminated final line counts once; empty files count zero.",
    "function_count": "Python AST FunctionDef/AsyncFunctionDef only: first decorator "
                      "or def through end_lineno inclusive, including nested definitions "
                      "in outer spans. Lambdas excluded. Other languages unavailable.",
}


def git(root: Path, *arguments: str) -> bytes:
    result = subprocess.run(["git", "-C", str(root), *arguments], capture_output=True,
                            timeout=15, check=True)
    return result.stdout


def eligible(name: str) -> bool:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name:
        raise ValueError(f"Nonportable source path: {name!r}")
    if any(part.lower() in EXCLUDED for part in path.parts[:-1]):
        return False
    if name == "CMakeLists.txt":
        return True
    return (len(path.parts) > 1 and path.parts[0] in SOURCE_DIRECTORIES
            and (path.suffix in SCOPE["suffixes"] or path.name == "CMakeLists.txt"
                 or path.name.endswith(".hpp.in")))


def read_source(root: Path, name: str) -> bytes:
    path = root
    for part in PurePosixPath(name).parts:
        path /= part
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Source path contains a link/reparse point: {name}")
    if not stat.S_ISREG(info.st_mode):
        raise ValueError(f"Source is not a regular file: {name}")
    return path.read_bytes()


def python_functions(data: bytes) -> dict:
    try:
        encoding, _ = tokenize.detect_encoding(io.BytesIO(data).readline)
        tree = ast.parse(data.decode(encoding))
    except (SyntaxError, UnicodeError, LookupError, ValueError) as error:
        return {"status": "unavailable", "reason": type(error).__name__, "functions": []}
    functions = []

    def visit(node: ast.AST, parents: tuple[str, ...] = ()) -> None:
        scope = parents
        if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            scope = (*parents, node.name)
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            start = min([node.lineno, *[item.lineno for item in node.decorator_list]])
            lines = node.end_lineno - start + 1
            functions.append({"name": ".".join(scope), "start_line": start,
                              "end_line": node.end_lineno, "lines": lines,
                              "over_limit": lines > LIMITS["function_lines"]})
        for child in ast.iter_child_nodes(node):
            visit(child, scope)

    visit(tree)
    return {"status": "measured", "method": "Python standard-library AST physical spans",
            "functions": sorted(functions, key=lambda item: (item["start_line"], item["name"]))}


def measure(name: str, data: bytes) -> dict:
    if b"\x00" in data or b"\r" in data.replace(b"\r\n", b""):
        raise ValueError(f"Unsupported binary or bare-CR source: {name}")
    lines = data.count(b"\n") + int(bool(data) and not data.endswith(b"\n"))
    measurement = (python_functions(data) if name.endswith(".py") else
                   {"status": "unavailable", "reason": "No pinned reliable parser for this language",
                    "functions": []})
    return {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "lines": lines, "over_limit": lines > LIMITS["file_lines"],
            "function_measurement": measurement}


def compare_files(files: list[dict], baseline: dict | None) -> dict:
    limitation = ("File paths only: renames are additions/removals. Function changes are "
                  "not classified; Python names may repeat or move, and other-language "
                  "functions are unavailable. Existing warnings are not approved exceptions.")
    if baseline is None:
        return {"status": "not_requested", "limitation": limitation,
                "note": "Snapshot only; no warning is claimed to be newly introduced."}
    if (baseline.get("schema") != 1 or baseline.get("kind") != "race-source-size-report"
            or baseline.get("scope") != SCOPE or baseline.get("limits") != LIMITS):
        raise ValueError("Baseline schema, inventory boundary or limits differ")
    previous = {}
    for item in baseline["files"]:
        name, lines = item["path"], item["lines"]
        if (not isinstance(name, str) or not eligible(name) or name in previous
                or type(lines) is not int or lines < 0):
            raise ValueError("Invalid/duplicate baseline file count")
        previous[name] = lines
    changes = []
    for item in files:
        name, lines = item["path"], item["lines"]
        old = previous.get(name)
        if not item["over_limit"]:
            continue
        status = ("added_over_limit" if old is None else
                  "newly_over_limit" if old <= LIMITS["file_lines"] else "existing_over_limit")
        changes.append({"path": name, "status": status, "previous_lines": old,
                        "lines": lines, "delta": None if old is None else lines - old})
    return {"status": "compared", "baseline_commit": baseline.get("source", {}).get("commit"),
            "file_warnings": changes,
            "removed_paths": sorted(set(previous) - {item["path"] for item in files}),
            "limitation": limitation}


def report(root: Path, baseline: dict | None = None) -> dict:
    root = root.resolve(strict=True)
    actual = Path(git(root, "rev-parse", "--show-toplevel").decode("utf-8").strip()).resolve()
    if actual != root:
        raise ValueError("--root must be the checkout root")
    commit = git(root, "rev-parse", "HEAD").decode("ascii").strip()
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all").decode("utf-8")
    names = git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    selected = sorted({name for name in names.decode("utf-8").split("\0") if name and eligible(name)})
    files = [measure(name, read_source(root, name)) for name in selected]
    if not files:
        raise ValueError("Source inventory is empty")
    digest = hashlib.sha256(json.dumps(
        [{"path": item["path"], "sha256": item["sha256"]} for item in files],
        sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
    functions = [entry for item in files for entry in item["function_measurement"]["functions"]]
    return {"schema": 1, "kind": "race-source-size-report", "advisory": True,
            "limits": LIMITS, "scope": SCOPE,
            "source": {"commit": commit, "status_porcelain_v1": status,
                       "inventory_sha256": digest},
            "summary": {"files": len(files), "lines": sum(item["lines"] for item in files),
                        "oversized_files": sum(item["over_limit"] for item in files),
                        "measured_functions": len(functions),
                        "oversized_measured_functions": sum(item["over_limit"] for item in functions),
                        "files_with_unavailable_function_measurement": sum(
                            item["function_measurement"]["status"] != "measured" for item in files)},
            "comparison": compare_files(files, baseline), "files": files}


def human(value: dict) -> str:
    summary = value["summary"]
    lines = [f"RACE advisory size report: {summary['files']} files, {summary['lines']} physical lines",
             f"Guidance: files >800 / Python function spans >80; warnings do not fail.",
             f"Warnings: {summary['oversized_files']} files, "
             f"{summary['oversized_measured_functions']} measured functions.",
             f"Function measurement unavailable in {summary['files_with_unavailable_function_measurement']} files."]
    for item in value["files"]:
        if item["over_limit"]:
            lines.append(f"FILE {item['lines']:5} {item['path']}")
        for function in item["function_measurement"]["functions"]:
            if function["over_limit"]:
                lines.append(f"FUNC {function['lines']:5} {item['path']}:{function['start_line']} {function['name']}")
    comparison = value["comparison"]
    if comparison["status"] == "compared":
        for item in comparison["file_warnings"]:
            lines.append(f"BASE {item['status']} {item['path']} ({item['previous_lines']} -> {item['lines']})")
    else:
        lines.append(comparison["note"])
    lines.extend([comparison["limitation"], "Inventory: " + SCOPE["boundary"],
                  "Function spans: " + SCOPE["function_count"]])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--json", action="store_true", help="emit structured snapshot to stdout")
    parser.add_argument("--baseline", type=Path, help="prior --json snapshot; file comparison only")
    args = parser.parse_args()
    try:
        baseline = json.loads(args.baseline.read_text(encoding="utf-8-sig")) if args.baseline else None
        value = report(args.root, baseline)
        print(json.dumps(value, indent=2, sort_keys=True) if args.json else human(value))
        return 0
    except (OSError, ValueError, TypeError, KeyError, AttributeError, RecursionError,
            subprocess.SubprocessError) as error:
        print(f"source size report failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

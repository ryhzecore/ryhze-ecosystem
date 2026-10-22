"""Advisory C++ file and function sizes for the code-structure scorecard.

Complements source_size_report.py, which measures files and Python functions only.
A function is a brace block opened outside any other function, on a line that carries
a parameter list or its closing parenthesis, measured to its matching '}'. Namespace,
class, struct, enum and union braces are scopes, so in-class definitions inside a
namespace are measured too. String and character literals and // comments are ignored
when counting braces. Lambdas and nested blocks count toward their enclosing function.
Exit 0 with warnings; --strict exits 1 when any limit is exceeded.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

FILE_LIMIT, FUNCTION_LIMIT = 800, 80
SUFFIXES = (".cpp", ".hpp", ".h", ".cc")
LITERAL = re.compile(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])+\'')
NAME = re.compile(r"([~\w:]+)\s*\(")
SCOPE = re.compile(r"\s*((template\s*<.*>\s*)?(namespace|class|struct|enum|union)\b|extern\s+\"C\")")


def functions(lines: list[str]) -> list[dict]:
    """Return [{name, line, length}] for every function definition outside another function."""
    found, stack, start, pending, previous = [], [], None, ("?", 0), ""
    for number, raw in enumerate(lines, 1):
        code = LITERAL.sub('""', raw).split("//")[0]
        in_function = "function" in stack
        names = NAME.findall(code)
        continued = previous.rstrip().endswith((",", "(", ":")) or code.lstrip().startswith((":", ","))
        if not in_function and names and not continued:
            pending = (names[0], number)
        previous = code if code.strip() else previous
        first = True
        for ch in code:
            if ch == "{":
                if not first or in_function:
                    kind = "block"
                elif SCOPE.match(code):
                    kind = "scope"
                elif "(" in code or ")" in code:
                    kind = "function"
                else:
                    kind = "block"
                first = False
                if kind == "function" and start is None:
                    # A signature spans at most a few lines; an older name belongs to another function.
                    fresh = pending[1] and number - pending[1] <= 12
                    start = (pending[1] if fresh else number, len(stack), pending[0] if fresh else "?")
                stack.append(kind)
                in_function = in_function or kind == "function"
            elif ch == "}" and stack:
                stack.pop()
                if start is not None and len(stack) == start[1]:
                    found.append({"name": start[2], "line": start[0], "length": number - start[0] + 1})
                    start, pending = None, ("?", 0)
    return found


def tracked(root: Path) -> list[Path]:
    names = subprocess.run(["git", "-C", str(root), "-c", "core.longpaths=true", "ls-files", "apps", "src",
                            "tests", "benchmarks", "include"], capture_output=True, text=True,
                           check=True).stdout.splitlines()
    return [root / name for name in names if name.endswith(SUFFIXES)]


def report(paths: list[Path], root: Path) -> dict:
    files, long_functions = [], []
    for path in paths:
        lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
        relative = path.relative_to(root).as_posix() if path.is_relative_to(root) else str(path)
        if len(lines) > FILE_LIMIT:
            files.append({"path": relative, "lines": len(lines)})
        long_functions += [{"path": relative, **f} for f in functions(lines) if f["length"] > FUNCTION_LIMIT]
    return {"limits": {"file_lines": FILE_LIMIT, "function_lines": FUNCTION_LIMIT},
            "files_scanned": len(paths),
            "files_over_limit": sorted(files, key=lambda f: -f["lines"]),
            "functions_over_limit": sorted(long_functions, key=lambda f: -f["length"])}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, help="files to scan (default: tracked C++ sources)")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    parser.add_argument("--strict", action="store_true", help="exit 1 when any limit is exceeded")
    args = parser.parse_args()
    root = args.root.resolve()
    result = report([p.resolve() for p in args.paths] or tracked(root), root)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"C++ size report: {result['files_scanned']} files; limits {FILE_LIMIT} lines/file, "
              f"{FUNCTION_LIMIT} lines/function")
        for f in result["files_over_limit"]:
            print(f"FILE  {f['lines']:5}  {f['path']}")
        for f in result["functions_over_limit"]:
            print(f"FUNC  {f['length']:5}  {f['path']}:{f['line']}  {f['name']}")
        print(f"Over limit: {len(result['files_over_limit'])} files, {len(result['functions_over_limit'])} functions")
    over = result["files_over_limit"] or result["functions_over_limit"]
    return 1 if args.strict and over else 0


if __name__ == "__main__":
    raise SystemExit(main())

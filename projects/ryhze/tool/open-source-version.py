#!/usr/bin/env python3
"""Inspect an existing Git revision in another worktree without switching app/."""

import argparse
import json
import pathlib
import subprocess


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("revision", nargs="?", default="main")
    parser.add_argument("--no-open", action="store_true", help="Verify/create the worktree without opening VS Code")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    commit = git(root, "rev-parse", "--verify", "--end-of-options", args.revision + "^{commit}")
    baseline = "d03e4024083c169d8fbc8e39e67740a52ebf2724"
    name = "original-d03e402" if commit == baseline else "source-" + commit[:12]
    destination = root.parent / "source-versions" / name
    registered = []
    for block in git(root, "worktree", "list", "--porcelain").split("\n\n"):
        values = dict(line.split(" ", 1) for line in block.splitlines() if " " in line)
        if values.get("worktree") == str(destination):
            registered.append(values)
    if destination.is_symlink():
        raise SystemExit("Refusing to reuse a symlink destination: " + str(destination))
    if destination.exists():
        if len(registered) != 1 or registered[0].get("HEAD") != commit:
            raise SystemExit("Existing destination does not match this repository/revision; nothing was overwritten: " + str(destination))
        current_common = pathlib.Path(git(root, "rev-parse", "--git-common-dir"))
        actual_common = pathlib.Path(git(destination, "rev-parse", "--git-common-dir"))
        current_common = (root / current_common).resolve()
        actual_common = (destination / actual_common).resolve()
        if actual_common != current_common or git(destination, "rev-parse", "HEAD") != commit:
            raise SystemExit("Destination Git metadata does not match; nothing was overwritten.")
    else:
        if registered:
            raise SystemExit("The worktree is registered but missing; inspect it manually before continuing.")
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "-C", str(root), "worktree", "add", "--detach", str(destination), commit], check=True)
    print(json.dumps({"repository": str(root), "revision": args.revision, "commit": commit, "worktree": str(destination), "working_tree_changes": git(destination, "status", "--short")}, indent=2))
    if not args.no_open:
        subprocess.run([
            "/usr/bin/code", "--user-data-dir", str(pathlib.Path.home() / ".config/Code-Ryhze"),
            "--extensions-dir", str(pathlib.Path.home() / ".local/share/ryhze-vscode/extensions"),
            "--new-window", str(destination)
        ], check=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Verify complete public-header snapshots and require revision bumps."""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_PATH = ROOT / "contracts" / "version.json"


def run_git(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(ROOT), *args],
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def git_prefix() -> str:
    result = run_git("rev-parse", "--show-prefix", check=False)
    return result.stdout.strip() if result.returncode == 0 else ""


def git_path(relative: str) -> str:
    return git_prefix() + relative


def load_version_bytes(data: str) -> dict:
    parsed = json.loads(data)
    if parsed.get("schema") != 1 or not isinstance(parsed.get("scopes"), dict):
        raise SystemExit("contract version schema is invalid")
    return parsed


def load_version() -> dict:
    if not VERSION_PATH.is_file():
        raise SystemExit("missing contracts/version.json")
    return load_version_bytes(VERSION_PATH.read_text())


def scope_files(scope: dict) -> list[str]:
    files: list[str] = []
    for item in scope.get("paths", []):
        base = ROOT / item
        if not base.is_dir():
            raise SystemExit(f"missing contract directory: {item}")
        for path in base.rglob("*.h"):
            if path.is_file():
                files.append(path.relative_to(ROOT).as_posix())
    return sorted(set(files))


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_snapshot(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise SystemExit(f"missing snapshot: {path.relative_to(ROOT)}")
    result: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) != 2 or len(parts[0]) != 64:
            raise SystemExit(f"invalid snapshot line {line_number}: {raw}")
        rel = parts[1].strip()
        if rel in result:
            raise SystemExit(f"duplicate snapshot entry: {rel}")
        result[rel] = parts[0].lower()
    return result


def render_snapshot(files: list[str]) -> str:
    return "".join(f"{digest(ROOT / rel)}  {rel}\n" for rel in files)


def verify_snapshot(name: str, scope: dict, update: bool) -> None:
    files = scope_files(scope)
    snapshot_rel = scope.get("snapshot")
    if not snapshot_rel:
        raise SystemExit(f"{name}: missing snapshot path")
    snapshot = ROOT / snapshot_rel
    if update:
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(render_snapshot(files))
        return
    recorded = parse_snapshot(snapshot)
    expected = set(files)
    actual = set(recorded)
    missing = sorted(expected - actual)
    stale = sorted(actual - expected)
    if missing or stale:
        if missing:
            print(f"{name}: snapshot missing files: {', '.join(missing)}")
        if stale:
            print(f"{name}: snapshot has stale files: {', '.join(stale)}")
        raise SystemExit(1)
    mismatched = [rel for rel in files if digest(ROOT / rel) != recorded[rel]]
    if mismatched:
        raise SystemExit(f"{name}: changed without snapshot update: {', '.join(mismatched)}")


def git_json(revision: str) -> dict | None:
    result = run_git("show", f"{revision}:{git_path('contracts/version.json')}", check=False)
    if result.returncode != 0:
        return None
    return load_version_bytes(result.stdout)


def changed_paths(left: str, right: str | None, paths: list[str]) -> bool:
    args = ["diff", "--name-only"]
    if right is None:
        args.append(left)
    else:
        args.extend([left, right])
    args.append("--")
    args.extend(git_path(path) for path in paths)
    result = run_git(*args, check=False)
    return result.returncode == 0 and bool(result.stdout.strip())


def revision_of(version: dict, name: str) -> int:
    scope = version.get("scopes", {}).get(name)
    if not isinstance(scope, dict):
        return -1
    value = scope.get("revision")
    return value if isinstance(value, int) else -1


def verify_revision_bumps(current: dict) -> None:
    head = git_json("HEAD")
    parent = git_json("HEAD^")
    for name, scope in current["scopes"].items():
        if not scope.get("enforce_revision", True):
            continue
        revision = revision_of(current, name)
        if revision < 1:
            raise SystemExit(f"{name}: revision must be a positive integer")
        paths = list(scope.get("paths", []))
        if head is not None and changed_paths("HEAD", None, paths):
            old = revision_of(head, name)
            if old >= 0 and revision <= old:
                raise SystemExit(
                    f"{name}: public headers changed in worktree; "
                    f"revision must increase above {old}"
                )
        if head is not None and parent is not None and changed_paths("HEAD^", "HEAD", paths):
            old = revision_of(parent, name)
            now = revision_of(head, name)
            if old >= 0 and now <= old:
                raise SystemExit(
                    f"{name}: HEAD changed public headers without revision bump "
                    f"({old} -> {now})"
                )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args()
    current = load_version()
    for name, scope in current["scopes"].items():
        verify_snapshot(name, scope, args.update)
    if not args.update:
        verify_revision_bumps(current)
        summary = ", ".join(
            f"{name}=r{scope['revision']}" for name, scope in current["scopes"].items()
        )
        print(f"contract snapshots: PASS ({summary})")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
import argparse
import re
import sys

@dataclass(frozen=True)
class Project:
    project_id: str
    title: str
    image: str
    is_default: bool

ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$")
IMAGE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\.img$")

def load_catalog(path: Path) -> list[Project]:
    projects: list[Project] = []
    ids: set[str] = set()
    images: set[str] = set()
    defaults = 0
    for line_no, raw in enumerate(path.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        if len(fields) != 4:
            raise ValueError(f"{path}:{line_no}: expected 4 fields")
        project_id, title, image, default = [field.strip() for field in fields]
        if not ID_RE.fullmatch(project_id):
            raise ValueError(f"{path}:{line_no}: invalid project id {project_id!r}")
        if not title or "|" in title or "\n" in title:
            raise ValueError(f"{path}:{line_no}: invalid title")
        if not IMAGE_RE.fullmatch(image) or "/" in image:
            raise ValueError(f"{path}:{line_no}: invalid image filename {image!r}")
        if default not in ("", "default"):
            raise ValueError(f"{path}:{line_no}: fourth field must be empty or default")
        if project_id in ids:
            raise ValueError(f"{path}:{line_no}: duplicate project id {project_id}")
        if image in images:
            raise ValueError(f"{path}:{line_no}: duplicate image filename {image}")
        is_default = default == "default"
        defaults += 1 if is_default else 0
        ids.add(project_id)
        images.add(image)
        projects.append(Project(project_id, title, image, is_default))
    if not projects:
        raise ValueError(f"{path}: catalog is empty")
    if defaults != 1:
        raise ValueError(f"{path}: exactly one default project is required")
    return projects

def emit_list(projects: list[Project]) -> None:
    for project in projects:
        marker = "default" if project.is_default else ""
        print(f"{project.project_id}|{project.title}|{project.image}|{marker}")

def emit_grub_amd64(projects: list[Project], kernel: str, image_dir: str) -> None:
    default_index = next(i for i, project in enumerate(projects) if project.is_default)
    print(f"set default={default_index}")
    for project in projects:
        print(f'menuentry "{project.title}" {{')
        print(f"    multiboot2 {kernel}")
        print(f"    module2 {image_dir}/{project.image} rootfs")
        print("    boot")
        print("}")

def load_boot_entry(path: Path) -> str:
    entry = path.read_text().strip()
    if not re.fullmatch(r"/krnl/extension/[A-Za-z0-9_-]{1,48}", entry):
        raise ValueError(f"{path}: invalid boot extension entry {entry!r}")
    return entry

def emit_boot_header(entry: str) -> None:
    print("#ifndef NEXSOS_GENERATED_BOOT_ENTRY_H")
    print("#define NEXSOS_GENERATED_BOOT_ENTRY_H")
    print(f'#define NEXS_BOOT_CONFIG_ENTRY "{entry}"')
    print("#endif")

def emit_grub_aarch64(projects: list[Project], loader_dir: str) -> None:
    default_index = next(i for i, project in enumerate(projects) if project.is_default)
    print(f"set default={default_index}")
    for project in projects:
        print(f'menuentry "{project.title}" {{')
        print(f"    chainloader {loader_dir}/{project.project_id}.efi")
        print("    boot")
        print("}")

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", type=Path, required=True)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    amd = sub.add_parser("grub-amd64")
    amd.add_argument("--kernel", default="/boot/kernel.elf")
    amd.add_argument("--image-dir", default="/boot/user-images")
    arm = sub.add_parser("grub-aarch64")
    arm.add_argument("--loader-dir", default="/boot/user-loaders")
    header = sub.add_parser("boot-header")
    header.add_argument("--entry", type=Path, required=True)
    args = parser.parse_args()
    try:
        projects = load_catalog(args.catalog)
    except (OSError, ValueError) as exc:
        print(f"user-project catalog error: {exc}", file=sys.stderr)
        return 2
    if args.command == "list":
        emit_list(projects)
    elif args.command == "grub-amd64":
        emit_grub_amd64(projects, args.kernel, args.image_dir)
    elif args.command == "grub-aarch64":
        emit_grub_aarch64(projects, args.loader_dir)
    elif args.command == "boot-header":
        try:
            emit_boot_header(load_boot_entry(args.entry))
        except (OSError, ValueError) as exc:
            print(f"boot entry error: {exc}", file=sys.stderr)
            return 2
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

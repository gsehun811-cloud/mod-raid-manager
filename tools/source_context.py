"""Resolve a compatible, read-only AzerothCore reference checkout for development tools."""

import os
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1]


def reference_root():
    configured = os.environ.get("RAID_MANAGER_SOURCE_ROOT")
    candidates = [Path(configured).expanduser().resolve()] if configured else list(MODULE.parents)
    for candidate in candidates:
        if (candidate / "src/common/Define.h").is_file() and (
                candidate / "data/sql/base/db_world/item_template.sql").is_file():
            return candidate
    raise RuntimeError("Set RAID_MANAGER_SOURCE_ROOT to a compatible AzerothCore checkout containing source SQL")


def input_name(path, root, module=MODULE):
    try:
        return "modules/mod-raid-manager/" + path.relative_to(module).as_posix()
    except ValueError:
        return path.relative_to(root).as_posix()


def reference_input(root, name):
    prefix = "modules/mod-raid-manager/"
    return MODULE / name[len(prefix):] if name.startswith(prefix) else root / name

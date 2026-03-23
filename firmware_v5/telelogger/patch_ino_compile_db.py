#!/usr/bin/env python3
"""Append a compile_commands.json entry for telelogger.ino (copy of telelogger.ino.cpp).

Run after: pio run -t compiledb
So VS Code / Cursor cpptools matches the open .ino file to the same flags as the generated sketch.
"""
from __future__ import annotations

import json
import os
import sys


def main() -> int:
    project_dir = os.path.dirname(os.path.abspath(__file__))
    cc_path = os.path.join(project_dir, "compile_commands.json")
    ino_path = os.path.join(project_dir, "telelogger.ino")

    if not os.path.isfile(cc_path):
        print("patch_ino_compile_db: compile_commands.json missing — run: pio run -t compiledb", file=sys.stderr)
        return 1
    if not os.path.isfile(ino_path):
        print("patch_ino_compile_db: telelogger.ino not found", file=sys.stderr)
        return 1

    abs_ino = os.path.abspath(ino_path)

    with open(cc_path, "r", encoding="utf-8") as f:
        db = json.load(f)

    template = None
    for entry in db:
        if entry.get("file") == "telelogger.ino.cpp":
            template = entry
            break

    if template is None:
        print("patch_ino_compile_db: no telelogger.ino.cpp entry in compile_commands.json", file=sys.stderr)
        return 1

    db = [e for e in db if e.get("file") != abs_ino]

    new_entry = dict(template)
    new_entry["file"] = abs_ino
    cmd = new_entry.get("command")
    if isinstance(cmd, str) and cmd.endswith(" telelogger.ino.cpp"):
        new_entry["command"] = cmd[: -len("telelogger.ino.cpp")] + "telelogger.ino"

    db.append(new_entry)

    with open(cc_path, "w", encoding="utf-8") as f:
        json.dump(db, f)
        f.write("\n")

    print("patch_ino_compile_db: added entry for", abs_ino)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

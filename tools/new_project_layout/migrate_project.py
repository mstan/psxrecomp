#!/usr/bin/env python3
"""Project Studio CLI entry (migrate / update title repos → setup-host layout).

Examples:
  python3 tools/new_project_layout/migrate_project.py audit --root ~/src/ApeEscapeRecomp
  python3 tools/new_project_layout/migrate_project.py plan  --root ~/src/ApeEscapeRecomp
  python3 tools/new_project_layout/migrate_project.py apply --root ~/src/ApeEscapeRecomp --dry-run
  python3 tools/new_project_layout/migrate_project.py git status --root ~/src/ApeEscapeRecomp
  python3 tools/new_project_layout/migrate_project.py gui
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from project_studio.cli import main

if __name__ == "__main__":
    raise SystemExit(main())

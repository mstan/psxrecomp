#!/usr/bin/env python3
"""Launch the Project Studio GUI (CustomTkinter).

First run auto-bootstraps ``.venv`` from ``requirements-gui.txt``.
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from project_studio.cli import main

if __name__ == "__main__":
    raise SystemExit(main(["gui", *sys.argv[1:]]))

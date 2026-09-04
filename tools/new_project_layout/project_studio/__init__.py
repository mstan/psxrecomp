"""PSXRecomp Project Studio — audit / plan / apply for New Project Layout.

Setup-host releases only (no prebuilt game-C packaging path).
"""

from __future__ import annotations

__version__ = "0.1.0"

from .detect import audit_project
from .models import AuditReport, CheckResult, MigrateOptions, Plan, PlanStep
from .plan import build_plan
from .ops import apply_plan

__all__ = [
    "AuditReport",
    "CheckResult",
    "MigrateOptions",
    "Plan",
    "PlanStep",
    "audit_project",
    "build_plan",
    "apply_plan",
    "__version__",
]

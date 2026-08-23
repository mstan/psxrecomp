"""Data models for audit / plan / apply."""

from __future__ import annotations

from dataclasses import dataclass, field, asdict
from enum import Enum
from typing import Any


class Severity(str, Enum):
    REQUIRED = "required"
    RECOMMENDED = "recommended"
    OPTIONAL = "optional"
    INFO = "info"


class CheckStatus(str, Enum):
    PASS = "pass"
    FAIL = "fail"
    WARN = "warn"
    SKIP = "skip"


class LayoutClass(str, Enum):
    SCAFFOLD_COMPLETE = "scaffold-complete"
    SETUP_HOST_PARTIAL = "setup-host-partial"
    LEGACY_PACKAGING = "legacy-packaging"
    UNKNOWN = "unknown"


@dataclass
class CheckResult:
    id: str
    title: str
    status: CheckStatus
    severity: Severity
    detail: str = ""
    fix_op: str | None = None

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["status"] = self.status.value
        d["severity"] = self.severity.value
        return d


@dataclass
class AuditReport:
    root: str
    layout: LayoutClass
    project_name: str
    boot_exe: str | None
    checks: list[CheckResult] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "root": self.root,
            "layout": self.layout.value,
            "project_name": self.project_name,
            "boot_exe": self.boot_exe,
            "checks": [c.to_dict() for c in self.checks],
            "notes": list(self.notes),
            "fail_count": sum(1 for c in self.checks if c.status == CheckStatus.FAIL),
            "warn_count": sum(1 for c in self.checks if c.status == CheckStatus.WARN),
        }

    def failing_ops(self) -> list[str]:
        ops: list[str] = []
        for c in self.checks:
            if c.status in (CheckStatus.FAIL, CheckStatus.WARN) and c.fix_op:
                if c.fix_op not in ops:
                    ops.append(c.fix_op)
        return ops


@dataclass
class MigrateOptions:
    """User choices for apply (setup-host exclusively)."""

    disc: str | None = None
    project_name: str | None = None
    boot_exe: str | None = None
    players: int = 2
    zip_prefix: str | None = None
    github_owner: str | None = None
    github_repo: str | None = None
    window_title: str | None = None
    enable_recomp_ui: bool = True
    enable_wizard: bool = True
    enable_netplay: bool = False
    lobby_url: str = "ws://netplay.retcomm.net:8765"
    enable_ci: bool = True
    relocate_boxart: bool = True
    rewrite_cmake: bool = True
    merge_gitignore: bool = True
    probe_disc: bool = False
    record_pins: bool = True
    force: bool = False
    only: list[str] = field(default_factory=list)
    skip: list[str] = field(default_factory=list)
    dry_run: bool = False

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class PlanStep:
    op_id: str
    title: str
    detail: str = ""
    selected: bool = True

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class Plan:
    root: str
    layout: LayoutClass
    steps: list[PlanStep] = field(default_factory=list)
    options: MigrateOptions = field(default_factory=MigrateOptions)

    def to_dict(self) -> dict[str, Any]:
        return {
            "root": self.root,
            "layout": self.layout.value,
            "steps": [s.to_dict() for s in self.steps],
            "options": self.options.to_dict(),
        }


@dataclass
class ApplyResult:
    op_id: str
    ok: bool
    message: str
    changed_paths: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

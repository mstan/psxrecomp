"""Build an ordered migration plan from an audit + options."""

from __future__ import annotations

from pathlib import Path

from .detect import audit_project
from .models import AuditReport, LayoutClass, MigrateOptions, Plan, PlanStep


# Topological order for apply. Ops not listed are appended at the end.
_OP_ORDER = [
    "rename_psxrecomp_submodule",
    "repair_psxrecomp_submodule",
    "ensure_psxrecomp_submodule",
    "ensure_recomp_ui_submodule",
    "emit_codegen_setup",
    "emit_version",
    "emit_symbols_toml",
    "emit_sync_symbols",
    "merge_gitignore",
    "emit_mods_preloaded",
    "relocate_boxart",
    "emit_boxart_stub",
    "ensure_app_icon",
    "rewrite_cmake_setup_host",
    "emit_packager",
    "emit_ci_workflow",
    "annotate_legacy_packaging",
    "probe_disc_refresh",
    "record_framework_pins",
    "patch_readme_metrics",
]

_OP_TITLES = {
    "rename_psxrecomp_submodule": "Consolidate on psxrecomp/ (remove psxrecomp-v4)",
    "repair_psxrecomp_submodule": "Repair broken psxrecomp/ git checkout",
    "ensure_psxrecomp_submodule": "Add psxrecomp submodule",
    "ensure_recomp_ui_submodule": "Add recomp-ui submodule",
    "emit_codegen_setup": "Emit codegen_setup.c / .h",
    "emit_version": "Emit VERSION",
    "emit_symbols_toml": "Emit symbols.toml stub",
    "emit_sync_symbols": "Install tools/sync_symbols.py",
    "merge_gitignore": "Merge setup-host .gitignore rules",
    "emit_mods_preloaded": "Stub mods/preloaded catalog",
    "relocate_boxart": "Relocate boxart → launcher_assets/",
    "emit_boxart_stub": "Create launcher_assets stub dir",
    "ensure_app_icon": "Install assets/psxrecomp app icon",
    "rewrite_cmake_setup_host": "Rewrite CMakeLists.txt (setup-host)",
    "emit_packager": "Emit scripts/package_setup_release.sh",
    "emit_ci_workflow": "Emit setup-host release.yml",
    "annotate_legacy_packaging": "Annotate legacy prebuilt packaging",
    "probe_disc_refresh": "Refresh disc identity via probe_disc.py",
    "record_framework_pins": "Write framework_pins.txt",
    "patch_readme_metrics": "Patch README badges, RetComM Launcher, and R.A.I.D. footer",
}


def build_plan(
    root: Path,
    options: MigrateOptions | None = None,
    report: AuditReport | None = None,
) -> Plan:
    root = root.resolve()
    options = options or MigrateOptions()
    report = report or audit_project(root)

    wanted = set(report.failing_ops())

    # Option-gated extras
    if options.probe_disc and options.disc:
        wanted.add("probe_disc_refresh")
    if options.record_pins:
        wanted.add("record_framework_pins")
    if options.merge_gitignore:
        # only if audit asked for it — already in failing_ops when needed
        pass
    if options.relocate_boxart:
        pass  # already from audit
    if not options.rewrite_cmake:
        wanted.discard("rewrite_cmake_setup_host")
    if not options.enable_ci:
        wanted.discard("emit_ci_workflow")
    if not options.probe_disc:
        wanted.discard("probe_disc_refresh")
    if not options.record_pins:
        wanted.discard("record_framework_pins")

    if options.only:
        wanted = {o for o in wanted if o in options.only} | set(options.only)
    if options.skip:
        wanted -= set(options.skip)

    # Force-include rewrite when legacy cmake and rewrite enabled
    if options.rewrite_cmake and report.layout == LayoutClass.LEGACY_PACKAGING:
        wanted.add("rewrite_cmake_setup_host")
        wanted.add("emit_codegen_setup")
        wanted.add("emit_packager")
        if options.enable_ci:
            wanted.add("emit_ci_workflow")

    # Always ensure wizard/codegen for setup-host policy when rewriting
    if "rewrite_cmake_setup_host" in wanted:
        wanted.add("emit_codegen_setup")

    ordered = [op for op in _OP_ORDER if op in wanted]
    for op in sorted(wanted):
        if op not in ordered:
            ordered.append(op)

    steps = [
        PlanStep(
            op_id=op,
            title=_OP_TITLES.get(op, op),
            detail=next(
                (c.detail for c in report.checks if c.fix_op == op),
                "",
            ),
            selected=True,
        )
        for op in ordered
    ]

    return Plan(
        root=str(root),
        layout=report.layout,
        steps=steps,
        options=options,
    )

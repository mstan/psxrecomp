#!/usr/bin/env python3
"""Guard the catalog-diagnostic path from parser to launcher.

A manifest that will not parse produces a package with no features, so it has
no row anywhere in the Mods UI and no (package_id, feature_id) to hang a
per-feature diagnostic on. Until this path existed the failure was dropped in
silence: a mod author's typo produced a mod that simply did not exist, with
nothing anywhere explaining why.

Every link in that chain is a place where the report can go missing again
without anything failing to compile, which is why each one is asserted here.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI_ROOT = ROOT.parent / "recomp-ui"

MOD_PACKAGES_H = (ROOT / "runtime/include/mod_packages.h").read_text(encoding="utf-8")
MOD_PACKAGES = (ROOT / "runtime/src/mod_packages.cpp").read_text(encoding="utf-8")
MOD_RUNTIME = (ROOT / "runtime/src/mod_runtime.cpp").read_text(encoding="utf-8")

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


# 1. The parser collects the failure instead of dropping it.
check("scan_errors" in MOD_PACKAGES_H,
      "ModPackageManager must expose scan_errors()")
check("scan_errors_.push_back" in MOD_PACKAGES,
      "a manifest that fails to parse must be recorded, not skipped silently")
check("scan_errors_.clear()" in MOD_PACKAGES,
      "scan() must clear stale scan errors before rescanning")

# The silent `continue` this replaced took the parse error and threw it away.
parse_fail = MOD_PACKAGES.split("if (!read_manifest(manifest, package, &parse_error))")
check(len(parse_fail) == 2, "the manifest parse-failure branch moved or changed shape")
if len(parse_fail) == 2:
    branch = parse_fail[1][:400]
    check("scan_errors_.push_back" in branch,
          "the parse-failure branch must record the error before continuing")

# 2. Runtime source must not report it by logging: CLAUDE.md rule 3.
for line in MOD_RUNTIME.splitlines():
    stripped = line.strip()
    if stripped.startswith("*") or stripped.startswith("/*") or stripped.startswith("//"):
        continue
    check(not ("fprintf" in line and "scan_error" in line),
          "scan errors must not be reported with fprintf (CLAUDE.md rule 3)")

# 3. The provider exposes them on their own channel.
check("provider_catalog_diagnostic_count" in MOD_RUNTIME and
      "provider_catalog_diagnostic_get" in MOD_RUNTIME,
      "mod_runtime must implement the catalog diagnostic provider")
check("state().manager.scan_errors()" in MOD_RUNTIME,
      "the catalog diagnostic provider must read scan_errors()")
provider_table = MOD_RUNTIME.split("provider_feature_resource_set_path,")
check(len(provider_table) >= 2, "the provider vtable moved")
if len(provider_table) >= 2:
    check("provider_catalog_diagnostic_count" in provider_table[-1] and
          "provider_catalog_diagnostic_get" in provider_table[-1],
          "the catalog diagnostic callbacks must be registered in the vtable, "
          "not merely defined")

# 4. The ABI declares the channel, appended so older providers stay valid.
launcher_h = UI_ROOT / "src/recomp_launcher.h"
if launcher_h.is_file():
    ABI = launcher_h.read_text(encoding="utf-8")
    check("catalog_diagnostic_count" in ABI and "catalog_diagnostic_get" in ABI,
          "the launcher ABI must declare the catalog diagnostic channel")
    provider_struct = ABI.split("typedef struct RecompLauncherCModProvider {")[1]
    provider_struct = provider_struct.split("} RecompLauncherCModProvider;")[0]
    check(provider_struct.index("catalog_diagnostic_count") >
          provider_struct.index("feature_resource_set_path"),
          "catalog diagnostics must be APPENDED to the provider struct; "
          "inserting in the middle silently reinterprets every later callback")

    # 5. The launcher draws them, and above the Features/Packages switch --
    #    a package that failed to parse is missing from BOTH views.
    ui = UI_ROOT / "src/common/backends/imgui/launcher_imgui.cpp"
    if ui.is_file():
        UI = ui.read_text(encoding="utf-8")
        check("draw_mod_catalog_diagnostics" in UI,
              "the launcher must render catalog diagnostics")
        check("catalog_diagnostic_count(mods->ctx)" in UI,
              "the launcher must call the catalog diagnostic provider")
        draw_mods = UI.split("void draw_mods(LauncherModel* m, const LauncherTheme& th) {")
        check(len(draw_mods) == 2, "draw_mods moved or changed shape")
        if len(draw_mods) == 2:
            body = draw_mods[1].split("\n}\n")[0]
            check("draw_mod_catalog_diagnostics" in body,
                  "catalog diagnostics must be drawn by draw_mods, so they "
                  "appear in both the Features and Packages views")
            if "draw_mod_catalog_diagnostics" in body and "mod_show_packages" in body:
                check(body.index("draw_mod_catalog_diagnostics") <
                      body.index("if (m->mod_show_packages)"),
                      "catalog diagnostics must be drawn before the view "
                      "switch, not inside one view")

for failure in failures:
    print(f"FAIL: {failure}")
if failures:
    print(f"{len(failures)} catalog diagnostic test(s) failed")
    raise SystemExit(1)
print("mod catalog diagnostic tests passed")

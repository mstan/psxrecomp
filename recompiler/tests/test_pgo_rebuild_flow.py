"""No real build, game launch, cache cleanup, or profile mutation."""
import argparse
import contextlib
import io
from pathlib import Path
import sys
from unittest import mock

root = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(root), str(root / "tools")]
import psxrecomp_cli as cli
import compile_overlays as overlays

# Existing source paths satisfy the file guards; ALL consumers of their
# contents and all mutating/build/process operations are mocked below.
def check_flow(fail_training=False):
    args = argparse.Namespace(config=str(root / "CLAUDE.md"), project_root=str(root),
        build_dir="review-not-created", target="psx-runtime", exe_basename="psx-runtime",
        no_pgo=True, force_pgo=False, disc=str(root / "CLAUDE.md"), cmake_extra=[],
        train_secs=100, train_runs=1)
    progress = mock.Mock()
    events = []
    def configure(*a, **kw):
        events.append(("configure", kw["pgo"], kw["extra"][-1]))
    def train(*a, **kw):
        events.append(("train", kw["train_secs"], kw["train_runs"]))
        if fail_training:
            raise RuntimeError("synthetic training failure")
    with contextlib.ExitStack() as stack:
        replacements = dict(load_sections=lambda _: {},
            activate_embedded_toolchain=lambda *a: True,
            clamp_future_mtimes=lambda *a, **kw: 0,
            _cmake_configure=configure,
            _cmake_build=lambda *a: events.append(("build",)),
            run_pgo_train=train,
            _resolve_runtime_exe=lambda *a: (root / "fake-runtime.exe", None))
        for name, replacement in replacements.items():
            stack.enter_context(mock.patch.object(cli, name, replacement))
        result = cli.cmd_pgo_train(args, progress)
    expected = [("configure", "generate", "-DPSX_DEBUG_TOOLS=ON"), ("build",), ("train", 100, 1)]
    if not fail_training:
        expected += [("configure", "use", "-DPSX_DEBUG_TOOLS=OFF"), ("build",)]
        assert result == cli.EXIT_OK
        assert progress.result.call_args.kwargs["ok"] is True
    else:
        assert result == cli.EXIT_ERROR
        progress.result.assert_not_called()
        progress.error.assert_called_once()
    assert events == expected, events
    print("PGO flow", "failure" if fail_training else "success", events)

check_flow()
check_flow(True)
with mock.patch.object(overlays, "_toolchain_env", return_value=({}, "fake-toolchain")), \
     mock.patch.object(overlays.subprocess, "run", return_value=mock.Mock(returncode=0)) as run, \
     contextlib.redirect_stdout(io.StringIO()):
    assert overlays._compile_dll_direct("fake.c", "fake.dll", [], gcc="fake-gcc")
command = run.call_args.args[0]
assert "-O2" in command and "-shared" in command
assert not any("profile" in arg.lower() for arg in command)
print("Overlay DLL command (mocked subprocess):", command)
print("PASS: successful PGO finishes use/debug-OFF; failures report failure; overlay DLL command has no PGO flags")

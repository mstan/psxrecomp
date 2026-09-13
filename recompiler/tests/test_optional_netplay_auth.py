"""Preprocess actual main.cpp guards without needing SDL/UI/net dependencies.

Only unrelated include directives are removed; conditionals and production
account code stay intact. This is a feature-boundary test, not a C++ link test.
Real no-netplay title compile/link regression is additionally required.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--source", type=Path,
                    default=Path(__file__).resolve().parents[2] / "runtime/src/main.cpp")
    args = ap.parse_args()
    source = args.source.read_text(encoding="utf-8")
    lines = source.splitlines()
    source = "\n".join(line for line in lines if not re.match(r"\s*#\s*include\b", line)
                       or "recomp_net/" in line)
    with tempfile.TemporaryDirectory(prefix="psx-auth-guard-") as tmp:
        root = Path(tmp)
        unit = root / "main-guards.cpp"
        unit.write_text(source, encoding="utf-8")
        is_msvc = Path(args.cxx).stem.lower() in ("cl", "clang-cl")
        for enabled in (False, True):
            if enabled:
                (root / "recomp_net").mkdir()
                (root / "recomp_net/auth.h").write_text("RNET_AUTH_HEADER_INCLUDED\n")
                (root / "recomp_net/chat_filter.h").write_text("RNET_CHAT_HEADER_INCLUDED\n")
            flags = ["RECOMP_LAUNCHER=1", "RECOMP_LAUNCHER_HAS_ACCOUNT=1",
                     "SDL_VERSION_ATLEAST(x,y,z)=1", "DEFAULT_DEBUG_PORT=4370"]
            if enabled:
                flags += ["PSX_HAS_RECOMP_NET=1", "PSX_HAS_LOBBY_CLIENT=1"]
            if is_msvc:
                cmd = [args.cxx, "/nologo", "/EP", "/TP", "/I"+str(root),
                       *["/D"+f for f in flags], str(unit)]
            else:
                cmd = [args.cxx, "-E", "-P", "-x", "c++", "-I", str(root),
                       *["-D"+f for f in flags], str(unit)]
            p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", timeout=30)
            assert p.returncode == 0, p.stderr
            if enabled:
                for token in ("RNET_AUTH_HEADER_INCLUDED", "rnet_account_pump()",
                              "ae_np_account_login_begin", "account_available = ae_np_account_available"):
                    assert token in p.stdout, f"enabled path lost {token}"
            else:
                assert "rnet_account_" not in p.stdout, "offline launcher retained account linkage"
                assert "RNET_ACCOUNT_" not in p.stdout, "offline launcher retained account state dependency"
        print("PASS: offline launcher needs no auth headers/symbols; enabled account path retained")

if __name__ == "__main__":
    main()

"""Run source-backed regressions and actual standalone C++ rules; never run a server."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, help="Compatible AzerothCore reference checkout")
    parser.add_argument("--compiler", help="cl in a VS developer shell, or a C++20 compiler such as g++")
    parser.add_argument("--python-only", action="store_true")
    args = parser.parse_args()
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    if args.source_root:
        os.environ["RAID_MANAGER_SOURCE_ROOT"] = str(args.source_root.resolve())
    from source_context import reference_root
    root = reference_root()
    module = Path(__file__).resolve().parents[1]
    output = module / "tools/.build"
    output.mkdir(exist_ok=True)
    for script in ["test_source_manifest.py", "test_candidate_catalog.py", "test_candidate_indexes.py",
                   "test_preparation_sets.py"]:
        command = [sys.executable, str(module / "tools" / script)]
        if script in ["test_candidate_indexes.py", "test_preparation_sets.py"]:
            command += ["--write-oracle"]
        subprocess.run(command, check=True)
    if args.python_only:
        return
    compiler = args.compiler or ("cl" if os.name == "nt" else "c++")
    if not shutil.which(compiler):
        raise SystemExit("C++ compiler unavailable; use a VS developer shell or --compiler, or --python-only")
    source = [module / "tools/equipment_candidate_rules.cpp.test", module / "src/RaidEquipmentCandidateRules.cpp",
              module / "tools/equipment_candidate_indexes.cpp.test", module / "tools/preparation_policy.cpp.test"]
    includes = [module / "src", root / "src/common", output]
    exe = output / ("candidate-tests.exe" if os.name == "nt" else "candidate-tests")
    if Path(compiler).stem.lower() in ["cl", "clang-cl"]:
        command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/MP1"]
        command += ["/I" + str(p) for p in includes] + ["/TP"] + list(map(str, source))
        command += ["/Fo" + str(output) + "/", "/Fe" + str(exe)]
    else:
        command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror"]
        command += ["-I" + str(p) for p in includes] + ["-x", "c++"] + list(map(str, source))
        command += ["-o", str(exe)]
    subprocess.run(command, cwd=output, check=True)
    subprocess.run([str(exe)], cwd=output, check=True)


if __name__ == "__main__":
    main()

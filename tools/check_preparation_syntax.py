"""MSVC syntax-only offline integration check, using the last approved build flags.

Does not invoke CMake/MSBuild, create worldserver, or write the Release cache.
Run from a VS developer environment. Only four Raid Manager sources compile.
"""

import argparse
import ctypes
from pathlib import Path
import subprocess


module = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-log', required=True, type=Path, help='A compatible MSVC build log')
log = parser.parse_args().build_log.resolve()
(module / 'tools/.build').mkdir(exist_ok=True)
line = next(line.strip() for line in log.read_text(encoding="utf-8-sig").splitlines()
            if "CL.exe /c " in line and "RaidEquipmentCandidateRules.cpp" in line)
compiler, arguments = line.split(" /c ", 1)
count = ctypes.c_int()
parse = ctypes.windll.shell32.CommandLineToArgvW
parse.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
values = parse("cl.exe " + arguments, ctypes.byref(count))
args = [values[i] for i in range(1, count.value)]
ctypes.windll.kernel32.LocalFree(ctypes.cast(values, ctypes.c_void_p))
flags = [v for v in args if not v.startswith(("/Fo", "/Fd", "/MP")) and not v.endswith(".cpp")]
sources = [module / "src" / name for name in
           ["RaidEquipmentCandidateRules.cpp", "RaidEquipmentSelection.cpp", "RaidPreparation.cpp", "RaidWork.cpp"]]
for source in sources:
    print("Syntax only:", source.name, flush=True)
    result = subprocess.run([compiler, "/Zs"] + flags + [str(source)], cwd=module / "tools/.build")
    if result.returncode:
        raise SystemExit(result.returncode)
print("All four production sources passed syntax-only integration checking.")

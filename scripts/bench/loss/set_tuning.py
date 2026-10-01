"""Set or remove "native_tuning" in an edition's settings.json, atomically.

    python set_tuning.py <settings.json> [<spec>]    (no or an empty spec removes it)

Every other key is kept as it is; the file is replaced by a rename, so the
server never reads a half-written one. PowerShell 5.1 drops an empty-string
argument on its way to a native command, hence the optional spec.
"""
import json
import os
import sys
import tempfile

path = sys.argv[1]
spec = sys.argv[2].strip() if len(sys.argv) > 2 else ""
with open(path, encoding="utf-8") as f:
    obj = json.load(f)
if spec:
    obj["native_tuning"] = spec
else:
    obj.pop("native_tuning", None)
folder = os.path.dirname(os.path.abspath(path))
fd, tmp = tempfile.mkstemp(dir=folder, prefix=".settings-", suffix=".json")
with os.fdopen(fd, "w", encoding="utf-8") as f:
    json.dump(obj, f, indent=4, ensure_ascii=False)
os.replace(tmp, path)
print("native_tuning = %r" % obj.get("native_tuning", ""))

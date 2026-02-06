"""
PlatformIO Post-Script: cleanup temporary folders created during buildfs
Removes transient directories created under .pio/build/<env>/ by the buildfs
process to avoid leaving large temporary copies in the repository workspace.

Safe-by-design rules:
- Only removes directories inside the project's .pio/build/<env>/ folder
- Only removes a small, well-known set of names created by buildfs: "data",
  "fs" and directories that match the pattern "tmp*" or "*_data_copy"
- Will NOT remove arbitrary paths outside .pio/build/<env>/

Adds informative logging so the build output shows what was cleaned.
"""

from SCons.Script import Import
Import("env")
import os
import shutil
import fnmatch

project_dir = env["PROJECT_DIR"]
pioenv = env.get("PIOENV", "")

if not pioenv:
    print("[cleanup_buildfs_temp] PIOENV not set; skipping cleanup")
else:
    build_dir = os.path.join(project_dir, ".pio", "build", pioenv)
    if not os.path.isdir(build_dir):
        print(f"[cleanup_buildfs_temp] Build dir not found: {build_dir}; nothing to clean")
    else:
        # proceed with cleanup
        pass

# Candidate directory names (only inside build_dir)
candidate_names = ["data", "fs"]
# Patterns to match (safer than blanket removal)
pattern_names = ["tmp*", "*_data_copy"]

# Only perform cleanup when build_dir is defined
if 'build_dir' in globals() and os.path.isdir(build_dir):
    removed = []
    for entry in os.listdir(build_dir):
        path = os.path.join(build_dir, entry)
        if not os.path.isdir(path):
            continue

        # Exact-name removals
        if entry in candidate_names:
            try:
                shutil.rmtree(path)
                removed.append(path)
            except Exception as e:
                print(f"[cleanup_buildfs_temp] Failed to remove {path}: {e}")
            continue

        # Pattern-based removals
        for pat in pattern_names:
            if fnmatch.fnmatch(entry, pat):
                try:
                    shutil.rmtree(path)
                    removed.append(path)
                except Exception as e:
                    print(f"[cleanup_buildfs_temp] Failed to remove {path}: {e}")
                break

    if removed:
        print("[cleanup_buildfs_temp] Removed temporary buildfs directories:")
        for p in removed:
            print("  - " + p)
    else:
        print("[cleanup_buildfs_temp] No temporary buildfs directories found to remove")
else:
    # Nothing to do (either PIOENV missing or build_dir absent)
    pass

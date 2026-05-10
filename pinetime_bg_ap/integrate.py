#!/usr/bin/env python3
"""
Integrates BgAp app into an InfiniTime 1.14.0 source tree.
Usage: python3 integrate.py <path-to-infinitime>
"""

import sys, shutil
from pathlib import Path

def patch(path: Path, marker: str, insertion: str, after: bool = True, once: bool = True):
    text = path.read_text()
    if once and insertion.strip() in text:
        print(f"  [skip] already patched: {path.name}")
        return
    if marker not in text:
        print(f"  [WARN] marker not found in {path.name}: {repr(marker[:60])}")
        return
    replacement = (marker + "\n" + insertion) if after else (insertion + "\n" + marker)
    text = text.replace(marker, replacement, 1)
    path.write_text(text)
    print(f"  [ok]   patched: {path.name}")

def main():
    if len(sys.argv) < 2:
        print("Usage: integrate.py <infinitime-root>")
        sys.exit(1)

    it  = Path(sys.argv[1]).resolve()
    src = Path(__file__).parent / "src"

    if not (it / "src" / "displayapp").is_dir():
        print(f"ERROR: {it} does not look like an InfiniTime source tree")
        sys.exit(1)

    # ── Copy source files ──────────────────────────────────────────────
    dst_screens = it / "src" / "displayapp" / "screens"
    dst_ble     = it / "src" / "components" / "ble"

    for f in ["BgApApp.h", "BgApApp.cpp", "metis240.c"]:
        shutil.copy(src / f, dst_screens / f)
        print(f"  [cp]   {f} → screens/")
    for f in ["BleApClient.h", "BleApClient.cpp"]:
        shutil.copy(src / f, dst_ble / f)
        print(f"  [cp]   {f} → components/ble/")

    # ── Apps.h.in: add BgAp to the enum ──────────────────────────────
    apps_hin = it / "src" / "displayapp" / "apps" / "Apps.h.in"
    patch(apps_hin,
          marker="      Weather\n    };",
          insertion="      BgAp,",
          after=False)

    # ── apps/CMakeLists.txt: enable the app ───────────────────────────
    apps_cmake = it / "src" / "displayapp" / "apps" / "CMakeLists.txt"
    patch(apps_cmake,
          marker='set(DEFAULT_USER_APP_TYPES "${DEFAULT_USER_APP_TYPES}, Apps::Navigation")',
          insertion='    set(DEFAULT_USER_APP_TYPES "${DEFAULT_USER_APP_TYPES}, Apps::BgAp")',
          after=True)

    # ── UserApps.h: include BgApApp so the AppTraits specialisation
    #    is visible when userApps is evaluated ─────────────────────────
    user_apps_h = it / "src" / "displayapp" / "UserApps.h"
    patch(user_apps_h,
          marker='#include "displayapp/screens/Alarm.h"',
          insertion='#include "displayapp/screens/BgApApp.h"',
          after=False)

    # ── src/CMakeLists.txt: add our .cpp files to the build ───────────
    src_cmake = it / "src" / "CMakeLists.txt"

    patch(src_cmake,
          marker="        displayapp/screens/Alarm.cpp",
          insertion="        displayapp/screens/BgApApp.cpp",
          after=True)

    patch(src_cmake,
          marker="        components/ble/MusicService.cpp",
          insertion="        components/ble/BleApClient.cpp",
          after=True)

    # ── FreeRTOSConfig.h: trim heap by 256 bytes to make room ────────
    # BleApClient pulls in GAP central-role BSS (ble_gap_disc) not previously
    # linked. InfiniTime 1.14 has only a 16-byte RAM margin; this gives 136 bytes.
    freertos_cfg = it / "src" / "FreeRTOSConfig.h"
    OLD_HEAP = "#define configTOTAL_HEAP_SIZE                   (1024 * 40)"
    NEW_HEAP = "#define configTOTAL_HEAP_SIZE                   ((1024 * 40) - 256)"
    text = freertos_cfg.read_text()
    if NEW_HEAP in text:
        print(f"  [skip] already patched: {freertos_cfg.name}")
    elif OLD_HEAP in text:
        freertos_cfg.write_text(text.replace(OLD_HEAP, NEW_HEAP))
        print(f"  [ok]   patched: {freertos_cfg.name}")

    print("\nDone. Build with: ./build.sh")

if __name__ == "__main__":
    main()

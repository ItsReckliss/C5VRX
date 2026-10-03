#!/usr/bin/env python3
"""Verify the isolated C5VRX-4 snapshot, also called by IDF configuration."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent

def run(args):
    subprocess.run(args, cwd=ROOT, check=True)

def main():
    # Do not silently cross-compile the host regressions with the IDF compiler.
    cc = os.environ.get("C5VRX4_HOST_CC", "gcc")
    tracked = list(ROOT.glob("*.bsasm")) + [ROOT / "cvbs_tables.h"]
    before = {p: p.read_bytes() for p in tracked}
    run([sys.executable, "generate_pipeline.py"])
    run([sys.executable, "generate_phase8.py"])
    assert all(p.read_bytes() == content for p, content in before.items()), "stale generated program/table"
    cmake = (ROOT / "CMakeLists.txt").read_text()
    assert "C5VRX_ROOT" not in cmake and "EXTRA_COMPONENT_DIRS" not in cmake, "shared-main dependency"
    assert (ROOT / "partitions.csv").is_file() and (ROOT / "sdkconfig.base.defaults").is_file()
    video = (ROOT / "main/video.c").read_text()
    semantic = video.split("static int video_semantic_observe(", 1)[1].split("typedef struct {", 1)[0]
    assert "c5v4_cvbs_analyze" in semantic and "phase5_pair_is_sync" not in semantic
    assert "afc_ticks" not in video and "afc2_ctrl_decide" in video
    assert "goto afc_control;" in video and "phy_rx_lab_try_actuator(afc_epoch.phy)" in video
    assert '"force_ultra_v2"' in (ROOT / "pipeline.c").read_text()

    cases = [
        ("demod_quality", []), ("range_control", []), ("fusion_receiver", []),
        ("menu_raster", ["main/menu_raster.c", "-lm"]),
        ("arc", ["main/arc_phy.c"]),
        ("direct_gain_v2", ["main/direct_gain_v2.c", "main/arc_phy.c"]),
        ("direct_gain_v3", ["main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("rx_control_epoch", ["main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("rx_auto_lab", ["main/rx_auto_lab.c"]),
        ("arc_v3", ["main/arc_v3_controller.c"]),
        ("arc_v5_autotune", ["main/arc_v5_autotune.c", "main/arc_v3_controller.c"]),
        ("phase8_envelope", ["main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("cvbs_level", ["-I.", "cvbs_level.c"]),
        ("cvbs_snapshot", ["-I."]),
        ("afc_state", []), ("afc_v2", ["-lm"]), ("afc_v2_ctrl", ["-lm"]),
        ("integration", ["-DC5VRX4_EXPERIMENT=1", "-I.", "-Itools/phy_lab_stubs", "main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("c5vrx4_gate", ["-pthread", "-I.", "-Itools/phy_lab_stubs"]),
    ]
    with tempfile.TemporaryDirectory(prefix="c5vrx4-verify-") as td:
        for name, extra in cases:
            target = str(Path(td) / name)
            run([cc, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-Imain",
                 f"tools/test_{name}.c", *extra, "-o", target])
            run([target])
        for pinned in (False, True):
            target = str(Path(td) / f"phy_{pinned}")
            run([cc, "-pthread", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Itools/phy_lab_stubs", "-Imain",
                 *(["-DC5VRX_PHY_RX_LAB_PINNED=1"] if pinned else []),
                 "tools/test_phy_rx_lab.c", "-o", target])
            run([target])
        target = str(Path(td) / "unwrap")
        run([cc, "-O3", "-std=c11", "unwrap_oracle.c", "-o", target])
        run([target])
    for name in ("test_unwrap.py", "test_cvbs.py", "tools/test_phase8_hr_live.py",
                 "tools/test_fm_hc.py", "tools/check_golden_two_slot.py"):
        run([sys.executable, name])
    print("PASS: isolated C5VRX-4 integration, 21 C regressions, exhaustive unwrap and source-driven DSP tests")

if __name__ == "__main__":
    main()

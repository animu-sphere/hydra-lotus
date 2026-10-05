# SPDX-License-Identifier: Apache-2.0
import os
import json

from pxr import Gf, Vt


def _frames():
    path = os.environ["LOTUS_HYDRA_EVIDENCE"]
    if not os.path.exists(path):
        return []
    result = []
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            fields = {}
            for field in line.split():
                key, value = field.split("=", 1)
                fields[key] = int(value)
            result.append(fields)
    return result


def _render(app_controller, phase):
    before = len(_frames())
    app_controller._stageView.SetForceRefresh(True)
    app_controller._stageView.updateView()
    image_root = os.environ["LOTUS_HYDRA_IMAGE"]
    app_controller._takeShot(
        f"{image_root}-{phase}.png", iterations=10, waitForConvergence=True)
    frames = _frames()
    assert len(frames) > before, f"no Hydra frame completed for {phase}"
    assert frames[-1]["buffers_written"] >= 2
    assert frames[-1]["width"] > 0 and frames[-1]["height"] > 0
    # Waiting for convergence leaves a finished image: the bootstrap's
    # one frame, or every path-traced sample the render settings ask for.
    assert frames[-1]["converged"] == 1, f"{phase} did not converge"
    assert all(frame["validation_messages"] == 0 for frame in frames), \
        "Vulkan validation reported messages during a Hydra frame"
    # One device and pipeline serve every frame, and the targets are
    # recreated only when the AOV size changes.
    assert all(frame["renderer_creations"] == 1 for frame in frames), \
        "the Vulkan renderer was recreated between Hydra frames"
    sizes = [(frame["width"], frame["height"]) for frame in frames]
    size_changes = sum(1 for a, b in zip(sizes, sizes[1:]) if a != b)
    assert frames[-1]["target_creations"] == 1 + size_changes, \
        "the offscreen targets were not created exactly once per AOV size"
    return frames[-1]


def testUsdviewInputFunction(appController):
    appController._dataModel.viewSettings.showBBoxes = False
    appController._dataModel.viewSettings.showHUD = False

    first = _render(appController, "first-frame")
    with open(os.environ["LOTUS_RENDERER_REPORT"], encoding="utf-8") as stream:
        checks = {check["id"]: check for check in json.load(stream)["checks"]}
    if checks["renderer.ray_query.capability"]["status"] == "pass":
        assert first["ray_query"] == 1, "Hydra did not use the available ray-query path"
        assert first["samples"] == 64, "the first frame did not accumulate 64 samples"
    appController._dataModel.stage.GetPrimAtPath(
        "/World/Triangle").GetAttribute("points").Set(
            Vt.Vec3fArray([
                Gf.Vec3f(-0.4, -0.45, 0),
                Gf.Vec3f(0.4, -0.45, 0),
                Gf.Vec3f(0, 0.6, 0),
            ]))
    updated = _render(appController, "stable-update")
    assert updated["frame"] > first["frame"]
    assert updated["scene_revision"] > first["scene_revision"]
    assert updated["ray_query"] == first["ray_query"]
    assert updated["samples"] == first["samples"]
    # The smoke scene's one mesh is resident and instanced once; the point
    # edit replaces its geometry buffer instead of adding one.
    assert first["gpu_geometries"] == 1 and first["gpu_instances"] == 1
    assert updated["gpu_geometries"] == 1 and updated["gpu_instances"] == 1
    assert updated["geometry_uploads"] == first["geometry_uploads"] + 1
    # With acceleration structures, the mesh has one BLAS and one TLAS
    # instance, and the point edit builds exactly one more BLAS.
    if first["acceleration"] == 1:
        assert first["blas"] == 1 and first["tlas_instances"] == 1
        assert updated["blas"] == 1 and updated["tlas_instances"] == 1
        assert updated["blas_builds"] == first["blas_builds"] + 1

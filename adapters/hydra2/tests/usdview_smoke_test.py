# SPDX-License-Identifier: Apache-2.0
import os
import json
import time

from pxr import Gf, Vt, UsdLux


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
                fields[key] = (value if key in ("integrator", "light_sampling")
                               else int(value))
            result.append(fields)
    return result


def _image(phase):
    with open(f"{os.environ['LOTUS_HYDRA_IMAGE']}-{phase}.png", "rb") as stream:
        return stream.read()


def _render(app_controller, phase, temporal=False):
    before = len(_frames())
    app_controller._stageView.SetForceRefresh(True)
    app_controller._stageView.updateView()
    if temporal:
        # Qt's processEvents iteration count does not count rendered frames.
        # Observe five real completions without waiting for convergence.
        deadline = time.monotonic() + 10
        while len(_frames()) < before + 5 and time.monotonic() < deadline:
            app_controller._stageView.update()
            app_controller._processEvents(iterations=10, waitForConvergence=False)
        assert len(_frames()) >= before + 5, f"temporal stream stalled for {phase}"
    else:
        # Keep requesting paints instead of depending on a queued Qt timer.
        # testusdview's unbounded convergence wait can otherwise stall one
        # sample before completion without explaining which phase stopped.
        app_controller._processEvents(iterations=10, waitForConvergence=False)
        deadline = time.monotonic() + 15
        while (not app_controller._stageView._renderer.IsConverged()
               and time.monotonic() < deadline):
            app_controller._stageView.update()
            app_controller._processEvents(iterations=10, waitForConvergence=False)
        assert app_controller._stageView._renderer.IsConverged(), \
            f"progressive stream stalled for {phase}"
    image_root = os.environ["LOTUS_HYDRA_IMAGE"]
    app_controller._takeShot(
        f"{image_root}-{phase}.png", iterations=10,
        waitForConvergence=False)
    frames = _frames()
    assert len(frames) > before, f"no Hydra frame completed for {phase}"
    assert frames[-1]["buffers_written"] >= 2
    assert frames[-1]["width"] > 0 and frames[-1]["height"] > 0
    # Waiting for convergence leaves a finished image: the bootstrap's
    # one frame, or every path-traced sample the render settings ask for.
    assert frames[-1]["converged"] == (0 if temporal else 1), \
        f"{phase} has incorrect convergence"
    assert all(frame["validation_messages"] == 0 for frame in frames), \
        "Vulkan validation reported messages during a Hydra frame"
    with open(os.environ["LOTUS_RENDERER_REPORT"], encoding="utf-8") as stream:
        checks = {check["id"]: check for check in json.load(stream)["checks"]}
    synchronization = checks["renderer.validation.synchronization"]
    assert synchronization["status"] in ("pass", "skip"), synchronization
    if synchronization["status"] == "pass":
        assert all(frame["synchronization_validation"] == 1 for frame in frames), \
            "Hydra did not enable synchronization validation available to headless"
    else:
        assert synchronization.get("detail"), "unexplained synchronization SKIP"
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
    # The triangle is placed once and the point instancer's prototype three
    # times, each mesh's geometry resident once; the point edit replaces the
    # triangle's geometry buffer instead of adding one.
    assert first["gpu_geometries"] == 2 and first["gpu_instances"] == 4
    assert updated["gpu_geometries"] == 2 and updated["gpu_instances"] == 4
    assert updated["geometry_uploads"] == first["geometry_uploads"] + 1
    # With acceleration structures, each geometry has one BLAS and each
    # placement one TLAS instance, and the point edit refits one BLAS and
    # its dependent TLAS without rebuilding either.
    if first["acceleration"] == 1:
        assert first["blas"] == 2 and first["tlas_instances"] == 4
        assert updated["blas"] == 2 and updated["tlas_instances"] == 4
        assert updated["blas_builds"] == first["blas_builds"]
        assert updated["blas_updates"] == first["blas_updates"] + 1
        assert updated["tlas_builds"] == first["tlas_builds"]
        assert updated["tlas_updates"] == first["tlas_updates"] + 1

    # Deterministic mode: lotus:sampleIndex sets the accumulation's first
    # sample index, a change restarts the accumulation, and returning to the
    # default index renders the same image as before.
    before = len(_frames())
    appController._stageView.SetRendererSetting("lotus:sampleIndex", 4096)
    seeded = _render(appController, "sample-index")
    assert seeded["sample_index"] == 4096
    assert updated["sample_index"] == 0
    if seeded["ray_query"] == 1:
        assert seeded["samples"] == updated["samples"]
        assert any(frame["samples"] < seeded["samples"]
                   for frame in _frames()[before:]), \
            "changing lotus:sampleIndex did not restart the accumulation"
    appController._stageView.SetRendererSetting("lotus:sampleIndex", 0)
    restored = _render(appController, "sample-index-restored")
    assert restored["sample_index"] == 0
    assert _image("sample-index-restored") == _image("stable-update"), \
        "returning to sample index 0 did not reproduce the image"

    # Integrator selection: lotus:wavefront is a checkable item of the
    # Hydra Settings menu. Checking it restarts the accumulation with the
    # wavefront kernels installed beside the plugin, and unchecking it
    # renders the same image as before.
    assert restored["integrator"] == "reference"
    actions = [action for action in appController._ui.settingsFlagActions
               if action.key == "lotus:wavefront"]
    assert len(actions) == 1, "the Hydra Settings menu has no wavefront item"
    action = actions[0]
    assert action.isCheckable() and not action.isChecked()
    before = len(_frames())
    action.trigger()
    assert action.isChecked()
    wavefront = _render(appController, "wavefront")
    assert wavefront["integrator"] == "wavefront"
    if wavefront["ray_query"] == 1:
        frames = _frames()[before:]
        assert wavefront["samples"] == restored["samples"]
        assert any(frame["samples"] < wavefront["samples"] for frame in frames), \
            "checking the wavefront item did not restart the accumulation"
        assert any(frame["wavefront_rounds"] > 0 for frame in frames), \
            "no Hydra frame ran the wavefront kernels"
    action.trigger()
    assert not action.isChecked()
    reference = _render(appController, "integrator-restored")
    assert reference["integrator"] == "reference"
    assert reference["wavefront_rounds"] == 0
    assert _image("integrator-restored") == _image("stable-update"), \
        "returning to the reference integrator did not reproduce the image"

    # Light sampling: lotus:lightSampling is a checkable item too, checked
    # by default. Unchecking it restarts the accumulation with BSDF sampling
    # alone, and checking it again renders the same image as before.
    assert reference["light_sampling"] == "mis"
    actions = [action for action in appController._ui.settingsFlagActions
               if action.key == "lotus:lightSampling"]
    assert len(actions) == 1, "the Hydra Settings menu has no light sampling item"
    action = actions[0]
    assert action.isCheckable() and action.isChecked()
    before = len(_frames())
    action.trigger()
    assert not action.isChecked()
    brute_force = _render(appController, "bsdf-sampling")
    assert brute_force["light_sampling"] == "bsdf"
    if brute_force["ray_query"] == 1:
        assert brute_force["samples"] == reference["samples"]
        assert any(frame["samples"] < brute_force["samples"]
                   for frame in _frames()[before:]), \
            "unchecking the light sampling item did not restart the accumulation"
    action.trigger()
    assert action.isChecked()
    sampled = _render(appController, "light-sampling-restored")
    assert sampled["light_sampling"] == "mis"
    assert _image("light-sampling-restored") == _image("stable-update"), \
        "returning to light sampling did not reproduce the image"

    # Continuous temporal transport must never wait for progressive convergence.
    if sampled["ray_query"] != 1:
        return
    # A camera-following light changes illumination on camera motion, which
    # correctly resets radiance. Keep lighting fixed for reprojection checks.
    appController._dataModel.viewSettings.ambientLightOnly = False
    appController._dataModel.viewSettings.domeLightEnabled = False
    _render(appController, "temporal-fixed-lighting")
    actions = [action for action in appController._ui.settingsFlagActions
               if action.key == "lotus:temporal"]
    assert len(actions) == 1 and actions[0].isCheckable()
    temporal_action = actions[0]
    assert not temporal_action.isChecked()
    appController._stageView.SetRendererSetting("lotus:historySamples", 4)
    for wavefront in (False, True):
        appController._stageView.SetRendererSetting("lotus:wavefront", wavefront)
        before = len(_frames())
        temporal_action.trigger()
        label = "wavefront" if wavefront else "reference"
        static = _render(appController, f"temporal-{label}-static", temporal=True)
        frames = _frames()[before:]
        assert all(frame["temporal_radiance"] == 1 for frame in frames)
        assert static["samples"] == 1 and static["temporal_history_samples"] == 4
        assert static["temporal_valid_pixels"] > 0
        assert static["integrator"] == label
        camera = appController._dataModel.stage.GetPrimAtPath("/Camera")
        matrix = Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0.04 if not wavefront else 0.08, 0, 5))
        camera.GetAttribute("xformOp:transform").Set(matrix)
        before = len(_frames())
        _render(appController, f"temporal-{label}-camera", temporal=True)
        assert any(frame["temporal_motion_pixels"] > 0 for frame in _frames()[before:])
        assert all(frame["temporal_reset"] == 0 for frame in _frames()[before:])
        triangle = appController._dataModel.stage.GetPrimAtPath("/World/Triangle")
        triangle.GetAttribute("xformOp:translate").Set(
            Gf.Vec3d(0.04 if not wavefront else 0.08, 0, 0))
        before = len(_frames())
        _render(appController, f"temporal-{label}-object", temporal=True)
        assert any(frame["temporal_motion_pixels"] > 0 for frame in _frames()[before:])
        assert all(frame["temporal_reset"] == 0 for frame in _frames()[before:])
        before = len(_frames())
        appController._stageView.SetRendererSetting("lotus:historyReset", 1 if not wavefront else 2)
        _render(appController, f"temporal-{label}-reset", temporal=True)
        frames = _frames()[before:]
        assert sum(frame["temporal_reset"] == 3 for frame in frames) == 1
        assert frames[-1]["temporal_history"] == 1
        temporal_action.trigger()
        _render(appController, f"temporal-{label}-progressive")

    # Exercise installed reservoir shaders with fixed table-backed lighting.
    stage = appController._dataModel.stage
    sun = UsdLux.DistantLight.Define(stage, "/RestirSun")
    sun.CreateIntensityAttr(2)
    sun.CreateAngleAttr(0)
    sun.CreateColorAttr(Gf.Vec3f(3, 2, 1))
    appController._stageView.SetRendererSetting("convergedSamplesPerPixel", 4)
    appController._stageView.SetRendererSetting("lotus:restirCandidates", 2)
    appController._stageView.SetRendererSetting("lotus:restirHistoryCandidates", 8)
    actions = {action.key: action for action in appController._ui.settingsFlagActions}
    for key in ("lotus:restir", "lotus:restirTemporal", "lotus:restirSpatial"):
        assert key in actions and actions[key].isCheckable() and not actions[key].isChecked()
    for wavefront in (False, True):
        label = "wavefront" if wavefront else "reference"
        appController._stageView.SetRendererSetting("lotus:wavefront", wavefront)
        baseline = _render(appController, f"restir-{label}-baseline")
        actions["lotus:restir"].trigger()
        initial = _render(appController, f"restir-{label}-initial")
        assert initial["light_sampling"] == "restir" and initial["samples"] == 4
        actions["lotus:restirTemporal"].trigger()
        static = _render(appController, f"restir-{label}-static", temporal=True)
        assert static["restir"] == 1 and static["restir_history"] == 1
        assert static["restir_temporal"] == 1 and static["restir_spatial"] == 0
        assert static["temporal_radiance"] == 0 and static["samples"] == 1
        before = len(_frames())
        camera.GetAttribute("xformOp:transform").Set(
            Gf.Matrix4d(1).SetTranslate(Gf.Vec3d(0.12 if not wavefront else 0.16, 0, 5)))
        _render(appController, f"restir-{label}-camera", temporal=True)
        assert any(frame["temporal_motion_pixels"] > 0 for frame in _frames()[before:])
        assert all(frame["temporal_reset"] == 0 for frame in _frames()[before:])
        before = len(_frames())
        triangle.GetAttribute("xformOp:translate").Set(
            Gf.Vec3d(0.12 if not wavefront else 0.16, 0, 0))
        _render(appController, f"restir-{label}-object", temporal=True)
        assert any(frame["temporal_motion_pixels"] > 0 for frame in _frames()[before:])
        assert all(frame["temporal_reset"] == 0 for frame in _frames()[before:])
        actions["lotus:restirSpatial"].trigger()
        combined = _render(appController, f"restir-{label}-combined", temporal=True)
        assert combined["restir_spatial"] == 1 and combined["restir_history"] == 1
        before = len(_frames())
        appController._stageView.SetRendererSetting("lotus:historyReset", 3 if not wavefront else 4)
        _render(appController, f"restir-{label}-reset", temporal=True)
        assert sum(frame["temporal_reset"] == 3 for frame in _frames()[before:]) == 1
        actions["lotus:restirTemporal"].trigger()
        spatial = _render(appController, f"restir-{label}-spatial", temporal=True)
        assert spatial["restir_spatial"] == 1 and spatial["restir_history"] == 0
        # Leaving reuse flags checked must still restore progressive NEE/MIS.
        actions["lotus:restir"].trigger()
        restored = _render(appController, f"restir-{label}-restored")
        assert restored["light_sampling"] == "mis" and restored["samples"] == baseline["samples"]
        assert restored["restir"] == 0 and restored["restir_spatial"] == 0
        actions["lotus:restirSpatial"].trigger()

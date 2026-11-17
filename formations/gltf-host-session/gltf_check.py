# SPDX-License-Identifier: Apache-2.0
"""testusdview script of the glTF host session: the stage came through the
glTF file format plugin, its materials are MaterialX glTF PBR surfaces, and
Lotus drew the three cards in their colours."""
import time

from pxr import Usd, UsdShade


def _surfaces(stage):
    return [str(prim.GetPath())
            for prim in Usd.PrimRange(stage.GetPseudoRoot())
            if UsdShade.Shader(prim) and
            UsdShade.Shader(prim).GetShaderId() == "ND_gltf_pbr_surfaceshader"]


def testUsdviewInputFunction(appController):
    model = appController._dataModel
    stage = model.stage
    assert stage.GetRootLayer().GetFileFormat().formatId == "gltf", \
        stage.GetRootLayer().GetFileFormat().formatId
    surfaces = _surfaces(stage)
    assert len(surfaces) == 3, \
        f"Expected three glTF PBR surfaces, got {surfaces}"

    view = appController._stageView
    assert str(view.GetCurrentRendererId()) == "HdLotusRendererPlugin", \
        view.GetCurrentRendererId()
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.selection.clearPrims()
    view.SetForceRefresh(True)
    view.updateView()
    # Keep requesting paints until the progressive image has converged.
    deadline = time.monotonic() + 60
    appController._processEvents(iterations=10, waitForConvergence=False)
    while not view._renderer.IsConverged() and time.monotonic() < deadline:
        view.update()
        appController._processEvents(iterations=10, waitForConvergence=False)
    assert view._renderer.IsConverged(), "Lotus did not converge"
    appController._takeShot("gltf-cards.png", iterations=10,
                            waitForConvergence=False)

    image = appController.GrabViewportShot()
    assert not image.isNull(), "usdview returned an empty viewport image"
    red_pixels = green_pixels = blue_pixels = 0
    for y in range(0, image.height(), 2):
        for x in range(0, image.width(), 2):
            pixel = image.pixel(x, y)
            red = (pixel >> 16) & 255
            green = (pixel >> 8) & 255
            blue = pixel & 255
            red_pixels += red > max(green, blue) + 80
            green_pixels += green > max(red, blue) + 80
            blue_pixels += blue > max(red, green) + 80
    color_counts = (red_pixels, green_pixels, blue_pixels)
    assert min(color_counts) > 200, \
        f"Expected three visible cards, got {color_counts}"
    print("Lotus drew the glTF fixture "
          f"(R/G/B sample counts: {color_counts})")

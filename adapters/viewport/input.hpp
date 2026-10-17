// SPDX-License-Identifier: Apache-2.0
// The viewport's input events, without the window system's types, so that
// the navigation logic that consumes them is covered by window-free tests.
#pragma once

#include <cstdint>

namespace Lotus::viewport {

enum class EventType {
  Close,
  Resize,
  KeyDown,
  PointerDown,
  PointerUp,
  PointerMove,
  Scroll,
};

// Keys by what the viewport does with them.
enum class Key {
  Unknown,
  Escape,
  Fullscreen,
  // F1: hide or show the developer UI.
  ToggleUi,
  // F: restore the initial framing.
  Frame,
  // C: the next authored camera, then the free camera again.
  NextCamera,
  // R: reload the stage's layers.
  Reload,
  // Left and right arrows: one time code back or forward.
  TimeBack,
  TimeForward,
};

enum class Button {
  Left,
  Middle,
  Right,
};

struct Modifiers {
  bool shift = false;
  bool control = false;
  bool alt = false;
};

struct Event {
  EventType type = EventType::Close;
  Key key = Key::Unknown;
  Button button = Button::Left;
  Modifiers modifiers;
  // Resize: the framebuffer extent.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Pointer events: the cursor in framebuffer pixels from the top left.
  double x = 0.0;
  double y = 0.0;
  // Scroll: wheel notches, positive away from the user.
  double scroll = 0.0;
};

} // namespace Lotus::viewport

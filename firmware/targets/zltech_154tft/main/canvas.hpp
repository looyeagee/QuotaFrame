#pragma once
#include "usage_panel/canvas.hpp"
namespace usage_panel::zltech {
/**
 * The drawing surface of the 240 x 240 panel.
 *
 * The surface itself is shared; the geometry of this glass is what the target names here.
 */
using Canvas = usage_panel::SizedCanvas<240, 240>;
} // namespace usage_panel::zltech

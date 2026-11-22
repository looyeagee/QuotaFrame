#pragma once
#include "usage_panel/canvas.hpp"
namespace usage_panel::rlcd {
/**
 * The drawing surface of the 400 x 300 panel.
 *
 * The surface itself is shared; the geometry of this glass is what the target names here.
 */
using Canvas = usage_panel::SizedCanvas<400, 300>;
} // namespace usage_panel::rlcd

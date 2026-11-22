#pragma once
#include "usage_panel/canvas.hpp"
namespace usage_panel::amoled {
/**
 * The drawing surface of the 368 x 448 panel.
 *
 * The surface itself is shared; the geometry of this glass is what the target names here.
 */
using Canvas = usage_panel::SizedCanvas<368, 448>;
} // namespace usage_panel::amoled

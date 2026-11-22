#include "beeper.hpp"
namespace usage_panel::amoled {
bool Beeper::begin(const Board &) { return false; }
void Beeper::alert() {}
} // namespace usage_panel::amoled

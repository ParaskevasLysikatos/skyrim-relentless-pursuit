#pragma once

namespace DragonGuard
{
	// When the player uses a load door, excluded creatures (dragons) that are fighting stop combat,
	// so they don't chase the player through it. If one still comes through within 90 seconds,
	// it is moved back to the door it came from.
	void Install();
}

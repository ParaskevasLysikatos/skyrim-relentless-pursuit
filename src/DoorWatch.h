#pragma once

namespace DoorWatch
{
	// When the player uses a load door: logs every enemy fighting the player (name + distance), and
	// makes excluded creatures (dragons) stop combat so they don't follow. For 90 seconds afterwards,
	// an enemy that comes through after the player is logged, and an excluded one is sent back out
	// through the door it came from (if the dragon guard is on).
	void Install();
}

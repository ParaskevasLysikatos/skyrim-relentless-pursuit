#pragma once

namespace DoorWatch
{
	// When the player uses a load door:
	// - logs every enemy fighting the player (name + distance);
	// - queues up to iMaxFollowers of them within fFollowRadius; after the loading screen closes, each
	//   is moved to where the player came out after about distance / 350 s (2-12 s), then attacks;
	// - excluded creatures (dragons) stop combat instead, and if one still comes through within 90 s
	//   it is sent back out through the door it came from (dragon guard).
	void Install();
}

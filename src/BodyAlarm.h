#pragma once

namespace BodyAlarm
{
	// Body alarm: a hostile enemy that is on alert next to the body of an ally you (or a follower / summon of yours)
	// killed, and is within fBodyAlarmRange of you, attacks you after fBodyAlarmDelay seconds even if it never saw you.
	// - remembers the bodies of everyone killed by your side for 10 minutes (TESDeathEvent);
	// - every 0.5 s checks hostile, alerted (on alert or weapon out), not-yet-fighting actors around you;
	// - each enemy raises the alarm once per body and at most once every 30 s;
	// - creatures count too unless bBodyAlarmCreatures = 0; nothing happens while you're invisible
	//   unless bBodyAlarmInvisibilityHides = 0; dragons and the ini's excluded races never raise it.
	void Install();

	// Forgets every body and pending alarm (handles are not valid across loads). Call before a save loads / on a new game.
	void Reset();
}

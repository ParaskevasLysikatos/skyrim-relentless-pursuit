#pragma once

namespace Pursuit
{
	// Looks up the ActorTypeDragon keyword and the extra races from the ini. Call once after data is loaded.
	void ResolveExclusions();

	// Dragons (ActorTypeDragon keyword or a *Dragon* race editor ID) plus any extra races from the ini.
	[[nodiscard]] bool IsExcludedRace(const RE::TESRace* a_race);
	[[nodiscard]] bool IsExcludedActor(RE::Actor* a_actor);

	// Turns on "Disable Combat Boundary" on every encounter zone and clears "No Combat In Water" on
	// every swimming, non-excluded race. Changes are in memory only (no plugin, nothing written to saves),
	// and calling it again is harmless.
	void Apply(std::string_view a_reason);
}

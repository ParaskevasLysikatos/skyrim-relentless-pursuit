#pragma once

namespace Pursuit
{
	// Looks up the ActorTypeDragon keyword, the extra races from the ini and the no-boundary zone from
	// RelentlessPursuit.esp. Call once after data is loaded.
	void ResolveForms();

	// RelentlessPursuit.esp|0x800, or nullptr when the plugin isn't active.
	[[nodiscard]] RE::BGSEncounterZone* GetNoBoundaryZone();

	// Dragons (ActorTypeDragon keyword or a *Dragon* race editor ID) plus any extra races from the ini.
	[[nodiscard]] bool IsExcludedRace(const RE::TESRace* a_race);
	[[nodiscard]] bool IsExcludedActor(RE::Actor* a_actor);

	// Turns on "Disable Combat Boundary" on every encounter zone and clears "No Combat In Water" on
	// every swimming, non-excluded race. Changes are in memory only (no plugin, nothing written to saves),
	// and calling it again is harmless.
	void Apply(std::string_view a_reason);
}

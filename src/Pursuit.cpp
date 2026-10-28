#include "Pursuit.h"

#include "Settings.h"

namespace Pursuit
{
	namespace
	{
		constexpr RE::FormID kActorTypeDragon = 0x00035D59;  // Skyrim.esm
		constexpr RE::FormID kNoBoundaryZone = 0x800;        // RelentlessPursuit.esp
		constexpr auto       kPluginName = "RelentlessPursuit.esp"sv;

		RE::BGSKeyword*                g_dragonKeyword = nullptr;
		RE::BGSEncounterZone*          g_noBoundaryZone = nullptr;
		std::unordered_set<RE::FormID> g_extraExcluded;

		// Catches dragon races that lack the keyword (e.g. dlc2SpectralDragonRace) without
		// catching dragon priests.
		bool EditorIDLooksLikeDragon(const RE::TESRace* a_race)
		{
			const char* edid = a_race->GetFormEditorID();
			if (!edid || !*edid) {
				return false;
			}
			std::string name(edid);
			std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name.find("dragon") != std::string::npos &&
			       name.find("priest") == std::string::npos &&
			       name.find("dragonfly") == std::string::npos &&
			       name.find("dragonborn") == std::string::npos;
		}

		void ApplyCombatBoundary()
		{
			using Flag = RE::ENCOUNTER_ZONE_DATA::Flag;

			std::size_t total = 0;
			std::size_t changed = 0;
			for (auto* zone : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::BGSEncounterZone>()) {
				if (!zone) {
					continue;
				}
				++total;
				if (!zone->data.flags.all(Flag::kDisableCombatBoundary)) {
					zone->data.flags.set(Flag::kDisableCombatBoundary);
					++changed;
				}
			}
			logger::info("Doors: {} of {} encounter zones switched to 'Disable Combat Boundary'", changed, total);
		}

		void ApplyWaterCombat()
		{
			using Flag = RE::RACE_DATA::Flag;

			std::size_t total = 0;
			std::size_t changed = 0;
			std::size_t excluded = 0;
			std::size_t nonSwimmers = 0;
			for (auto* race : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESRace>()) {
				if (!race) {
					continue;
				}
				++total;
				if (IsExcludedRace(race)) {
					++excluded;
					continue;
				}
				if (!race->data.flags.all(Flag::kSwims)) {
					++nonSwimmers;  // no swim animation: they stop at the shore
					continue;
				}
				if (race->data.flags.all(Flag::kNoCombatInWater)) {
					race->data.flags.reset(Flag::kNoCombatInWater);
					++changed;
				}
			}
			logger::info("Water: {} of {} races now fight in water ({} excluded, {} can't swim)", changed, total, excluded, nonSwimmers);
		}
	}

	void ResolveForms()
	{
		auto* dataHandler = RE::TESDataHandler::GetSingleton();

		g_noBoundaryZone = dataHandler->LookupForm<RE::BGSEncounterZone>(kNoBoundaryZone, kPluginName);
		if (g_noBoundaryZone) {
			g_noBoundaryZone->data.flags.set(RE::ENCOUNTER_ZONE_DATA::Flag::kDisableCombatBoundary);
			logger::info("No-boundary zone found: {} [{:08X}]", kPluginName, g_noBoundaryZone->GetFormID());
		} else {
			logger::warn("{} not active (tick it in the plugin list); cells without an encounter zone won't be covered", kPluginName);
		}

		g_dragonKeyword = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeDragon);
		if (!g_dragonKeyword) {
			logger::warn("ActorTypeDragon keyword not found; falling back to race names only");
		}

		g_extraExcluded.clear();
		for (const auto& entry : Settings::Get().extraExcludedRaces) {
			if (auto* race = dataHandler->LookupForm<RE::TESRace>(entry.localID, entry.plugin)) {
				g_extraExcluded.insert(race->GetFormID());
				logger::info("Also excluding race {} [{:08X}]", race->GetFormEditorID(), race->GetFormID());
			} else {
				logger::warn("Excluded race {}|{:06X} not found (plugin missing or wrong FormID)", entry.plugin, entry.localID);
			}
		}

		std::size_t dragons = 0;
		for (auto* race : dataHandler->GetFormArray<RE::TESRace>()) {
			if (race && IsExcludedRace(race)) {
				++dragons;
				logger::info("Excluded: {} [{:08X}]", race->GetFormEditorID(), race->GetFormID());
			}
		}
		logger::info("{} races excluded in total", dragons);
	}

	RE::BGSEncounterZone* GetNoBoundaryZone()
	{
		return g_noBoundaryZone;
	}

	bool IsExcludedRace(const RE::TESRace* a_race)
	{
		if (!a_race) {
			return false;
		}
		if (g_dragonKeyword && a_race->HasKeyword(g_dragonKeyword)) {
			return true;
		}
		if (EditorIDLooksLikeDragon(a_race)) {
			return true;
		}
		return g_extraExcluded.contains(a_race->GetFormID());
	}

	bool IsExcludedActor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		if (g_dragonKeyword && a_actor->HasKeyword(g_dragonKeyword)) {
			return true;
		}
		return IsExcludedRace(a_actor->GetRace());
	}

	void Apply(std::string_view a_reason)
	{
		const auto& config = Settings::Get();
		logger::info("Applying ({})", a_reason);
		if (config.doorPursuit) {
			ApplyCombatBoundary();
		}
		if (config.waterPursuit) {
			ApplyWaterCombat();
		}
	}
}

#include "ZonelessCells.h"

#include "Pursuit.h"

namespace ZonelessCells
{
	namespace
	{
		std::mutex                     g_lock;
		std::unordered_set<RE::FormID> g_logged;  // log each cell once per session

		std::string Describe(RE::TESObjectCELL* a_cell)
		{
			if (const char* edid = a_cell->GetFormEditorID(); edid && *edid) {
				return edid;
			}
			if (const char* name = a_cell->GetFullName(); name && *name) {
				return name;
			}
			return a_cell->IsInteriorCell() ? "interior cell" : "exterior cell";
		}

		void GiveZone(RE::TESObjectCELL* a_cell)
		{
			auto* zone = Pursuit::GetNoBoundaryZone();
			if (!zone || !a_cell) {
				return;
			}
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			if (!loaded || loaded->encounterZone) {
				return;  // not loaded, or it already has a zone (whose boundary is already off)
			}
			loaded->encounterZone = zone;

			std::scoped_lock lock(g_lock);
			if (g_logged.insert(a_cell->GetFormID()).second) {
				logger::info("{} [{:08X}] had no encounter zone: gave it the no-boundary zone", Describe(a_cell), a_cell->GetFormID());
			}
		}

		class CellSink final : public RE::BSTEventSink<RE::BGSActorCellEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
			{
				if (!a_event || a_event->flags.any(RE::BGSActorCellEvent::CellFlag::kLeave)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto cellID = a_event->cellID;
				SKSE::GetTaskInterface()->AddTask([cellID]() {
					GiveZone(RE::TESForm::LookupByID<RE::TESObjectCELL>(cellID));
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static CellSink sink;

		if (!Pursuit::GetNoBoundaryZone()) {
			logger::warn("RelentlessPursuit.esp is not active: enemies won't follow you into cells without an encounter zone");
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			logger::error("Zone-less cells: player not available");
			return;
		}
		player->AsBGSActorCellEventSource()->AddEventSink(&sink);
		logger::info("Zone-less cells: watching the cells you enter");
	}

	void ApplyToPlayerCell()
	{
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			GiveZone(player->GetParentCell());
		}
	}
}

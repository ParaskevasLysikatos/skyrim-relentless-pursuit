#include "DoorWatch.h"
#include "Pursuit.h"
#include "Settings.h"
#include "ZonelessCells.h"

namespace
{
	bool ZonelessCellsOn()
	{
		const auto& config = Settings::Get();
		return config.doorPursuit && config.noZoneCells;
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Pursuit::ResolveForms();
			Pursuit::Apply("data loaded");
			DoorWatch::Install();
			if (ZonelessCellsOn()) {
				ZonelessCells::Install();
			}
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Pursuit::Apply("save loaded");  // in case a save restored any zone flags
			if (ZonelessCellsOn()) {
				ZonelessCells::ApplyToPlayerCell();
			}
			break;
		case SKSE::MessagingInterface::kNewGame:
			Pursuit::Apply("new game");
			if (ZonelessCellsOn()) {
				ZonelessCells::ApplyToPlayerCell();
			}
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	logger::info("Relentless Pursuit loaded");

	Settings::Load();

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		logger::critical("Could not register for SKSE messages");
		return false;
	}
	return true;
}

#include "DragonGuard.h"
#include "Pursuit.h"
#include "Settings.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Pursuit::ResolveExclusions();
			Pursuit::Apply("data loaded");
			if (Settings::Get().dragonGuard) {
				DragonGuard::Install();
			}
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Pursuit::Apply("save loaded");  // in case a save restored any zone flags
			break;
		case SKSE::MessagingInterface::kNewGame:
			Pursuit::Apply("new game");
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

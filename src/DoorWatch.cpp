#include "DoorWatch.h"

#include "Pursuit.h"
#include "Settings.h"

namespace DoorWatch
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto        kWatchTime = std::chrono::seconds(90);
		constexpr std::size_t kMaxListed = 12;

		struct Watched
		{
			RE::ActorHandle     actor;
			RE::ObjectRefHandle door;       // the door the player used, on the enemy's side
			RE::TESObjectCELL*  fromCell;   // only compared, never dereferenced
			RE::TESWorldSpace*  fromWorld;  // only compared, never dereferenced
			Clock::time_point   used;
			bool                excluded;
		};

		std::mutex           g_lock;
		std::vector<Watched> g_watched;

		bool IsFightingPlayer(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (!a_actor || a_actor == a_player || a_actor->IsDead() || a_actor->IsDisabled() || !a_actor->IsInCombat()) {
				return false;
			}
			const auto target = a_actor->GetActorRuntimeData().currentCombatTarget.get();
			return target.get() == a_player || a_actor->IsHostileToActor(a_player);
		}

		class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				if (!a_event || !a_event->actionRef || !a_event->objectActivated) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player || a_event->actionRef.get() != player) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* door = a_event->objectActivated.get();
				if (!door->extraList.GetByType<RE::ExtraTeleport>() || door->IsLocked()) {
					return RE::BSEventNotifyControl::kContinue;  // not a load door, or it won't open
				}

				const bool  guardOn = Settings::Get().dragonGuard;
				const auto  doorHandle = door->GetHandle();
				const auto  now = Clock::now();
				const auto  playerPos = player->GetPosition();
				std::string listed;
				std::size_t count = 0;
				std::vector<RE::ActorHandle> toStop;

				{
					std::scoped_lock lock(g_lock);
					std::erase_if(g_watched, [&](const Watched& w) { return now - w.used > kWatchTime; });

					RE::ProcessLists::GetSingleton()->ForAllActors([&](RE::Actor* a_actor) {
						if (!IsFightingPlayer(a_actor, player)) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						const auto handle = a_actor->GetHandle();
						const bool excluded = Pursuit::IsExcludedActor(a_actor);

						std::erase_if(g_watched, [&](const Watched& w) { return w.actor == handle; });
						g_watched.push_back({ handle, doorHandle, a_actor->GetParentCell(), a_actor->GetWorldspace(), now, excluded });
						if (excluded && guardOn) {
							toStop.push_back(handle);
						}

						if (++count <= kMaxListed) {
							listed += std::format("{}{} ({:.0f} units{})", count > 1 ? ", " : "", a_actor->GetName(),
								a_actor->GetPosition().GetDistance(playerPos), excluded ? ", excluded" : "");
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}

				if (count > 0) {
					logger::info("Door used, {} enemies fighting you: {}{}", count, listed, count > kMaxListed ? ", ..." : "");
				} else if (player->IsInCombat()) {
					logger::info("Door used while in combat, but no enemy has you as target");
				}

				if (!toStop.empty()) {
					SKSE::GetTaskInterface()->AddTask([toStop]() {
						for (const auto& handle : toStop) {
							if (auto actor = handle.get()) {
								actor->StopCombat();
								logger::info("{} [{:08X}] stops chasing (excluded)", actor->GetName(), actor->GetFormID());
							}
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class MoveAttachSink final : public RE::BSTEventSink<RE::TESMoveAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESMoveAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESMoveAttachDetachEvent>*) override
			{
				if (!a_event || !a_event->isCellAttached || !a_event->movedRef) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = a_event->movedRef->As<RE::Actor>();
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!actor || !player || actor == player) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* cell = actor->GetParentCell();
				if (!cell || cell != player->GetParentCell()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				Watched entry{};
				{
					std::scoped_lock lock(g_lock);
					if (g_watched.empty()) {
						return RE::BSEventNotifyControl::kContinue;
					}
					const auto handle = actor->GetHandle();
					const auto now = Clock::now();
					const auto it = std::ranges::find_if(g_watched, [&](const Watched& w) {
						return w.actor == handle && now - w.used <= kWatchTime;
					});
					if (it == g_watched.end()) {
						return RE::BSEventNotifyControl::kContinue;
					}
					const bool followed = cell != it->fromCell &&
					                      (cell->IsInteriorCell() || actor->GetWorldspace() != it->fromWorld);
					if (!followed) {
						return RE::BSEventNotifyControl::kContinue;
					}
					entry = *it;
					g_watched.erase(it);
				}

				const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - entry.used).count();
				if (!entry.excluded || !Settings::Get().dragonGuard) {
					logger::info("{} [{:08X}] followed you through the door ({} s after you)", actor->GetName(), actor->GetFormID(), seconds);
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto actorHandle = actor->GetHandle();
				const auto door = entry.door;
				SKSE::GetTaskInterface()->AddTask([actorHandle, door]() {
					auto dragon = actorHandle.get();
					auto target = door.get();
					if (!dragon || !target) {
						return;
					}
					dragon->StopCombat();
					dragon->MoveTo(target.get());
					logger::info("{} [{:08X}] followed you through a door and was sent back outside", dragon->GetName(), dragon->GetFormID());
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static ActivateSink   activateSink;
		static MoveAttachSink moveAttachSink;

		auto* events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logger::error("Door watch: event source not available");
			return;
		}
		events->AddEventSink<RE::TESActivateEvent>(&activateSink);
		events->AddEventSink<RE::TESMoveAttachDetachEvent>(&moveAttachSink);
		logger::info("Door watch installed (dragon guard {})", Settings::Get().dragonGuard ? "on" : "off");
	}
}

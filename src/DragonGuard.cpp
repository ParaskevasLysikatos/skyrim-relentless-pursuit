#include "DragonGuard.h"

#include "Pursuit.h"

namespace DragonGuard
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto kWatchTime = std::chrono::seconds(90);

		struct Watched
		{
			RE::ActorHandle     actor;
			RE::ObjectRefHandle door;       // the door the player used, on the dragon's side
			RE::TESObjectCELL*  fromCell;   // only compared, never dereferenced
			RE::TESWorldSpace*  fromWorld;  // only compared, never dereferenced
			Clock::time_point   until;
		};

		std::mutex           g_lock;
		std::vector<Watched> g_watched;

		bool IsFightingNearPlayer(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (!a_actor || a_actor == a_player || a_actor->IsDead() || a_actor->IsDisabled()) {
				return false;
			}
			if (!a_actor->IsInCombat() || !Pursuit::IsExcludedActor(a_actor)) {
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

				std::vector<RE::ActorHandle> stopped;
				const auto doorHandle = door->GetHandle();
				const auto until = Clock::now() + kWatchTime;

				{
					std::scoped_lock lock(g_lock);
					const auto now = Clock::now();
					std::erase_if(g_watched, [&](const Watched& w) { return w.until < now; });

					RE::ProcessLists::GetSingleton()->ForAllActors([&](RE::Actor* a_actor) {
						if (IsFightingNearPlayer(a_actor, player)) {
							const auto handle = a_actor->GetHandle();
							stopped.push_back(handle);
							g_watched.push_back({ handle, doorHandle, a_actor->GetParentCell(), a_actor->GetWorldspace(), until });
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}

				if (!stopped.empty()) {
					SKSE::GetTaskInterface()->AddTask([stopped]() {
						for (const auto& handle : stopped) {
							if (auto actor = handle.get()) {
								actor->StopCombat();
								logger::info("Door used: {} [{:08X}] stops chasing", actor->GetName(), actor->GetFormID());
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
				if (!actor || !Pursuit::IsExcludedActor(actor)) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* cell = actor->GetParentCell();
				if (!player || !cell || cell != player->GetParentCell()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				RE::ObjectRefHandle door;
				{
					std::scoped_lock lock(g_lock);
					const auto handle = actor->GetHandle();
					const auto now = Clock::now();
					const auto it = std::ranges::find_if(g_watched, [&](const Watched& w) {
						return w.actor == handle && w.until >= now;
					});
					if (it == g_watched.end()) {
						return RE::BSEventNotifyControl::kContinue;
					}
					const bool followed = cell != it->fromCell &&
					                      (cell->IsInteriorCell() || actor->GetWorldspace() != it->fromWorld);
					if (!followed) {
						return RE::BSEventNotifyControl::kContinue;
					}
					door = it->door;
					g_watched.erase(it);
				}

				const auto actorHandle = actor->GetHandle();
				SKSE::GetTaskInterface()->AddTask([actorHandle, door]() {
					auto dragon = actorHandle.get();
					auto target = door.get();
					if (!dragon || !target) {
						return;
					}
					dragon->StopCombat();
					dragon->MoveTo(target.get());
					logger::info("{} [{:08X}] followed through a door and was sent back outside", dragon->GetName(), dragon->GetFormID());
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
			logger::error("Dragon guard: event source not available, guard disabled");
			return;
		}
		events->AddEventSink<RE::TESActivateEvent>(&activateSink);
		events->AddEventSink<RE::TESMoveAttachDetachEvent>(&moveAttachSink);
		logger::info("Dragon guard installed");
	}
}

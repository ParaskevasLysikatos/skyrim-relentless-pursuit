#include "DoorWatch.h"

#include "Pursuit.h"
#include "Settings.h"

namespace DoorWatch
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto        kWatchTime = std::chrono::seconds(90);
		constexpr auto        kBatchTimeout = std::chrono::seconds(120);  // door used, loading screen never came
		constexpr std::size_t kMaxListed = 12;
		constexpr float       kRunSpeed = 350.0f;  // units per second, for the arrival delay
		constexpr float       kMinDelay = 2.0f;
		constexpr float       kMaxDelay = 12.0f;
		constexpr float       kStagger = 0.3f;     // seconds between two arrivals at the same moment

		struct Watched
		{
			RE::ActorHandle     actor;
			RE::ObjectRefHandle door;       // the door the player used, on the enemy's side
			RE::TESObjectCELL*  fromCell;   // only compared, never dereferenced
			RE::TESWorldSpace*  fromWorld;  // only compared, never dereferenced
			Clock::time_point   used;
			bool                excluded;
		};

		struct Follower
		{
			RE::ActorHandle           actor;
			std::chrono::milliseconds delay;
		};

		// Enemies that will come through after the player once the loading screen closes.
		struct Batch
		{
			RE::ObjectRefHandle   destDoor;  // the door the player comes out of
			RE::NiPoint3          destPos;   // where the player arrives
			RE::NiPoint3          destRot;
			Clock::time_point     used;
			std::vector<Follower> followers;
		};

		std::mutex             g_lock;
		std::vector<Watched>   g_watched;
		std::optional<Batch>   g_batch;

		bool IsFightingPlayer(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (!a_actor || a_actor == a_player || a_actor->IsDead() || a_actor->IsDisabled() || !a_actor->IsInCombat()) {
				return false;
			}
			const auto target = a_actor->GetActorRuntimeData().currentCombatTarget.get();
			return target.get() == a_player || a_actor->IsHostileToActor(a_player);
		}

		// Empty string = may follow; otherwise the reason it won't.
		std::string_view FollowBlocker(RE::Actor* a_actor, float a_distance, const Settings::Config& a_config)
		{
			if (a_distance > a_config.followRadius) {
				return "too far"sv;
			}
			if (const auto* race = a_actor->GetRace(); race && race->data.flags.all(RE::RACE_DATA::Flag::kImmobile)) {
				return "immobile"sv;
			}
			if (a_actor->IsOnMount()) {
				return "mounted"sv;
			}
			if (a_actor->AsActorState()->IsBleedingOut()) {
				return "bleeding out"sv;
			}
			if (a_config.skipQuestActors && a_actor->extraList.HasType(RE::ExtraDataType::kAliasInstanceArray)) {
				return "quest"sv;
			}
			return {};
		}

		// The engine's MoveTo (what Papyrus ObjectReference.MoveTo uses), with an explicit position.
		// CommonLib keeps its wrapper private, so call it through the same Address Library IDs.
		void MoveRefTo(RE::TESObjectREFR* a_ref, const RE::ObjectRefHandle& a_target, RE::TESObjectCELL* a_cell, RE::TESWorldSpace* a_world, const RE::NiPoint3& a_pos, const RE::NiPoint3& a_rot)
		{
			using func_t = void(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
			static REL::Relocation<func_t*> func{ REL::RelocationID(56227, 56626) };
			func(a_ref, a_target, a_cell, a_world, a_pos, a_rot);
		}

		bool PlayerIsOnThatSide(RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_destDoor, const RE::NiPoint3& a_destPos)
		{
			if (auto* world = a_destDoor->GetWorldspace()) {
				return a_player->GetWorldspace() == world && a_player->GetPosition().GetDistance(a_destPos) < 8192.0f;
			}
			return a_player->GetParentCell() == a_destDoor->GetParentCell();
		}

		// Same cell, or (outdoors) same worldspace and close by.
		bool NearPlayer(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (a_actor->GetParentCell() == a_player->GetParentCell()) {
				return true;
			}
			auto* world = a_player->GetWorldspace();
			return world && a_actor->GetWorldspace() == world && a_actor->GetPosition().GetDistance(a_player->GetPosition()) < 4096.0f;
		}

		// Main thread only.
		void BringThrough(RE::ActorHandle a_actor, RE::ObjectRefHandle a_destDoor, RE::NiPoint3 a_pos, RE::NiPoint3 a_rot, Clock::time_point a_used)
		{
			auto  actor = a_actor.get();
			auto  door = a_destDoor.get();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!actor || !door || !player || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			if (!PlayerIsOnThatSide(player, door.get(), a_pos)) {
				logger::info("{} [{:08X}] stays behind: you already left that area", actor->GetName(), actor->GetFormID());
				return;
			}
			if (NearPlayer(actor.get(), player)) {
				return;  // came through on its own
			}

			{
				std::scoped_lock lock(g_lock);
				std::erase_if(g_watched, [&](const Watched& w) { return w.actor == a_actor; });  // no "followed" double log
			}

			MoveRefTo(actor.get(), door->GetHandle(), door->GetParentCell(), door->GetWorldspace(), a_pos, a_rot);
			const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - a_used).count();
			logger::info("{} [{:08X}] came through the door after you ({} s after you used it)", actor->GetName(), actor->GetFormID(), seconds);
		}

		// Main thread only. Followers that arrived and calmed down on the way pick the fight up again.
		void ResumeCombat(const std::vector<RE::ActorHandle>& a_actors)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			for (const auto& handle : a_actors) {
				auto actor = handle.get();
				if (!actor || actor->IsDead() || actor->IsInCombat() || !actor->Is3DLoaded() ||
					!NearPlayer(actor.get(), player) || !actor->GetActorRuntimeData().currentProcess) {
					continue;
				}
				actor->StartCombat(player);
			}
		}

		void RunBatch(Batch a_batch)
		{
			std::thread([batch = std::move(a_batch)]() {
				const auto start = Clock::now();
				std::vector<RE::ActorHandle> handles;
				for (const auto& follower : batch.followers) {
					std::this_thread::sleep_until(start + follower.delay);
					handles.push_back(follower.actor);
					SKSE::GetTaskInterface()->AddTask([actor = follower.actor, door = batch.destDoor, pos = batch.destPos, rot = batch.destRot, used = batch.used]() {
						BringThrough(actor, door, pos, rot, used);
					});
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1500));
				SKSE::GetTaskInterface()->AddTask([handles]() { ResumeCombat(handles); });
			}).detach();
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
				auto* teleport = door->extraList.GetByType<RE::ExtraTeleport>();
				if (!teleport || door->IsLocked()) {
					return RE::BSEventNotifyControl::kContinue;  // not a load door, or it won't open
				}

				const auto& config = Settings::Get();
				const bool  guardOn = config.dragonGuard;
				const auto  doorHandle = door->GetHandle();
				const auto  now = Clock::now();
				const auto  playerPos = player->GetPosition();

				std::string listed;
				std::size_t count = 0;
				std::vector<RE::ActorHandle> toStop;

				struct Candidate
				{
					RE::ActorHandle actor;
					float           distance;
					std::string     name;
				};
				std::vector<Candidate> candidates;
				std::size_t skippedFar = 0, skippedQuest = 0, skippedOther = 0;

				{
					std::scoped_lock lock(g_lock);
					std::erase_if(g_watched, [&](const Watched& w) { return now - w.used > kWatchTime; });

					RE::ProcessLists::GetSingleton()->ForAllActors([&](RE::Actor* a_actor) {
						if (!IsFightingPlayer(a_actor, player)) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						const auto  handle = a_actor->GetHandle();
						const bool  excluded = Pursuit::IsExcludedActor(a_actor);
						const float distance = a_actor->GetPosition().GetDistance(playerPos);

						std::erase_if(g_watched, [&](const Watched& w) { return w.actor == handle; });
						g_watched.push_back({ handle, doorHandle, a_actor->GetParentCell(), a_actor->GetWorldspace(), now, excluded });

						if (excluded) {
							if (guardOn) {
								toStop.push_back(handle);
							}
						} else if (config.teleportFollow) {
							const auto blocker = FollowBlocker(a_actor, distance, config);
							if (blocker.empty()) {
								candidates.push_back({ handle, distance, a_actor->GetName() });
							} else if (blocker == "too far"sv) {
								++skippedFar;
							} else if (blocker == "quest"sv) {
								++skippedQuest;
							} else {
								++skippedOther;
							}
						}

						if (++count <= kMaxListed) {
							listed += std::format("{}{} ({:.0f} units{})", count > 1 ? ", " : "", a_actor->GetName(), distance, excluded ? ", excluded" : "");
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}

				if (count > 0) {
					logger::info("Door used, {} enemies fighting you: {}{}", count, listed, count > kMaxListed ? ", ..." : "");
				} else if (player->IsInCombat()) {
					logger::info("Door used while in combat, but no enemy has you as target");
				}

				// Queue the followers; they start coming once the loading screen closes.
				const auto* data = teleport->teleportData;
				if (config.teleportFollow && data && data->linkedDoor && !candidates.empty()) {
					std::ranges::sort(candidates, {}, &Candidate::distance);
					const auto maxFollowers = static_cast<std::size_t>(std::max(config.maxFollowers, 0));
					const auto overLimit = candidates.size() > maxFollowers ? candidates.size() - maxFollowers : 0;
					candidates.resize(std::min(candidates.size(), maxFollowers));

					Batch       batch{ data->linkedDoor, data->position, data->rotation, now, {} };
					std::string plan;
					for (std::size_t i = 0; i < candidates.size(); ++i) {
						const float seconds = std::clamp(candidates[i].distance / kRunSpeed, kMinDelay, kMaxDelay) + kStagger * static_cast<float>(i);
						batch.followers.push_back({ candidates[i].actor, std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0f)) });
						plan += std::format("{}{} (~{:.0f} s)", i > 0 ? ", " : "", candidates[i].name, seconds);
					}
					std::ranges::sort(batch.followers, {}, &Follower::delay);

					logger::info("Coming through after you: {}{}", plan,
						overLimit + skippedFar + skippedQuest + skippedOther > 0 ?
							std::format(" (not coming: {} over the limit, {} too far, {} quest, {} mounted/immobile/bleeding out)", overLimit, skippedFar, skippedQuest, skippedOther) :
							std::string());

					std::scoped_lock lock(g_lock);
					g_batch = std::move(batch);
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

		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->opening || a_event->menuName != RE::LoadingMenu::MENU_NAME) {
					return RE::BSEventNotifyControl::kContinue;
				}

				std::optional<Batch> batch;
				{
					std::scoped_lock lock(g_lock);
					batch.swap(g_batch);
				}
				if (batch && Clock::now() - batch->used <= kBatchTimeout) {
					RunBatch(std::move(*batch));
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
					logger::info("{} [{:08X}] followed you through the door on its own ({} s after you)", actor->GetName(), actor->GetFormID(), seconds);
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
		static MenuSink       menuSink;

		auto* events = RE::ScriptEventSourceHolder::GetSingleton();
		auto* ui = RE::UI::GetSingleton();
		if (!events || !ui) {
			logger::error("Door watch: event sources not available");
			return;
		}
		events->AddEventSink<RE::TESActivateEvent>(&activateSink);
		events->AddEventSink<RE::TESMoveAttachDetachEvent>(&moveAttachSink);
		ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuSink);

		const auto& config = Settings::Get();
		logger::info("Door watch installed (follow-through {}: radius {:.0f}, max {}, quest enemies {}; dragon guard {})",
			config.teleportFollow ? "on" : "off", config.followRadius, config.maxFollowers,
			config.skipQuestActors ? "skipped" : "follow too", config.dragonGuard ? "on" : "off");
	}
}

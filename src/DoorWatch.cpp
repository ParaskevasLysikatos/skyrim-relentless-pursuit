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
		constexpr auto        kTick = std::chrono::milliseconds(500);      // how often the ones that came through are checked
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
			RE::TESObjectCELL*        fromCell;   // where it was when you used the door; only compared
			RE::TESWorldSpace*        fromWorld;  // only compared
			bool                      alerted;    // on alert, but not fighting you yet
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

		// One enemy that came through, kept in the fight for fPursuitTime seconds. Main thread only.
		struct Arrival
		{
			RE::ActorHandle    actor;
			std::string        name;
			RE::FormID         formID{ 0 };
			RE::TESObjectCELL* fromCell{ nullptr };   // only compared
			RE::TESWorldSpace* fromWorld{ nullptr };  // only compared
			Clock::time_point  arrived;
			int                returns{ 0 };          // times it was brought out again after walking back
			bool               restarted{ false };    // "attacks you again" already logged
			std::string        outcome;               // empty while still watched
		};

		// Everything that came through one door. The worker thread only reads the atomics.
		struct Chase
		{
			RE::ObjectRefHandle      destDoor;
			RE::NiPoint3             destPos;
			RE::NiPoint3             destRot;
			Clock::time_point        used;
			std::vector<Arrival>     arrivals;  // main thread only
			std::atomic<std::size_t> pending{ 0 };
			std::atomic_bool         over{ false };
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

		// Hostile to the player and on alert (heard the fight, weapon out), but not in combat yet.
		// Enemies searching for you after losing sight of you are still in combat, so they count as fighting.
		bool IsAlertedEnemy(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (!a_actor || a_actor == a_player || a_actor->IsDead() || a_actor->IsDisabled() || a_actor->IsInCombat() ||
				a_actor->IsPlayerTeammate() || !a_actor->IsHostileToActor(a_player)) {
				return false;
			}
			const auto* process = a_actor->GetActorRuntimeData().currentProcess;
			if (process && process->lowProcessFlags.all(RE::AIProcess::LowProcessFlags::kAlert)) {
				return true;
			}
			const auto* state = a_actor->AsActorState();
			return state && state->IsWeaponDrawn();
		}

		// Same interior cell, or (outdoors) same worldspace.
		bool SameArea(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			auto* cell = a_player->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				return a_actor->GetParentCell() == cell;
			}
			auto* world = a_player->GetWorldspace();
			return world && a_actor->GetWorldspace() == world;
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

		// Back on the side of the door it came from: it walked back through the door.
		bool WentBack(RE::Actor* a_actor, RE::PlayerCharacter* a_player, const Arrival& a_arrival)
		{
			if (a_arrival.fromCell && a_actor->GetParentCell() == a_arrival.fromCell) {
				return true;
			}
			auto* world = a_actor->GetWorldspace();
			return a_arrival.fromWorld && world == a_arrival.fromWorld && a_player->GetWorldspace() != world;
		}

		// Main thread only. Enemies stop fighting once you leave their cell, so the ones that come through
		// arrive calm and their normal AI walks them straight back through the door. Start the fight again
		// as soon as the actor is fully loaded on this side. Returns true if combat was started.
		bool KeepFighting(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (a_actor->IsInCombat() || !a_actor->Is3DLoaded()) {
				return false;
			}
			const auto* process = a_actor->GetActorRuntimeData().currentProcess;
			if (!process || !process->InHighProcess()) {
				return false;
			}
			a_actor->StartCombat(a_player);
			return true;
		}

		// Main thread only.
		void BringThrough(Chase& a_chase, const Follower& a_follower)
		{
			auto  actor = a_follower.actor.get();
			auto  door = a_chase.destDoor.get();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!actor || !door || !player || actor->IsDead() || actor->IsDisabled()) {
				return;
			}
			if (!PlayerIsOnThatSide(player, door.get(), a_chase.destPos)) {
				logger::info("{} [{:08X}] stays behind: you already left that area", actor->GetName(), actor->GetFormID());
				return;
			}

			Arrival arrival;
			arrival.actor = a_follower.actor;
			arrival.name = actor->GetName();
			arrival.formID = actor->GetFormID();
			arrival.fromCell = a_follower.fromCell;
			arrival.fromWorld = a_follower.fromWorld;
			arrival.arrived = Clock::now();

			if (!NearPlayer(actor.get(), player)) {
				{
					std::scoped_lock lock(g_lock);
					std::erase_if(g_watched, [&](const Watched& w) { return w.actor == a_follower.actor; });  // no "followed" double log
				}
				MoveRefTo(actor.get(), door->GetHandle(), door->GetParentCell(), door->GetWorldspace(), a_chase.destPos, a_chase.destRot);
				const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - a_chase.used).count();
				logger::info("{} [{:08X}] came through the door after you ({} s after you used it{})", arrival.name, arrival.formID, seconds,
					a_follower.alerted ? ", was alerted" : "");
			}
			// Otherwise it came through on its own; watch it all the same so it keeps fighting.

			KeepFighting(actor.get(), player);  // usually not loaded yet; the checks below retry every 0.5 s
			a_chase.arrivals.push_back(std::move(arrival));
		}

		// Main thread only. For fPursuitTime seconds after coming through, every arrival is kept in the fight
		// and brought out again (iMaxReturns times) if it walks back through the door.
		void Tick(Chase& a_chase)
		{
			const auto& config = Settings::Get();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto  door = a_chase.destDoor.get();
			const auto now = Clock::now();
			const auto watchFor = std::chrono::milliseconds(static_cast<long long>(config.pursuitTime * 1000.0f));
			const bool playerThere = player && door && PlayerIsOnThatSide(player, door.get(), a_chase.destPos);

			bool allDone = true;
			for (auto& a : a_chase.arrivals) {
				if (!a.outcome.empty()) {
					continue;
				}
				const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - a.arrived).count();
				auto       actor = a.actor.get();
				if (!actor) {
					a.outcome = "gone";
				} else if (actor->IsDead()) {
					a.outcome = std::format("killed {} s after coming through", seconds);
				} else if (actor->IsDisabled()) {
					a.outcome = "disabled";
				} else if (!playerThere) {
					a.outcome = "you left the area";
				} else if (now - a.arrived >= watchFor) {
					a.outcome = !NearPlayer(actor.get(), player) ? "away from you" : actor->IsInCombat() ? "still fighting you" : "near you, not fighting";
				} else if (!NearPlayer(actor.get(), player)) {
					if (WentBack(actor.get(), player, a)) {
						if (a.returns < config.maxReturns) {
							++a.returns;
							MoveRefTo(actor.get(), door->GetHandle(), door->GetParentCell(), door->GetWorldspace(), a_chase.destPos, a_chase.destRot);
							logger::info("{} [{:08X}] went back through the door: brought out again ({} of {}, {} s after coming through)",
								a.name, a.formID, a.returns, config.maxReturns, seconds);
						} else {
							a.outcome = std::format("went back through the door (brought out again {} times)", a.returns);
							logger::info("{} [{:08X}] went back through the door again: left there", a.name, a.formID);
						}
					}
					// Otherwise it's just away from you (chasing you, or you ran); leave it to its own AI.
				} else if (KeepFighting(actor.get(), player) && !a.restarted) {
					a.restarted = true;
					logger::info("{} [{:08X}] wasn't fighting after coming through: attacks you again ({} s after coming through)", a.name, a.formID, seconds);
				}

				if (a.outcome.empty()) {
					allDone = false;
				}
			}

			if (allDone && a_chase.pending == 0) {
				a_chase.over = true;
			}
		}

		// Main thread only.
		void Finish(Chase& a_chase)
		{
			std::string summary;
			for (auto& a : a_chase.arrivals) {
				if (a.outcome.empty()) {
					a.outcome = "watch ended";
				}
				summary += std::format("{}{} ({})", summary.empty() ? "" : ", ", a.name, a.outcome);
			}
			if (!summary.empty()) {
				logger::info("Pursuit over: {}", summary);
			}
		}

		void RunBatch(Batch a_batch)
		{
			auto chase = std::make_shared<Chase>();
			chase->destDoor = a_batch.destDoor;
			chase->destPos = a_batch.destPos;
			chase->destRot = a_batch.destRot;
			chase->used = a_batch.used;
			chase->pending = a_batch.followers.size();

			const auto watchFor = std::chrono::milliseconds(static_cast<long long>(Settings::Get().pursuitTime * 1000.0f));

			std::thread([chase, followers = std::move(a_batch.followers), watchFor]() {
				auto*      tasks = SKSE::GetTaskInterface();
				const auto start = Clock::now();
				const auto lastDelay = followers.empty() ? std::chrono::milliseconds(0) : followers.back().delay;
				const auto end = start + lastDelay + watchFor + std::chrono::seconds(1);

				std::size_t next = 0;
				while (!chase->over && Clock::now() < end) {
					const auto now = Clock::now();
					for (; next < followers.size() && now >= start + followers[next].delay; ++next) {
						tasks->AddTask([chase, follower = followers[next]]() {
							BringThrough(*chase, follower);
							--chase->pending;
						});
					}
					if (next > 0) {
						tasks->AddTask([chase]() { Tick(*chase); });
					}
					auto wake = now + kTick;
					if (next < followers.size()) {
						wake = (std::min)(wake, start + followers[next].delay);
					}
					std::this_thread::sleep_until(wake);
				}
				tasks->AddTask([chase]() { Finish(*chase); });
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
				const bool  alertedOn = config.teleportFollow && config.alertedFollow;
				const auto  doorHandle = door->GetHandle();
				const auto  now = Clock::now();
				const auto  playerPos = player->GetPosition();

				std::string listed;
				std::string alertedListed;
				std::size_t count = 0;
				std::size_t alertedCount = 0;
				std::vector<RE::ActorHandle> toStop;

				struct Candidate
				{
					RE::ActorHandle    actor;
					float              distance;
					std::string        name;
					RE::TESObjectCELL* cell;
					RE::TESWorldSpace* world;
					bool               alerted;
				};
				std::vector<Candidate> candidates;
				std::size_t skippedFar = 0, skippedQuest = 0, skippedOther = 0;

				{
					std::scoped_lock lock(g_lock);
					std::erase_if(g_watched, [&](const Watched& w) { return now - w.used > kWatchTime; });

					RE::ProcessLists::GetSingleton()->ForAllActors([&](RE::Actor* a_actor) {
						if (!IsFightingPlayer(a_actor, player)) {
							// Not fighting you: maybe on alert (heard the fight) and hostile.
							if (alertedOn && IsAlertedEnemy(a_actor, player) && SameArea(a_actor, player) && !Pursuit::IsExcludedActor(a_actor)) {
								const float distance = a_actor->GetPosition().GetDistance(playerPos);
								const auto  blocker = FollowBlocker(a_actor, distance, config);
								if (blocker.empty()) {
									candidates.push_back({ a_actor->GetHandle(), distance, a_actor->GetName(), a_actor->GetParentCell(), a_actor->GetWorldspace(), true });
								}
								if (++alertedCount <= kMaxListed) {
									alertedListed += std::format("{}{} ({:.0f} units{})", alertedCount > 1 ? ", " : "", a_actor->GetName(), distance,
										blocker.empty() ? "" : std::format(", {}", blocker));
								}
							}
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
								candidates.push_back({ handle, distance, a_actor->GetName(), a_actor->GetParentCell(), a_actor->GetWorldspace(), false });
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
				if (alertedCount > 0) {
					logger::info("Door used, {} enemies alerted (not fighting you yet): {}{}", alertedCount, alertedListed, alertedCount > kMaxListed ? ", ..." : "");
				}

				// Queue the followers; they start coming once the loading screen closes.
				// The ones fighting you come first, then the alerted ones; closest first in each group.
				const auto* data = teleport->teleportData;
				if (config.teleportFollow && data && data->linkedDoor && !candidates.empty()) {
					std::ranges::sort(candidates, [](const Candidate& a_lhs, const Candidate& a_rhs) {
						return std::tie(a_lhs.alerted, a_lhs.distance) < std::tie(a_rhs.alerted, a_rhs.distance);
					});
					const auto maxFollowers = static_cast<std::size_t>((std::max)(config.maxFollowers, 0));
					const auto overLimit = candidates.size() > maxFollowers ? candidates.size() - maxFollowers : 0;
					candidates.resize((std::min)(candidates.size(), maxFollowers));

					Batch       batch{ data->linkedDoor, data->position, data->rotation, now, {} };
					std::string plan;
					for (std::size_t i = 0; i < candidates.size(); ++i) {
						const auto& c = candidates[i];
						const float seconds = std::clamp(c.distance / kRunSpeed, kMinDelay, kMaxDelay) + kStagger * static_cast<float>(i);
						batch.followers.push_back({ c.actor, std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0f)), c.cell, c.world, c.alerted });
						plan += std::format("{}{} (~{:.0f} s{})", i > 0 ? ", " : "", c.name, seconds, c.alerted ? ", alerted" : "");
					}
					std::ranges::sort(batch.followers, {}, &Follower::delay);

					if (!batch.followers.empty()) {
						logger::info("Coming through after you: {}{}", plan,
							overLimit + skippedFar + skippedQuest + skippedOther > 0 ?
								std::format(" (not coming: {} over the limit, {} too far, {} quest, {} mounted/immobile/bleeding out)", overLimit, skippedFar, skippedQuest, skippedOther) :
								std::string());

						std::scoped_lock lock(g_lock);
						g_batch = std::move(batch);
					}
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
		logger::info("Door watch installed (follow-through {}: radius {:.0f}, max {}, alerted enemies {}, quest enemies {}, kept fighting for {:.0f} s, brought out again up to {} times; dragon guard {})",
			config.teleportFollow ? "on" : "off", config.followRadius, config.maxFollowers,
			config.alertedFollow ? "too" : "no", config.skipQuestActors ? "skipped" : "follow too",
			config.pursuitTime, config.maxReturns, config.dragonGuard ? "on" : "off");
	}
}

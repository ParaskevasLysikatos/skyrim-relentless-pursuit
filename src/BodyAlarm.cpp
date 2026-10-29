#include "BodyAlarm.h"

#include "Pursuit.h"
#include "Settings.h"

namespace BodyAlarm
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr auto        kTick = std::chrono::milliseconds(500);
		constexpr auto        kBodyMemory = std::chrono::minutes(10);  // bodies older than this are forgotten
		constexpr auto        kCooldown = std::chrono::seconds(30);    // per enemy, between two alarms
		constexpr std::size_t kMaxBodies = 64;

		struct Body
		{
			RE::ActorHandle   actor;
			std::string       name;
			RE::FormID        formID{ 0 };
			Clock::time_point died;
		};

		struct Pending
		{
			RE::ActorHandle   finder;
			std::string       name;
			RE::FormID        formID{ 0 };
			Clock::time_point due;
		};

		std::mutex        g_bodyLock;  // the death sink may run off the main thread
		std::vector<Body> g_bodies;

		// Main thread only (the tick and Reset run there).
		std::vector<Pending>                              g_pending;
		std::unordered_map<RE::FormID, Clock::time_point> g_lastAlarm;  // finder -> its last alarm
		std::unordered_set<std::uint64_t>                 g_fired;      // finder << 32 | body: already used

		std::uint64_t PairKey(RE::FormID a_finder, RE::FormID a_body)
		{
			return (static_cast<std::uint64_t>(a_finder) << 32) | a_body;
		}

		// You, a follower, or something one of you summoned or raised.
		bool IsPlayerSide(RE::TESObjectREFR* a_killer)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!a_killer || !player) {
				return false;
			}
			if (a_killer == player) {
				return true;
			}
			auto* actor = a_killer->As<RE::Actor>();
			if (!actor) {
				return false;
			}
			if (actor->IsPlayerTeammate()) {
				return true;
			}
			if (actor->IsCommandedActor()) {
				const auto commander = actor->GetCommandingActor();
				return commander && (commander.get() == player || commander->IsPlayerTeammate());
			}
			return false;
		}

		bool IsInvisible(RE::PlayerCharacter* a_player)
		{
			const auto* values = a_player->AsActorValueOwner();
			return values && values->GetActorValue(RE::ActorValue::kInvisibility) > 0.0f;
		}

		// Same interior cell, or (outdoors) same worldspace.
		bool SameArea(RE::Actor* a_actor, RE::Actor* a_other)
		{
			auto* cell = a_other->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				return a_actor->GetParentCell() == cell;
			}
			auto* world = a_other->GetWorldspace();
			return world && a_actor->GetWorldspace() == world;
		}

		// Hostile, on alert (heard or found something: alert flag or weapon out), not fighting yet, fully simulated.
		bool CanRaiseAlarm(RE::Actor* a_actor, RE::PlayerCharacter* a_player, const Settings::Config& a_config)
		{
			if (!a_actor || a_actor == a_player || a_actor->IsDead() || a_actor->IsDisabled() || a_actor->IsInCombat() ||
				!a_actor->Is3DLoaded() || a_actor->IsPlayerTeammate() || a_actor->IsCommandedActor()) {
				return false;
			}
			const auto* process = a_actor->GetActorRuntimeData().currentProcess;
			if (!process || !process->InHighProcess()) {
				return false;
			}
			if (!a_config.bodyAlarmCreatures && !a_actor->HasKeywordString("ActorTypeNPC"sv)) {
				return false;
			}
			if (Pursuit::IsExcludedActor(a_actor) || Pursuit::IsHarmlessAnimal(a_actor) || !a_actor->IsHostileToActor(a_player)) {
				return false;
			}
			if (process->lowProcessFlags.all(RE::AIProcess::LowProcessFlags::kAlert)) {
				return true;
			}
			const auto* state = a_actor->AsActorState();
			return state && state->IsWeaponDrawn();
		}

		// Main thread only.
		void FireDue(RE::PlayerCharacter* a_player, const Settings::Config& a_config, Clock::time_point a_now)
		{
			const bool invisible = a_config.bodyAlarmInvisibilityHides && IsInvisible(a_player);
			std::erase_if(g_pending, [&](const Pending& p) {
				if (p.due > a_now) {
					return false;
				}
				auto finder = p.finder.get();
				if (!finder || finder->IsDead() || finder->IsDisabled()) {
					return true;
				}
				if (finder->IsInCombat()) {
					logger::info("{} [{:08X}] body alarm: already fighting", p.name, p.formID);
					return true;
				}
				if (invisible) {
					logger::info("{} [{:08X}] body alarm: you're invisible, it keeps searching", p.name, p.formID);
					return true;
				}
				const float distance = finder->GetPosition().GetDistance(a_player->GetPosition());
				if (!SameArea(finder.get(), a_player) || distance > a_config.bodyAlarmRange * 1.5f) {
					logger::info("{} [{:08X}] body alarm: you got away ({:.0f} units)", p.name, p.formID, distance);
					return true;
				}
				finder->StartCombat(a_player);
				logger::info("{} [{:08X}] attacks you (body alarm, {:.0f} units away)", p.name, p.formID, distance);
				return true;
			});
		}

		// Main thread only.
		void Tick()
		{
			const auto& config = Settings::Get();
			auto*       player = RE::PlayerCharacter::GetSingleton();
			auto*       ui = RE::UI::GetSingleton();
			if (!player || !player->Is3DLoaded() || !player->GetParentCell() || (ui && ui->GameIsPaused())) {
				return;
			}
			const auto now = Clock::now();

			FireDue(player, config, now);

			std::vector<Body> bodies;
			{
				std::scoped_lock lock(g_bodyLock);
				std::erase_if(g_bodies, [&](const Body& b) {
					auto actor = b.actor.get();
					return now - b.died > kBodyMemory || !actor || !actor->IsDead() || actor->IsDisabled();
				});
				bodies = g_bodies;
			}
			if (bodies.empty() || (config.bodyAlarmInvisibilityHides && IsInvisible(player))) {
				return;
			}

			const auto delay = std::chrono::milliseconds(static_cast<long long>(config.bodyAlarmDelay * 1000.0f));
			const auto playerPos = player->GetPosition();

			RE::ProcessLists::GetSingleton()->ForAllActors([&](RE::Actor* a_actor) {
				if (!CanRaiseAlarm(a_actor, player, config) || !SameArea(a_actor, player)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float toPlayer = a_actor->GetPosition().GetDistance(playerPos);
				if (toPlayer > config.bodyAlarmRange) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto finderID = a_actor->GetFormID();
				if (const auto it = g_lastAlarm.find(finderID); it != g_lastAlarm.end() && now - it->second < kCooldown) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto finderHandle = a_actor->GetHandle();
				if (std::ranges::any_of(g_pending, [&](const Pending& p) { return p.finder == finderHandle; })) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				for (const auto& body : bodies) {
					auto dead = body.actor.get();
					if (!dead || dead.get() == a_actor || g_fired.contains(PairKey(finderID, body.formID)) || !SameArea(dead.get(), a_actor)) {
						continue;
					}
					const float toBody = a_actor->GetPosition().GetDistance(dead->GetPosition());
					if (toBody > config.bodyAlarmBodyDistance || a_actor->IsHostileToActor(dead.get())) {
						continue;  // too far from it, or not one of its own
					}

					g_fired.insert(PairKey(finderID, body.formID));
					g_lastAlarm[finderID] = now;
					g_pending.push_back({ finderHandle, a_actor->GetName(), finderID, now + delay });
					logger::info("{} [{:08X}] found the body of {} [{:08X}] ({:.0f} units from it, {:.0f} units from you): attacks you in {:.1f} s",
						a_actor->GetName(), finderID, body.name, body.formID, toBody, toPlayer, config.bodyAlarmDelay);
					break;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (!a_event || !a_event->actorDying || !IsPlayerSide(a_event->actorKiller.get())) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* dying = a_event->actorDying->As<RE::Actor>();
				if (!dying) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const auto handle = dying->GetHandle();
				std::scoped_lock lock(g_bodyLock);
				if (std::ranges::any_of(g_bodies, [&](const Body& b) { return b.actor == handle; })) {
					return RE::BSEventNotifyControl::kContinue;  // the "dying" and "dead" events both come
				}
				g_bodies.push_back({ handle, dying->GetName(), dying->GetFormID(), Clock::now() });
				if (g_bodies.size() > kMaxBodies) {
					g_bodies.erase(g_bodies.begin());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		const auto& config = Settings::Get();
		if (!config.bodyAlarm) {
			logger::info("Body alarm off");
			return;
		}

		static DeathSink deathSink;
		auto*            events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logger::error("Body alarm: event source not available");
			return;
		}
		events->AddEventSink<RE::TESDeathEvent>(&deathSink);

		std::thread([]() {
			for (;;) {
				std::this_thread::sleep_for(kTick);
				SKSE::GetTaskInterface()->AddTask([]() { Tick(); });
			}
		}).detach();

		logger::info("Body alarm on: an enemy on alert within {:.0f} units of a body your side killed, and within {:.0f} units of you, attacks you after {:.1f} s (creatures {}, invisibility {})",
			config.bodyAlarmBodyDistance, config.bodyAlarmRange, config.bodyAlarmDelay,
			config.bodyAlarmCreatures ? "too" : "no", config.bodyAlarmInvisibilityHides ? "hides you" : "doesn't hide you");
	}

	void Reset()
	{
		{
			std::scoped_lock lock(g_bodyLock);
			g_bodies.clear();
		}
		g_pending.clear();
		g_lastAlarm.clear();
		g_fired.clear();
	}
}

#include "Settings.h"

namespace Settings
{
	namespace
	{
		constexpr auto kIniPath = "Data/SKSE/Plugins/RelentlessPursuit.ini"sv;

		Config g_config;

		std::string Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return std::string(a_text.substr(first, last - first + 1));
		}

		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		void ParseFloat(const std::string& a_value, float& a_out, float a_min, float a_max, std::string_view a_key)
		{
			try {
				a_out = std::clamp(std::stof(a_value), a_min, a_max);
			} catch (...) {
				logger::warn("{} '{}' is not a number, keeping {:.1f}", a_key, a_value, a_out);
			}
		}

		bool ParseBool(const std::string& a_value, bool a_default)
		{
			const auto v = Lower(a_value);
			if (v == "1" || v == "true" || v == "yes" || v == "on") {
				return true;
			}
			if (v == "0" || v == "false" || v == "no" || v == "off") {
				return false;
			}
			return a_default;
		}

		// "Skyrim.esm|0x0131F9, Skyrim.esm|0x0131FF"
		void ParseRaceList(const std::string& a_value, std::vector<ExtraRace>& a_out)
		{
			std::size_t start = 0;
			while (start <= a_value.size()) {
				auto end = a_value.find(',', start);
				if (end == std::string::npos) {
					end = a_value.size();
				}
				const auto entry = Trim(std::string_view(a_value).substr(start, end - start));
				start = end + 1;
				if (entry.empty()) {
					continue;
				}

				const auto bar = entry.find('|');
				if (bar == std::string::npos) {
					logger::warn("Ignoring excluded race '{}': expected Plugin.esp|0xFormID", entry);
					continue;
				}

				ExtraRace race;
				race.plugin = Trim(std::string_view(entry).substr(0, bar));
				const auto idText = Trim(std::string_view(entry).substr(bar + 1));
				try {
					race.localID = static_cast<RE::FormID>(std::stoul(idText, nullptr, 16));
				} catch (...) {
					logger::warn("Ignoring excluded race '{}': '{}' is not a hex FormID", entry, idText);
					continue;
				}
				a_out.push_back(std::move(race));
			}
		}
	}

	void Load()
	{
		std::ifstream file{ std::string(kIniPath) };
		if (!file) {
			logger::warn("{} not found, using defaults", kIniPath);
			return;
		}

		std::string line;
		while (std::getline(file, line)) {
			const auto comment = line.find_first_of(";#");
			if (comment != std::string::npos) {
				line.erase(comment);
			}
			line = Trim(line);
			if (line.empty() || line.front() == '[') {
				continue;
			}

			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto key = Lower(Trim(std::string_view(line).substr(0, eq)));
			const auto value = Trim(std::string_view(line).substr(eq + 1));

			if (key == "bdoorpursuit") {
				g_config.doorPursuit = ParseBool(value, g_config.doorPursuit);
			} else if (key == "bwaterpursuit") {
				g_config.waterPursuit = ParseBool(value, g_config.waterPursuit);
			} else if (key == "bdragonguard") {
				g_config.dragonGuard = ParseBool(value, g_config.dragonGuard);
			} else if (key == "bnozonecells") {
				g_config.noZoneCells = ParseBool(value, g_config.noZoneCells);
			} else if (key == "bteleportfollow") {
				g_config.teleportFollow = ParseBool(value, g_config.teleportFollow);
			} else if (key == "balertedfollow") {
				g_config.alertedFollow = ParseBool(value, g_config.alertedFollow);
			} else if (key == "bskipquestactors") {
				g_config.skipQuestActors = ParseBool(value, g_config.skipQuestActors);
			} else if (key == "ffollowradius") {
				try {
					g_config.followRadius = (std::max)(0.0f, std::stof(value));
				} catch (...) {
					logger::warn("fFollowRadius '{}' is not a number, keeping {:.0f}", value, g_config.followRadius);
				}
			} else if (key == "imaxfollowers") {
				try {
					g_config.maxFollowers = std::clamp(std::stoi(value), 0, 30);
				} catch (...) {
					logger::warn("iMaxFollowers '{}' is not a number, keeping {}", value, g_config.maxFollowers);
				}
			} else if (key == "fpursuittime") {
				try {
					g_config.pursuitTime = std::clamp(std::stof(value), 0.0f, 600.0f);
				} catch (...) {
					logger::warn("fPursuitTime '{}' is not a number, keeping {:.0f}", value, g_config.pursuitTime);
				}
			} else if (key == "imaxreturns") {
				try {
					g_config.maxReturns = std::clamp(std::stoi(value), 0, 10);
				} catch (...) {
					logger::warn("iMaxReturns '{}' is not a number, keeping {}", value, g_config.maxReturns);
				}
			} else if (key == "bbodyalarm") {
				g_config.bodyAlarm = ParseBool(value, g_config.bodyAlarm);
			} else if (key == "fbodyalarmrange") {
				ParseFloat(value, g_config.bodyAlarmRange, 0.0f, 20000.0f, "fBodyAlarmRange"sv);
			} else if (key == "fbodyalarmbodydistance") {
				ParseFloat(value, g_config.bodyAlarmBodyDistance, 0.0f, 8192.0f, "fBodyAlarmBodyDistance"sv);
			} else if (key == "fbodyalarmdelay") {
				ParseFloat(value, g_config.bodyAlarmDelay, 0.0f, 60.0f, "fBodyAlarmDelay"sv);
			} else if (key == "bbodyalarmcreatures") {
				g_config.bodyAlarmCreatures = ParseBool(value, g_config.bodyAlarmCreatures);
			} else if (key == "bbodyalarminvisibilityhides") {
				g_config.bodyAlarmInvisibilityHides = ParseBool(value, g_config.bodyAlarmInvisibilityHides);
			} else if (key == "sextraexcludedraces") {
				ParseRaceList(value, g_config.extraExcludedRaces);
			}
		}

		logger::info("Settings: doors={} noZoneCells={} teleportFollow={} alertedFollow={} followRadius={:.0f} maxFollowers={} skipQuestActors={} pursuitTime={:.0f} maxReturns={} water={} dragonGuard={} extraExcludedRaces={}",
			g_config.doorPursuit, g_config.noZoneCells, g_config.teleportFollow, g_config.alertedFollow, g_config.followRadius, g_config.maxFollowers,
			g_config.skipQuestActors, g_config.pursuitTime, g_config.maxReturns, g_config.waterPursuit, g_config.dragonGuard, g_config.extraExcludedRaces.size());
		logger::info("Settings: bodyAlarm={} range={:.0f} bodyDistance={:.0f} delay={:.1f} creatures={} invisibilityHides={}",
			g_config.bodyAlarm, g_config.bodyAlarmRange, g_config.bodyAlarmBodyDistance, g_config.bodyAlarmDelay,
			g_config.bodyAlarmCreatures, g_config.bodyAlarmInvisibilityHides);
	}

	const Config& Get()
	{
		return g_config;
	}
}

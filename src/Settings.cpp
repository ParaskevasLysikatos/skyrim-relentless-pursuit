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
			} else if (key == "sextraexcludedraces") {
				ParseRaceList(value, g_config.extraExcludedRaces);
			}
		}

		logger::info("Settings: doors={} water={} dragonGuard={} extraExcludedRaces={}",
			g_config.doorPursuit, g_config.waterPursuit, g_config.dragonGuard, g_config.extraExcludedRaces.size());
	}

	const Config& Get()
	{
		return g_config;
	}
}

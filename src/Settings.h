#pragma once

namespace Settings
{
	struct ExtraRace
	{
		std::string plugin;
		RE::FormID  localID{ 0 };
	};

	struct Config
	{
		bool doorPursuit{ true };   // switch off the combat boundary on every encounter zone
		bool waterPursuit{ true };  // swimming races keep fighting in water
		bool dragonGuard{ true };   // excluded creatures never follow you through load doors

		std::vector<ExtraRace> extraExcludedRaces;  // on top of dragons, which are always excluded
	};

	// Reads Data/SKSE/Plugins/RelentlessPursuit.ini. Missing file or keys fall back to the defaults above.
	void Load();

	[[nodiscard]] const Config& Get();
}

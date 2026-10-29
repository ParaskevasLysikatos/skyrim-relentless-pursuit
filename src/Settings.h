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
		bool noZoneCells{ true };   // cells without an encounter zone get the no-boundary zone while you're in them

		bool  teleportFollow{ true };     // enemies fighting you come through the load door after you
		bool  alertedFollow{ true };      // hostile enemies on alert (not fighting you yet) come too
		float followRadius{ 8192.0f };    // how far away (units) an enemy can be when you use the door
		int   maxFollowers{ 8 };          // at most this many per door
		bool  skipQuestActors{ false };   // leave enemies that are in a quest alias behind
		float pursuitTime{ 30.0f };       // seconds each one is kept in the fight after coming through
		int   maxReturns{ 2 };            // times one that walks back through the door is brought out again

		std::vector<ExtraRace> extraExcludedRaces;  // on top of dragons, which are always excluded
	};

	// Reads Data/SKSE/Plugins/RelentlessPursuit.ini. Missing file or keys fall back to the defaults above.
	void Load();

	[[nodiscard]] const Config& Get();
}

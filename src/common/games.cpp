#include "games.h"
#include "strings.h"

#include <cstring>

namespace hl {

struct GameName {
	const char *exe; /* lower case, without .exe */
	const char *name;
};

static const GameName k_games[] = {
	{"valorant-win64-shipping", "VALORANT"},
	{"cs2", "Counter-Strike 2"},
	{"csgo", "Counter-Strike"},
	{"r5apex", "Apex Legends"},
	{"r5apex_dx12", "Apex Legends"},
	{"fortniteclient-win64-shipping", "Fortnite"},
	{"rocketleague", "Rocket League"},
	{"league of legends", "League of Legends"},
	{"dota2", "Dota 2"},
	{"overwatch", "Overwatch 2"},
	{"r6-siege", "Rainbow Six Siege"},
	{"rainbowsix", "Rainbow Six Siege"},
	{"rainbowsix_vulkan", "Rainbow Six Siege"},
	{"cod", "Call of Duty"},
	{"modernwarfare", "Call of Duty"},
	{"bf2042", "Battlefield 2042"},
	{"bf6", "Battlefield 6"},
	{"pubg", "PUBG"},
	{"tslgame", "PUBG"},
	{"eldenring", "Elden Ring"},
	{"minecraft", "Minecraft"},
	{"minecraft.windows", "Minecraft"}, /* Bedrock edition */
	{"javaw", "Minecraft"},             /* Java edition (only with a Minecraft window title) */
	{"java", "Minecraft"},
	{"gta5", "Grand Theft Auto V"},
	{"gta5_enhanced", "Grand Theft Auto V"},
	{"rdr2", "Red Dead Redemption 2"},
	{"destiny2", "Destiny 2"},
	{"deadlock", "Deadlock"},
	{"marvel-win64-shipping", "Marvel Rivals"},
	{"thefinals", "THE FINALS"},
	{"discovery", "THE FINALS"},
	{"helldivers2", "Helldivers 2"},
	{"cyberpunk2077", "Cyberpunk 2077"},
	{"witcher3", "The Witcher 3"},
	{"eft", "Escape from Tarkov"},
	{"escapefromtarkov", "Escape from Tarkov"},
	{"rust", "Rust"},
	{"robloxplayerbeta", "Roblox"},
	{"osu!", "osu!"},
	{"fifa", "EA SPORTS FC"},
	{"fc25", "EA SPORTS FC 25"},
	{"fc26", "EA SPORTS FC 26"},
	{"warframe.x64", "Warframe"},
	{"palworld-win64-shipping", "Palworld"},
	{"genshinimpact", "Genshin Impact"},
	{"starrail", "Honkai: Star Rail"},
	{"zenlesszonezero", "Zenless Zone Zero"},
	{"wow", "World of Warcraft"},
	{"diablo iv", "Diablo IV"},
	{"pathofexile", "Path of Exile"},
	{"pathofexile2", "Path of Exile 2"},
	{"seaofthieves", "Sea of Thieves"},
	{"halo infinite", "Halo Infinite"},
	{"forzahorizon5", "Forza Horizon 5"},
	{"bg3", "Baldur's Gate 3"},
	{"bg3_dx11", "Baldur's Gate 3"},
	{"hunt", "Hunt: Showdown"},
	{"deadbydaylight-win64-shipping", "Dead by Daylight"},
	{"tf2", "Team Fortress 2"},
	{"hl2", "Half-Life 2"},
	{"arc-win64-shipping", "ARC Raiders"},
	{"pioneergame", "ARC Raiders"},
};

static std::string strip_suffixes(std::string base)
{
	static const char *suffixes[] = {"-win64-shipping", "_win64_shipping", "-win64", "_x64", "64", "_dx12",
					 "_dx11"};
	for (const char *suf : suffixes) {
		size_t n = strlen(suf);
		if (base.size() > n && to_lower(base.substr(base.size() - n)) == suf) {
			base.resize(base.size() - n);
			break;
		}
	}
	return base;
}

std::string game_display_name(const std::string &executable, const std::string &window_title)
{
	std::string exe = executable;
	size_t slash = exe.find_last_of("\\/");
	if (slash != std::string::npos)
		exe = exe.substr(slash + 1);
	std::string lower = to_lower(exe);
	if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".exe") {
		lower.resize(lower.size() - 4);
		exe.resize(exe.size() - 4);
	}

	for (const auto &g : k_games)
		if (lower == g.exe)
			return g.name;

	/* Prefer a short window title ("Hollow Knight") over a cryptic exe name. */
	if (!window_title.empty() && window_title.size() <= 40)
		return window_title;

	if (!exe.empty())
		return strip_suffixes(exe);
	return "Desktop";
}

static std::string exe_base(const std::string &executable)
{
	std::string exe = executable;
	size_t slash = exe.find_last_of("\\/");
	if (slash != std::string::npos)
		exe = exe.substr(slash + 1);
	std::string lower = to_lower(exe);
	if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".exe")
		lower.resize(lower.size() - 4);
	return lower;
}

bool is_java_runtime(const std::string &executable)
{
	const std::string base = exe_base(executable);
	return base == "javaw" || base == "java";
}

bool is_known_game(const std::string &executable, const std::string &window_title,
		   const std::string &window_class)
{
	const std::string base = exe_base(executable);
	if (base.empty())
		return false;
	if (is_java_runtime(executable)) {
		/* any Java app runs as javaw.exe: only game windows count. Minecraft draws
		 * through LWJGL, whose window class is GLFW30 (LWJGL 3, Minecraft 1.13+) or
		 * LWJGL (LWJGL 2, older versions); the title also changes with the version,
		 * server and mod loader, so it is only a second check. */
		if (window_class == "GLFW30" || window_class == "LWJGL")
			return true;
		const std::string t = to_lower(window_title);
		static const char *titles[] = {"minecraft", "lunar client", "badlion", "feather client", "labymod"};
		for (const char *w : titles)
			if (t.find(w) != std::string::npos)
				return true;
		return false;
	}
	for (const auto &g : k_games)
		if (base == g.exe)
			return true;
	return false;
}

} // namespace hl

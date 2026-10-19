#pragma once
#include "Fill.h"
#include "rec/Replay.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace demo_to_rec {

struct Mark {
	int at{}; // frame, or server tick if server_tick
	bool server_tick{};
	int type{};
	int zone{};
};

struct ExportOptions {
	FillOptions fill;
	int team{}; // 0 = player's team
	std::vector<Mark> marks;
	std::string target;
	std::string classname = "player";
};

struct Conversion {
	rec::Replay replay;
	rec::ExportInfo info;
	std::vector<Tick> ticks;
	size_t interpolated{};
	size_t from_commands{};
};

rec::Frame to_frame(const Tick& tick);

Conversion convert(const DemoTracks& demo, const PlayerTrack& player, int first_tick, int last_tick,
	const ExportOptions& options = {});

std::vector<std::pair<size_t, size_t>> ranges(const std::vector<Tick>& ticks, bool Tick::* flag);

void write_export(const std::filesystem::path& path, const Conversion& conversion);

}

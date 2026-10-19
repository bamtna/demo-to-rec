#include "Convert.h"
#include <algorithm>
#include <stdexcept>

namespace demo_to_rec {

rec::Frame to_frame(const Tick& tick)
{
	rec::Frame frame;
	frame.buttons = tick.buttons;
	frame.position = { tick.origin.x, tick.origin.y, tick.origin.z };
	frame.angles = { tick.angles.x, tick.angles.y };
	frame.velocity = { tick.velocity.x, tick.velocity.y, tick.velocity.z };
	return frame;
}

std::vector<std::pair<size_t, size_t>> ranges(const std::vector<Tick>& ticks, bool Tick::* flag)
{
	std::vector<std::pair<size_t, size_t>> result;
	for (size_t i = 0; i < ticks.size(); i++) {
		if (!(ticks[i].*flag)) continue;
		if (!result.empty() && result.back().second + 1 == i) {
			result.back().second = i;
		}
		else {
			result.push_back({ i, i });
		}
	}
	return result;
}

Conversion convert(const DemoTracks& demo, const PlayerTrack& player, int first_tick, int last_tick,
	const ExportOptions& options)
{
	Conversion conversion;
	conversion.ticks = fill_ticks(demo, player, first_tick, last_tick, options.fill);

	int team = options.team;
	if (!team) {
		auto it = std::lower_bound(player.samples.begin(), player.samples.end(), first_tick,
			[](const Sample& s, int t) { return s.tick < t; });
		team = it != player.samples.end() ? it->team : 0;
	}
	if (team != rec::TEAM_T && team != rec::TEAM_CT) {
		throw std::runtime_error("Team must be 2 (T) or 3 (CT)");
	}

	std::vector<rec::Frame> frames;
	frames.reserve(conversion.ticks.size());
	for (const auto& tick : conversion.ticks) {
		frames.push_back(to_frame(tick));
		conversion.interpolated += tick.interpolated;
		conversion.from_commands += tick.from_command;
	}
	conversion.replay = rec::make_v2(team, std::move(frames));

	for (const auto& mark : options.marks) {
		int frame = mark.server_tick ? mark.at - first_tick : mark.at;
		if (frame < 0 || frame > int(conversion.ticks.size())) {
			throw std::runtime_error("Bookmark at " + std::to_string(mark.at) + " is outside the replay");
		}
		conversion.replay.bookmarks.push_back({ frame, mark.type, mark.zone, options.target, options.classname });
	}

	auto& info = conversion.info;
	info.map = demo.map;
	info.tick_interval = demo.tick_interval;
	info.source = std::filesystem::path(demo.path).filename().string();
	info.source_tick = first_tick;
	auto json_ranges = [](const std::vector<std::pair<size_t, size_t>>& list) {
		std::string text = "[";
		for (const auto& [a, b] : list) {
			if (text.size() > 1) text += ", ";
			text += "[" + std::to_string(a) + ", " + std::to_string(b) + "]";
		}
		return text + "]";
	};
	info.extra = {
		{ "demo_kind", rec::json_string(cssdemo::demo_kind_name(demo.kind)) },
		{ "player", rec::json_string(player.name) },
		{ "user_id", std::to_string(player.user_id) },
		{ "steam_id", rec::json_string(player.guid) },
		{ "last_tick", std::to_string(last_tick) },
		{ "interpolation", rec::json_string(options.fill.interpolation == Interpolation::Cubic ? "cubic" : "linear") },
		{ "interpolated_frames", std::to_string(conversion.interpolated) },
		{ "command_frames", std::to_string(conversion.from_commands) },
		{ "interpolated", json_ranges(ranges(conversion.ticks, &Tick::interpolated)) },
		{ "estimated_velocity", json_ranges(ranges(conversion.ticks, &Tick::velocity_estimated)) },
	};
	return conversion;
}

void write_export(const std::filesystem::path& path, const Conversion& conversion)
{
	rec::save_export(path, conversion.replay, conversion.info);
}

}

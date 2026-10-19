#include "demo_to_rec/demo_to_rec.h"
#include <charconv>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace demo_to_rec;

namespace {

const char* usage = R"(Usage:
  demo-to-rec list <demo> [--player WHO] [--min-ticks N]
  demo-to-rec export <demo> --player WHO (--segment N | --ticks FIRST LAST) -o <out.rec[.bz2]> [options]
  demo-to-rec compare <demo> --player WHO (--segment N | --ticks FIRST LAST) <reference.rec> [--offset K]
      [--decimate N] [options]
  demo-to-rec dump <demo> --player WHO
  demo-to-rec info <replay.rec[.bz2]> [--frames]

WHO is a userid, steamid or name.

Options:
  --team 2|3                  default: the player's team
  --mark FRAME TYPE ZONE      bookmark (type 2 enter, 3 leave, 1 checkpoint; zone 1 start, 99 end)
  --mark-tick TICK TYPE ZONE  bookmark at a server tick
  --target NAME               bookmark targetname
  --classname NAME            bookmark classname (default player)
  --linear                    linear interpolation instead of cubic
  --no-commands               ignore the recording player's user commands
  --no-duck                   don't derive IN_DUCK
  --no-jump                   don't derive IN_JUMP
  --gravity G                 default 800
)";

int to_int(const std::string& text)
{
	int value{};
	auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size()) {
		throw std::runtime_error("Invalid number: " + text);
	}
	return value;
}

struct Args {
	std::vector<std::string> positional;
	std::string player;
	std::optional<int> segment;
	std::optional<std::pair<int, int>> ticks;
	std::optional<int> offset;
	std::string output;
	int min_ticks{};
	int decimate{};
	bool frames{};
	ExportOptions options;
};

Args parse_args(const std::vector<std::string>& argv)
{
	Args args;
	for (size_t i = 0; i < argv.size(); i++) {
		const std::string& arg = argv[i];
		auto value = [&](size_t count = 1) -> const std::string& {
			if (i + count >= argv.size()) throw std::runtime_error("Missing value for " + arg);
			return argv[++i];
		};
		if (arg == "--player") args.player = value();
		else if (arg == "--segment") args.segment = to_int(value());
		else if (arg == "--ticks") {
			int first = to_int(value(2));
			args.ticks = { first, to_int(value()) };
		}
		else if (arg == "--offset") args.offset = to_int(value());
		else if (arg == "-o" || arg == "--output") args.output = value();
		else if (arg == "--min-ticks") args.min_ticks = to_int(value());
		else if (arg == "--decimate") args.decimate = to_int(value());
		else if (arg == "--frames") args.frames = true;
		else if (arg == "--team") args.options.team = to_int(value());
		else if (arg == "--mark" || arg == "--mark-tick") {
			Mark mark;
			mark.server_tick = arg == "--mark-tick";
			mark.at = to_int(value(3));
			mark.type = to_int(value());
			mark.zone = to_int(value());
			args.options.marks.push_back(mark);
		}
		else if (arg == "--target") args.options.target = value();
		else if (arg == "--classname") args.options.classname = value();
		else if (arg == "--linear") args.options.fill.interpolation = Interpolation::Linear;
		else if (arg == "--no-commands") args.options.fill.use_commands = false;
		else if (arg == "--no-duck") args.options.fill.derive_duck = false;
		else if (arg == "--no-jump") args.options.fill.derive_jump = false;
		else if (arg == "--gravity") args.options.fill.gravity = std::stod(value());
		else if (arg.starts_with("--")) throw std::runtime_error("Unknown option " + arg);
		else args.positional.push_back(arg);
	}
	return args;
}

const PlayerTrack& require_player(const DemoTracks& tracks, const std::string& who)
{
	if (who.empty()) throw std::runtime_error("--player is required");
	const PlayerTrack* player = tracks.find_player(who);
	if (!player) throw std::runtime_error("No single player matches " + who);
	return *player;
}

std::pair<int, int> require_range(const DemoTracks& tracks, const PlayerTrack& player, const Args& args)
{
	if (args.ticks) return *args.ticks;
	auto segments = find_segments(player, tracks.tick_interval, args.options.fill.segments);
	if (!args.segment) throw std::runtime_error("--segment or --ticks is required");
	if (*args.segment < 0 || size_t(*args.segment) >= segments.size()) {
		throw std::runtime_error(player.name + " has " + std::to_string(segments.size()) + " segments");
	}
	const auto& segment = segments[size_t(*args.segment)];
	return { segment.first_tick, segment.last_tick };
}

void print_demo(const DemoTracks& tracks)
{
	std::printf("%s: %s, %s demo, ticks %d-%d, tick interval %g, %zu packets", tracks.path.c_str(), tracks.map.c_str(),
		cssdemo::demo_kind_name(tracks.kind), tracks.first_tick, tracks.last_tick, tracks.tick_interval,
		tracks.packet_ticks.size());
	if (tracks.parse_errors) std::printf(", %zu parse errors", tracks.parse_errors);
	if (tracks.truncated) std::printf(", truncated");
	std::printf("\n");
}

int command_list(const Args& args)
{
	if (args.positional.size() != 1) throw std::runtime_error(usage);
	auto tracks = load_demo(args.positional[0]);
	print_demo(tracks);
	const PlayerTrack* only = args.player.empty() ? nullptr : &require_player(tracks, args.player);
	for (const auto& [user_id, player] : tracks.players) {
		if (only && only != &player) continue;
		std::printf("userid %d \"%s\" %s%s%s\n", user_id, player.name.c_str(), player.guid.c_str(),
			player.fake_player ? " bot" : "", player.is_local ? " recording" : "");
		int index = 0;
		for (const auto& segment : find_segments(player, tracks.tick_interval)) {
			int number = index++;
			if (segment.ticks() < args.min_ticks) continue;
			std::printf("  segment %d: ticks %d-%d, %d ticks (%.2fs), team %d, %s to %s\n", number, segment.first_tick,
				segment.last_tick, segment.ticks(), segment.ticks() * tracks.tick_interval, segment.team,
				break_name(segment.starts), break_name(segment.ends));
		}
	}
	return 0;
}

int command_export(const Args& args)
{
	if (args.positional.size() != 1 || args.output.empty()) throw std::runtime_error(usage);
	auto tracks = load_demo(args.positional[0]);
	const auto& player = require_player(tracks, args.player);
	auto [first, last] = require_range(tracks, player, args);
	auto conversion = convert(tracks, player, first, last, args.options);
	write_export(args.output, conversion);
	std::printf("%s: %s ticks %d-%d, %zu frames (%zu interpolated, %zu from user commands), team %d -> %s\n",
		tracks.map.c_str(), player.name.c_str(), first, last, conversion.ticks.size(), conversion.interpolated,
		conversion.from_commands, conversion.replay.team, args.output.c_str());
	return 0;
}

int command_compare(const Args& args)
{
	if (args.positional.size() != 2) throw std::runtime_error(usage);
	auto tracks = load_demo(args.positional[0]);
	PlayerTrack player = require_player(tracks, args.player);
	auto [first, last] = require_range(tracks, player, args);
	if (args.decimate > 1) {
		std::erase_if(player.samples, [&](const Sample& s) {
			return s.tick != first && s.tick != last && (s.tick - first) % args.decimate != 0;
		});
	}
	auto conversion = convert(tracks, player, first, last, args.options);
	auto reference = rec::load(args.positional[1]);
	std::vector<bool> interpolated;
	for (const auto& tick : conversion.ticks) interpolated.push_back(tick.interpolated);
	auto comparison = compare(conversion.replay, interpolated, reference, args.offset);
	std::printf("%s ticks %d-%d against %s (%zu frames)\n%s", player.name.c_str(), first, last,
		args.positional[1].c_str(), reference.frames.size(), describe(comparison).c_str());
	return 0;
}

int command_dump(const Args& args)
{
	if (args.positional.size() != 1) throw std::runtime_error(usage);
	auto tracks = load_demo(args.positional[0]);
	const auto& player = require_player(tracks, args.player);
	std::printf("tick,x,y,z,pitch,yaw,vx,vy,vz,flags,team,life_state,observer_mode,interpolation_frame,tick_base\n");
	for (const auto& s : player.samples) {
		Vector v = s.velocity.value_or(Vector{});
		std::printf("%d,%.5f,%.5f,%.5f,%.4f,%.4f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d\n", s.tick, s.origin.x, s.origin.y,
			s.origin.z, s.angles.x, s.angles.y, v.x, v.y, v.z, s.flags, s.team, s.life_state, s.observer_mode,
			s.interpolation_frame, s.tick_base);
	}
	return 0;
}

int command_info(const Args& args)
{
	if (args.positional.size() != 1) throw std::runtime_error(usage);
	auto replay = rec::load(args.positional[0]);
	std::printf("version %d, team %d, %d declared frames, %zu frames, %d cells, %zu bookmarks\n", replay.version,
		replay.team, replay.declared_frames, replay.frames.size(), replay.frame_cells, replay.bookmarks.size());
	for (const auto& mark : replay.bookmarks) {
		std::printf("bookmark frame %d type %d zone %d \"%s\" \"%s\"\n", mark.tick, mark.type, mark.zone,
			mark.target.c_str(), mark.classname.c_str());
	}
	if (args.frames) {
		for (size_t i = 0; i < replay.frames.size(); i++) {
			const auto& f = replay.frames[i];
			std::printf("%zu %u %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", i, f.buttons, f.position[0], f.position[1],
				f.position[2], f.angles[0], f.angles[1], f.velocity[0], f.velocity[1], f.velocity[2]);
		}
	}
	return 0;
}

}

int main(int argc, char* argv[])
{
	try {
		if (argc < 2) {
			std::cerr << usage;
			return 1;
		}
		std::string command = argv[1];
		auto args = parse_args(std::vector<std::string>(argv + 2, argv + argc));
		if (command == "list") return command_list(args);
		if (command == "export") return command_export(args);
		if (command == "compare") return command_compare(args);
		if (command == "dump") return command_dump(args);
		if (command == "info") return command_info(args);
		std::cerr << usage;
		return 1;
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << "\n";
		return 1;
	}
}

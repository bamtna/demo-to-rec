#include "demo_to_rec/demo_to_rec.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace demo_to_rec;
namespace fs = std::filesystem;

namespace {

constexpr int SKIPPED = 77;

void require(bool result, const std::string& message)
{
	if (!result) throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance)
{
	return std::abs(a - b) <= tolerance;
}

std::vector<uint8_t> bytes(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	require(bool(file), "Cannot open " + path.string());
	return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

std::string text_file(const fs::path& path)
{
	auto data = bytes(path);
	return { data.begin(), data.end() };
}

std::string number(float value)
{
	char text[32];
	std::snprintf(text, sizeof(text), "%.9g", value);
	return text;
}

// same as tools/ksf_inputs.py
std::string dump(const rec::Replay& replay)
{
	std::ostringstream out;
	out << replay.version << ' ' << replay.team << ' ' << replay.declared_frames << ' ' << replay.frames.size() << ' '
		<< replay.frame_cells << '\n';
	for (const auto& mark : replay.bookmarks) {
		out << "mark " << mark.tick << ' ' << mark.type << ' ' << mark.zone << ' ' << mark.target << '|'
			<< mark.classname << '\n';
	}
	for (size_t i = 0; i < replay.frames.size(); i++) {
		const auto& frame = replay.frames[i];
		out << i << ' ' << frame.buttons;
		for (float value : frame.position) out << ' ' << number(value);
		for (float value : frame.angles) out << ' ' << number(value);
		for (float value : frame.velocity) out << ' ' << number(value);
		out << '\n';
	}
	return out.str();
}

bool same(float a, float b)
{
	return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

bool same(const rec::Replay& a, const rec::Replay& b)
{
	if (a.version != b.version || a.team != b.team || a.declared_frames != b.declared_frames
		|| a.frame_cells != b.frame_cells || a.extension != b.extension || a.extra_cells != b.extra_cells
		|| a.bookmarks.size() != b.bookmarks.size() || a.frames.size() != b.frames.size()) {
		return false;
	}
	for (size_t i = 0; i < a.bookmarks.size(); i++) {
		const auto& x = a.bookmarks[i];
		const auto& y = b.bookmarks[i];
		if (x.tick != y.tick || x.type != y.type || x.zone != y.zone || x.target != y.target
			|| x.classname != y.classname) {
			return false;
		}
	}
	for (size_t i = 0; i < a.frames.size(); i++) {
		const auto& x = a.frames[i];
		const auto& y = b.frames[i];
		if (x.buttons != y.buttons || !same(x.roll, y.roll)) return false;
		for (int axis = 0; axis < 3; axis++) {
			if (!same(x.position[axis], y.position[axis]) || !same(x.velocity[axis], y.velocity[axis])
				|| (axis < 2 && !same(x.angles[axis], y.angles[axis]))) {
				return false;
			}
		}
	}
	return true;
}

std::vector<uint8_t> header(std::initializer_list<int> values)
{
	std::vector<uint8_t> result;
	for (int value : values) {
		for (int i = 0; i < 4; i++) result.push_back(uint8_t(uint32_t(value) >> (8 * i)));
	}
	return result;
}

template <typename F>
void throws(F&& f, const std::string& message)
{
	try {
		f();
	}
	catch (const std::runtime_error&) {
		return;
	}
	throw std::runtime_error(message);
}

int test_rec_reader(const fs::path& fixtures)
{
	for (const char* name : { "wr-v2", "session-v3" }) {
		auto replay = rec::load(fixtures / (std::string(name) + ".rec"));
		require(dump(replay) == text_file(fixtures / (std::string(name) + ".txt")),
			std::string(name) + " differs from the python reader's dump");
	}
	if (rec::has_bzip2()) {
		require(dump(rec::load(fixtures / "wr-v2.rec.bz2")) == dump(rec::load(fixtures / "wr-v2.rec")),
			"Compressed replay differs");
	}

	auto frame = header({ 8, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
	auto v2 = header({ 2, 2, 1, 0 });
	v2.insert(v2.end(), frame.begin(), frame.end());
	require(rec::parse(v2).frames.size() == 1, "Valid v2 rejected");
	auto truncated = v2;
	truncated.pop_back();
	throws([&] { rec::parse(truncated); }, "Misaligned frames accepted");
	throws([&] { rec::parse(header({ 4, 2, 1, 0 })); }, "Unknown version accepted");
	throws([&] { rec::parse(std::vector<uint8_t>{ 'B', 'Z', 'h', '9' }); }, "Truncated bzip2 accepted");
	throws([&] { rec::parse(header({ 3, 2, 1, 0, 9, 0 })); }, "Short v3 frame accepted");
	auto marked = header({ 2, 2, 1, 1, 2, 0, 0 });
	marked.resize(16 + 524);
	marked.insert(marked.end(), frame.begin(), frame.end());
	throws([&] { rec::parse(marked); }, "Bookmark past the end accepted");
	auto nan = v2;
	auto bits = std::bit_cast<uint32_t>(std::nanf(""));
	std::memcpy(nan.data() + 16 + 4, &bits, 4);
	throws([&] { rec::parse(nan); }, "Nonfinite position accepted");
	std::cout << "reader matches the fixture dumps\n";
	return 0;
}

void survives(const rec::Replay& replay, const std::string& message)
{
	for (bool compress : { false, true }) {
		if (compress && !rec::has_bzip2()) continue;
		auto data = rec::serialise(replay, compress);
		require(same(rec::parse(data), replay), message);
		require(rec::serialise(rec::parse(data), compress) == data, message);
	}
}

int test_rec_writer(const fs::path& fixtures)
{
	for (const char* name : { "wr-v2.rec", "session-v3.rec" }) {
		auto source = bytes(fixtures / name);
		require(rec::serialise(rec::parse(source), false) == source, std::string(name) + " doesn't round-trip");
	}
	if (rec::has_bzip2()) {
		require(rec::serialise(rec::load(fixtures / "wr-v2.rec.bz2"), true) == bytes(fixtures / "wr-v2.rec.bz2"),
			"bzip2 output differs from python's bz2");
	}

	rec::Replay run;
	run.version = 2;
	run.team = 3;
	run.declared_frames = 4;
	run.bookmarks = { { 0, 2, 1, "", "player" }, { 1, 3, 1, "start", "player" },
		{ 4, 2, 99, std::string(255, 'x'), "playersurf" } };
	run.frames = { { 0, { -0.f, 1e-40f, -16384 }, { 89, 359.5f }, { 0, 0, 0 } },
		{ 8 | 2, { 1.5f, -2.25f, 3 }, { -90, -206.06f }, { 3500, -3500, -1e-3f } },
		{ 0xffffffffu, { 16384, 0, 1 }, { 0.1f, 180 }, { 1, 2, 3 }, 0.5f },
		{ 1024, { 7, 8, 9 }, { 0, -0.f }, { 4, 5, 6 } } };
	survives(run, "Synthetic v2 run changed");
	auto session = run;
	session.version = 3;
	session.frame_cells = 12;
	session.declared_frames = 4004;
	session.extension = { 0x3cb43958u, 0xffffffffu, 0 };
	session.extra_cells = { 1, 0x7fc00001u, 0, 2, 0xffffffffu, 3, 4, 5 };
	survives(session, "Synthetic v3 run changed");

	auto refuses = [](rec::Replay replay, const std::function<void(rec::Replay&)>& change, const std::string& message) {
		change(replay);
		throws([&] { rec::serialise(replay, false); }, message);
	};
	refuses(run, [](auto& r) { r.version = 4; }, "Unknown version written");
	refuses(run, [](auto& r) { r.frame_cells = 12; }, "v2 with extra cells written");
	refuses(run, [](auto& r) { r.extension = { 1 }; }, "v2 with an extension written");
	refuses(session, [](auto& r) { r.frame_cells = 11; }, "v3 with missing cells written");
	refuses(run, [](auto& r) { r.frames.clear(); }, "Empty replay written");
	refuses(run, [](auto& r) { r.bookmarks[2].tick = 5; }, "Bookmark past the end written");
	refuses(run, [](auto& r) { r.bookmarks[0].target = std::string(256, 'x'); }, "Long bookmark text written");
	refuses(run, [](auto& r) { r.frames[1].velocity[2] = std::nanf(""); }, "Nonfinite velocity written");

	auto made = rec::make_v2(2, run.frames);
	require(made.declared_frames == 4 && made.frame_cells == 10 && made.version == 2, "make_v2 header");

	auto directory = fs::temp_directory_path() / "demo-to-rec-tests";
	fs::create_directories(directory);
	rec::ExportInfo info;
	info.map = "surf_\"test\"";
	info.tick_interval = 0.015;
	info.source = "x.dem";
	info.source_tick = 100;
	info.extra = { { "player", rec::json_string("a\\b") } };
	for (const char* name : { "export.rec", "export.rec.bz2" }) {
		if (std::string(name).ends_with(".bz2") && !rec::has_bzip2()) continue;
		auto path = directory / name;
		rec::save_export(path, made, info);
		require(same(rec::load(path), made), std::string(name) + " reads back differently");
		auto json = text_file(rec::sidecar_path(path));
		require(json.find("\"map\": \"surf_\\\"test\\\"\"") != std::string::npos, "Sidecar map not escaped");
		require(json.find("\"tick_rate\": 66.66666") != std::string::npos, "Sidecar tick rate missing");
		require(json.find("\"player\": \"a\\\\b\"") != std::string::npos, "Sidecar extra field missing");
	}
	throws([&] { rec::save_export(directory / "export.txt", made, info); }, "Export to a non .rec accepted");
	fs::remove_all(directory);
	std::cout << "writer round-trips\n";
	return 0;
}

int test_rec_corpus(const std::string& folder)
{
	if (folder.empty() || !fs::is_directory(folder)) {
		std::cout << "no replay corpus, skipped\n";
		return SKIPPED;
	}
	int count = 0;
	for (const auto& entry : fs::recursive_directory_iterator(folder)) {
		if (entry.path().extension() != ".rec") continue;
		auto source = bytes(entry.path());
		require(rec::serialise(rec::parse(source), false) == source, "Corpus replay differs: " + entry.path().string());
		count++;
	}
	require(count > 0, "Replay corpus is empty");
	std::cout << count << " corpus replays round-trip\n";
	return 0;
}

std::vector<std::pair<Vector, Vector>> ballistic(int ticks, double dt, double gravity, Vector origin, Vector velocity)
{
	std::vector<std::pair<Vector, Vector>> path;
	for (int i = 0; i < ticks; i++) {
		velocity.z = float(velocity.z - 0.5 * gravity * dt);
		origin = { float(origin.x + velocity.x * dt), float(origin.y + velocity.y * dt), float(origin.z + velocity.z * dt) };
		velocity.z = float(velocity.z - 0.5 * gravity * dt);
		path.push_back({ origin, velocity });
	}
	return path;
}

Sample playing_sample(int tick, Vector origin)
{
	Sample sample;
	sample.tick = tick;
	sample.origin = origin;
	sample.team = 3;
	sample.interpolation_frame = 0;
	return sample;
}

DemoTracks synthetic_demo()
{
	DemoTracks demo;
	demo.tick_interval = 0.015;
	demo.map = "surf_synthetic";
	demo.kind = cssdemo::DemoKind::SourceTV;
	return demo;
}

int test_interpolation()
{
	require(near(lerp_angle(350, 10, 0.5), 0, 1e-4), "Yaw doesn't wrap through 0");
	require(near(lerp_angle(350, 10, 0.25), 355, 1e-4), "Yaw quarter way");
	require(near(lerp_angle(10, 350, 0.75), 355, 1e-4), "Yaw wraps backwards");
	require(near(lerp_angle(-170, 170, 0.5), 180, 1e-4), "Signed yaw doesn't wrap through 180");
	require(near(lerp_angle(90, 100, 0.5), 95, 1e-4), "Plain yaw");
	require(near(normalize_pitch(350), -10, 1e-4) && near(normalize_pitch(45), 45, 1e-4), "Pitch normalize");
	Vector p1{ 1, 2, 3 }, p2{ 5, -2, 9 }, m{ 3, 3, 3 };
	Vector h0 = hermite(p1, m, p2, m, 0), h1 = hermite(p1, m, p2, m, 1);
	require(near(h0.x, 1, 1e-6) && near(h1.z, 9, 1e-6), "Hermite endpoints");

	double dt = 0.015;
	auto path = ballistic(200, dt, 800, { 0, 0, 1000 }, { 1200, -300, 250 });
	for (size_t i = 1; i < path.size(); i++) {
		Vector v = estimate_velocity(path[i - 1].first, path[i].first, false, dt, 800);
		require(near(v.x, path[i].second.x, 0.05) && near(v.y, path[i].second.y, 0.05) && near(v.z, path[i].second.z, 0.05),
			"Velocity estimate is off in the air");
	}

	// snapshot every 4 ticks, 1/32 unit
	auto demo = synthetic_demo();
	PlayerTrack player;
	player.user_id = 7;
	player.name = "sourcetv";
	int first_tick = 1000;
	for (size_t i = 0; i < path.size(); i += 4) {
		Vector o = path[i].first;
		o = { std::round(o.x * 32) / 32, std::round(o.y * 32) / 32, std::round(o.z * 32) / 32 };
		auto sample = playing_sample(first_tick + int(i), o);
		sample.angles = { 350.0f + float(i) * 0.05f, std::fmod(355.0f + float(i) * 0.5f, 360.0f), 0 };
		player.samples.push_back(sample);
	}
	int last_tick = player.samples.back().tick;
	auto errors = [&](Interpolation mode) {
		FillOptions options;
		options.interpolation = mode;
		auto ticks = fill_ticks(demo, player, first_tick, last_tick, options);
		require(ticks.size() == size_t(last_tick - first_tick + 1), "Not every tick filled");
		double worst = 0;
		for (size_t i = 0; i < ticks.size(); i++) {
			require(ticks[i].tick == first_tick + int(i), "Ticks not contiguous");
			require(ticks[i].interpolated == (i % 4 != 0), "Interpolated flag wrong");
			require(ticks[i].velocity_estimated, "Velocity not marked estimated");
			const Vector& truth = path[i].first;
			worst = std::max(worst, double(std::hypot(ticks[i].origin.x - truth.x, ticks[i].origin.y - truth.y,
				ticks[i].origin.z - truth.z)));
			double yaw = std::fmod(355.0 + i * 0.5, 360.0);
			double d = std::fmod(ticks[i].angles.y - yaw + 540.0, 360.0) - 180.0;
			require(std::abs(d) < 0.01, "Yaw interpolation off at tick " + std::to_string(i));
			require(ticks[i].angles.y >= 0 && ticks[i].angles.y < 360, "Yaw left the networked range");
			require(near(ticks[i].angles.x, -10 + i * 0.05, 0.01), "Pitch not normalized or off");
		}
		return worst;
	};
	double linear = errors(Interpolation::Linear);
	double cubic = errors(Interpolation::Cubic);
	require(cubic < 0.06, "Cubic interpolation error " + std::to_string(cubic));
	require(cubic < linear, "Cubic isn't better than linear on a parabola");

	// teleport
	PlayerTrack teleporter;
	teleporter.name = "teleporter";
	for (int i = 0; i <= 8; i += 4) teleporter.samples.push_back(playing_sample(2000 + i, { float(i), 0, 0 }));
	for (int i = 12; i <= 20; i += 4) {
		auto sample = playing_sample(2000 + i, { 5000.0f + i, 0, 0 });
		sample.interpolation_frame = 1;
		teleporter.samples.push_back(sample);
	}
	auto ticks = fill_ticks(demo, teleporter, 2000, 2020);
	require(ticks.size() == 21, "Teleport range not filled");
	require(ticks[10].origin.x == 8 && ticks[10].interpolated, "Ticks before a teleport not held");
	require(ticks[12].origin.x == 5012, "Teleport target missing");
	require(near(ticks[12].velocity.x, 1 / 0.015, 1e-3), "Velocity spike across the teleport");

	auto dead = teleporter;
	dead.samples[3].life_state = cssdemo::LIFE_DEAD;
	throws([&] { fill_ticks(demo, dead, 2000, 2020); }, "Range over a death accepted");
	throws([&] { fill_ticks(demo, teleporter, 1990, 2010); }, "Range before the first snapshot accepted");

	PlayerTrack jumper;
	jumper.name = "jumper";
	auto air = ballistic(10, dt, 800, { 0, 0, 0 }, { 0, 0, 301 });
	auto ground = playing_sample(3000, { 0, 0, 0 });
	ground.flags = cssdemo::FL_ONGROUND;
	jumper.samples.push_back(ground);
	for (int i = 0; i < 10; i++) {
		auto sample = playing_sample(3001 + i, air[size_t(i)].first);
		if (i >= 5) sample.flags = cssdemo::FL_ANIMDUCKING | cssdemo::FL_DUCKING;
		jumper.samples.push_back(sample);
	}
	ticks = fill_ticks(demo, jumper, 3000, 3010);
	require(ticks[1].buttons == IN_JUMP, "Takeoff has no IN_JUMP");
	require(!(ticks[2].buttons & IN_JUMP), "IN_JUMP after takeoff");
	require((ticks[6].buttons & IN_DUCK) && !(ticks[4].buttons & IN_DUCK), "IN_DUCK not from FL_ANIMDUCKING");

	auto client = synthetic_demo();
	client.kind = cssdemo::DemoKind::Client;
	PlayerTrack local;
	local.name = "local";
	local.is_local = true;
	// two commands in tick 15
	auto add_command = [&](int tick, int number) {
		Command command;
		command.tick = tick;
		command.cmd.command_number = number;
		command.cmd.tick_count = tick;
		command.cmd.buttons = uint32_t(number - 500);
		command.cmd.view_angles = { 10.0f, float(number - 500), 0 };
		client.commands.push_back(command);
	};
	for (int i = 0; i < 20; i++) {
		int ahead = i >= 15;
		auto sample = playing_sample(4000 + i, { float(i), 0, 0 });
		sample.velocity = Vector{ 1 / 0.015f, 0, 0 };
		sample.origin_precise = true;
		sample.tick_base = 9000 + i + ahead;
		local.samples.push_back(sample);
		if (i == 15) add_command(4000 + i - 1, 515);
		add_command(4000 + i - 1, 500 + i + ahead); // recorded before the packet
	}
	local.samples.erase(local.samples.begin() + 10); // a dropped packet
	ticks = fill_ticks(client, local, 4000, 4019);
	for (const auto& tick : ticks) {
		int i = tick.tick - 4000;
		if (i == 10) {
			require(tick.interpolated, "Missing packet tick not interpolated");
			require(near(tick.velocity.x, 1 / 0.015, 1e-3) && !tick.velocity_estimated, "Networked velocity not lerped");
		}
		int number = i + (i >= 15);
		require(tick.from_command, "Tick " + std::to_string(i) + " has no command");
		require(tick.angles.y == float(number) && tick.angles.x == 10, "Command angles not used");
		uint32_t expected = i == 15 ? (15u | 16u) : uint32_t(number);
		require(tick.buttons == expected, "Command buttons wrong at tick " + std::to_string(i));
	}
	std::printf("interpolation: worst %.4f units cubic, %.4f linear on a 4 tick sourcetv parabola\n", cubic, linear);
	return 0;
}

int test_segments()
{
	double dt = 0.015;
	PlayerTrack player;
	player.name = "segments";
	int tick = 100;
	float x = 0;
	auto add = [&](int count, auto change) {
		for (int i = 0; i < count; i++) {
			auto sample = playing_sample(tick++, { x, 0, 0 });
			x += 10;
			change(sample);
			player.samples.push_back(sample);
		}
	};
	add(10, [](Sample&) {});
	int frame = 0;
	add(10, [&](Sample& s) { s.interpolation_frame = ++frame % 4; });
	add(5, [&](Sample& s) { s.interpolation_frame = frame % 4; s.life_state = cssdemo::LIFE_DEAD; });
	player.events.push_back({ PlayerEvent::Kind::Spawn, tick, 0 });
	add(10, [&](Sample& s) { s.interpolation_frame = frame % 4; });
	x += 20000; // teleport
	add(10, [&](Sample& s) { s.interpolation_frame = frame % 4; });
	add(10, [&](Sample& s) { s.interpolation_frame = frame % 4; s.team = 2; });
	tick += 100; // gap
	add(10, [&](Sample& s) { s.interpolation_frame = frame % 4; s.team = 2; });
	x += 40;
	add(10, [&](Sample& s) { s.interpolation_frame = (frame + 1) % 4; s.team = 2; });

	auto segments = find_segments(player, dt);
	std::vector<std::pair<Break, Break>> expected = { { Break::DemoStart, Break::Death },
		{ Break::Spawn, Break::Teleport }, { Break::Teleport, Break::Team }, { Break::Team, Break::Gap },
		{ Break::Gap, Break::Teleport }, { Break::Teleport, Break::DemoEnd } };
	require(segments.size() == expected.size(), "Expected 6 segments, got " + std::to_string(segments.size()));
	for (size_t i = 0; i < expected.size(); i++) {
		require(segments[i].starts == expected[i].first && segments[i].ends == expected[i].second,
			"Segment " + std::to_string(i) + " is " + break_name(segments[i].starts) + " to " + break_name(segments[i].ends));
	}
	require(segments[0].ticks() == 20, "First segment should run through the per-tick teleports");
	require(segments[2].team == 3 && segments[3].team == 2, "Segment teams");
	std::cout << "segments split at death, spawn, teleport, team and gaps\n";
	return 0;
}

std::vector<fs::path> demos_in(const std::vector<std::string>& folders)
{
	std::vector<fs::path> demos;
	for (const auto& folder : folders) {
		if (folder.empty() || !fs::is_directory(folder)) continue;
		for (const auto& entry : fs::directory_iterator(folder)) {
			if (entry.path().extension() == ".dem") demos.push_back(entry.path());
		}
	}
	std::sort(demos.begin(), demos.end());
	demos.erase(std::unique(demos.begin(), demos.end()), demos.end());
	return demos;
}

int test_demos(const std::vector<std::string>& folders)
{
	auto demos = demos_in(folders);
	if (demos.empty()) {
		std::cout << "no demos, skipped\n";
		return SKIPPED;
	}
	auto directory = fs::temp_directory_path() / "demo-to-rec-demos";
	fs::create_directories(directory);
	size_t client = 0, sourcetv = 0, segments = 0, frames = 0, interpolated = 0;
	for (const auto& path : demos) {
		auto demo = load_demo(path);
		require(demo.tick_interval > 0, path.string() + " has no tick interval");
		require(demo.kind != cssdemo::DemoKind::Unknown, path.string() + " has no server info");
		(demo.kind == cssdemo::DemoKind::Client ? client : sourcetv)++;
		bool exported = false;
		for (const auto& [user_id, player] : demo.players) {
			for (const auto& segment : find_segments(player, demo.tick_interval)) {
				auto conversion = convert(demo, player, segment.first_tick, segment.last_tick);
				const auto& replay = conversion.replay;
				require(replay.frames.size() == size_t(segment.ticks()), "Frame count isn't the tick count");
				require(replay.team == segment.team, "Team not from the segment");
				auto back = rec::parse(rec::serialise(replay, false));
				require(same(back, replay), "Converted replay doesn't read back");
				if (player.is_local && segment.ticks() > 100) {
					require(conversion.from_commands * 10 >= conversion.ticks.size() * 9,
						path.filename().string() + ": recording player has few user commands");
				}
				if (!exported && segment.ticks() > 1) {
					auto out = directory / (path.stem().string() + ".rec");
					write_export(out, conversion);
					require(same(rec::load(out), replay), "Exported file reads back differently");
					require(fs::exists(rec::sidecar_path(out)), "No sidecar");
					exported = true;
				}
				segments++;
				frames += replay.frames.size();
				interpolated += conversion.interpolated;
			}
		}
	}
	fs::remove_all(directory);
	std::printf("%zu demos (%zu client, %zu sourcetv), %zu segments, %zu frames, %zu interpolated\n", demos.size(),
		client, sourcetv, segments, frames, interpolated);
	return 0;
}

// replay_viewer bot names are the start of the .rec filename
int test_validate(const std::string& game_dir)
{
	fs::path replays = fs::path(game_dir) / "addons/sourcemod/data/replay_viewer/replays";
	if (game_dir.empty() || !fs::is_directory(replays)) {
		std::cout << "no replay_viewer replays, skipped\n";
		return SKIPPED;
	}
	int validated = 0;
	for (const auto& path : demos_in({ game_dir })) {
		auto demo = load_demo(path);
		for (const auto& [user_id, player] : demo.players) {
			if (!player.name.starts_with("replay_")) continue;
			fs::path reference;
			if (fs::is_directory(replays / demo.map)) {
				for (const auto& entry : fs::directory_iterator(replays / demo.map)) {
					if (entry.path().filename().string().starts_with(player.name)) reference = entry.path();
				}
			}
			if (reference.empty()) continue;
			auto segments = find_segments(player, demo.tick_interval);
			if (segments.empty()) continue;
			auto longest = *std::max_element(segments.begin(), segments.end(),
				[](const Segment& a, const Segment& b) { return a.ticks() < b.ticks(); });
			auto expected = rec::load(reference);
			std::printf("%s: %s ticks %d-%d against %s\n", path.filename().string().c_str(), player.name.c_str(),
				longest.first_tick, longest.last_tick, reference.filename().string().c_str());
			for (int decimate : { 1, 2, 3, 4 }) {
				PlayerTrack thinned = player;
				if (decimate > 1) {
					std::erase_if(thinned.samples, [&](const Sample& s) {
						return s.tick != longest.first_tick && s.tick != longest.last_tick
							&& (s.tick - longest.first_tick) % decimate != 0;
					});
				}
				auto conversion = convert(demo, thinned, longest.first_tick, longest.last_tick);
				std::vector<bool> flags;
				for (const auto& tick : conversion.ticks) flags.push_back(tick.interpolated);
				auto comparison = compare(conversion.replay, flags, expected);
				std::printf("-- snapshot every %d tick%s\n%s", decimate, decimate > 1 ? "s" : "", describe(comparison).c_str());
				if (decimate == 1) {
					require(comparison.snapshots.position.p50 < 0.05, "Snapshot position error too large");
					require(comparison.snapshots.yaw.p99 < 0.2, "Snapshot yaw error too large");
				}
				else {
					require(comparison.interpolated.position.p50 < 0.2, "Interpolated position error too large");
				}
			}
			validated++;
		}
	}
	if (!validated) {
		std::cout << "no demo with a replay_viewer bot and its .rec, skipped\n";
		return SKIPPED;
	}
	return 0;
}

}

int main(int argc, char* argv[])
{
	try {
		require(argc >= 2, "Usage: demo-to-rec-tests <test> [args]");
		std::string test = argv[1];
		std::vector<std::string> args(argv + 2, argv + argc);
		auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
		if (test == "rec_reader") return test_rec_reader(arg(0));
		if (test == "rec_writer") return test_rec_writer(arg(0));
		if (test == "rec_corpus") return test_rec_corpus(arg(0));
		if (test == "interpolation") return test_interpolation();
		if (test == "segments") return test_segments();
		if (test == "demos") return test_demos(args);
		if (test == "validate") return test_validate(arg(0));
		throw std::runtime_error("Unknown test " + test);
	}
	catch (const std::exception& e) {
		std::cerr << "FAILED: " << e.what() << "\n";
		return 1;
	}
}

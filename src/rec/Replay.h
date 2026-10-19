#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

// .rec replays, v2 and v3
namespace rec {

enum BookmarkType {
	BOOKMARK_CHECKPOINT = 1,
	BOOKMARK_ENTER_ZONE = 2,
	BOOKMARK_LEAVE_ZONE = 3,
};

enum Zone {
	ZONE_START = 1,
	ZONE_END = 99,
};

enum Team {
	TEAM_T = 2,
	TEAM_CT = 3,
};

struct Bookmark {
	int tick{};
	int type{};
	int zone{};
	std::string target;
	std::string classname;
};

// state after the tick's command
struct Frame {
	uint32_t buttons{};
	std::array<float, 3> position{};
	std::array<float, 2> angles{}; // pitch, yaw
	std::array<float, 3> velocity{};
	float roll{}; // always 0
};

struct Replay {
	int version = 2;
	int team{};
	int declared_frames{};
	int frame_cells = 10;
	std::vector<Bookmark> bookmarks;
	std::vector<Frame> frames;
	// v3 only, unknown, kept as read
	std::vector<uint32_t> extension;
	std::vector<uint32_t> extra_cells;
};

bool has_bzip2();

Replay parse(std::span<const uint8_t> source);
Replay load(const std::filesystem::path& path);
std::vector<uint8_t> serialise(const Replay& replay, bool compress);
void save(const std::filesystem::path& path, const Replay& replay, bool compress);

Replay make_v2(int team, std::vector<Frame> frames);

struct ExportInfo {
	std::string format = "DEMO_TO_REC_EXPORT";
	std::string map;
	double tick_interval{};
	std::string source;
	int64_t source_tick{};
	// values are json
	std::vector<std::pair<std::string, std::string>> extra;
};

// also writes <stem>.json
void save_export(const std::filesystem::path& path, const Replay& replay, const ExportInfo& info);
std::filesystem::path sidecar_path(const std::filesystem::path& path);

std::string json_string(const std::string& text);
std::string json_number(double value, int digits = 9);

}

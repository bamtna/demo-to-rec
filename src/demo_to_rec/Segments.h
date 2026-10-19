#pragma once
#include "Track.h"
#include <vector>

namespace demo_to_rec {

enum class Break {
	DemoStart,
	DemoEnd,
	Spawn,
	Death, // or spectating
	Team,
	Teleport,
	Gap,
};

const char* break_name(Break reason);

struct Segment {
	int first_tick{};
	int last_tick{};
	size_t first_sample{};
	size_t last_sample{};
	int team{};
	Break starts{};
	Break ends{};

	int ticks() const { return last_tick - first_tick + 1; }
};

struct SegmentOptions {
	int max_gap = 64;
	double max_speed = 3500 * 1.7320508; // sv_maxvelocity
	double teleport_slack = 16;
	double teleport_deviation = 16;
	double max_acceleration = 1000; // u/s per tick
	bool split_on_interpolation_frame = true;
};

// before is the sample before a, if any
std::optional<Break> sample_break(const PlayerTrack& player, const Sample* before, const Sample& a, const Sample& b,
	double tick_interval, const SegmentOptions& options);

std::vector<Segment> find_segments(const PlayerTrack& player, double tick_interval, const SegmentOptions& options = {});

}

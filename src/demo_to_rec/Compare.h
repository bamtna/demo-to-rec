#pragma once
#include "rec/Replay.h"
#include <optional>
#include <string>
#include <vector>

namespace demo_to_rec {

struct ErrorStats {
	size_t count{};
	double mean{};
	double rms{};
	double p50{};
	double p99{};
	double max{};
};

ErrorStats error_stats(std::vector<double> errors);

struct FrameErrors {
	ErrorStats position;
	ErrorStats pitch;
	ErrorStats yaw;
	ErrorStats speed; // horizontal
	ErrorStats velocity_z;
	size_t buttons_equal{};
	size_t duck_equal{};
	size_t jumps{}; // IN_JUMP in the reference
	size_t jumps_found{};
	size_t jumps_extra{};
};

struct Comparison {
	int offset{}; // reference frame = ours + offset
	size_t frames{};
	FrameErrors all;
	FrameErrors snapshots;
	FrameErrors interpolated;
};

// aligned on position unless offset is given
Comparison compare(const rec::Replay& ours, const std::vector<bool>& interpolated, const rec::Replay& reference,
	std::optional<int> offset = std::nullopt);

std::string describe(const Comparison& comparison);

}

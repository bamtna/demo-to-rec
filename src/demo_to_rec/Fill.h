#pragma once
#include "Segments.h"
#include "Track.h"
#include <cstdint>
#include <map>
#include <vector>

namespace demo_to_rec {

constexpr uint32_t IN_JUMP = 1 << 1;
constexpr uint32_t IN_DUCK = 1 << 2;

struct Tick {
	int tick{};
	Vector origin{};
	QAngle angles{}; // pitch in [-90, 90]
	Vector velocity{};
	uint32_t buttons{};
	int flags{};
	bool interpolated{};
	bool velocity_estimated{};
	bool from_command{};
};

enum class Interpolation {
	Linear,
	Cubic,
};

struct FillOptions {
	Interpolation interpolation = Interpolation::Cubic;
	double gravity = 800;
	bool use_commands = true;
	bool derive_duck = true;
	bool derive_jump = true;
	double jump_speed = 200;
	SegmentOptions segments;
};

struct CommandInfo {
	uint32_t buttons{};
	QAngle angles{};
	int number{};
};

// by m_nTickBase, assumes a listen server
std::map<int, CommandInfo> pair_commands(const DemoTracks& demo, const PlayerTrack& player);

float normalize_pitch(float pitch);
// keeps 0-360 if both are
float lerp_angle(float a, float b, double t);
Vector lerp(const Vector& a, const Vector& b, double t);
Vector hermite(const Vector& p1, const Vector& m1, const Vector& p2, const Vector& m2, double t);

// backward difference, minus half a tick of gravity in the air
Vector estimate_velocity(const Vector& previous, const Vector& origin, bool on_ground, double tick_interval, double gravity);

std::vector<Tick> fill_ticks(const DemoTracks& demo, const PlayerTrack& player, int first_tick, int last_tick,
	const FillOptions& options = {});

}

#include "Fill.h"
#include <algorithm>
#include <optional>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace demo_to_rec {

namespace {

Vector operator-(const Vector& a, const Vector& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vector operator+(const Vector& a, const Vector& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vector operator*(const Vector& a, double s) { return { float(a.x * s), float(a.y * s), float(a.z * s) }; }

double shortest(float from, float to)
{
	double delta = std::fmod(double(to) - from, 360.0);
	if (delta > 180) delta -= 360;
	if (delta < -180) delta += 360;
	return delta;
}

float wrap(float angle, bool positive)
{
	double value = std::fmod(double(angle), 360.0);
	if (positive) {
		if (value < 0) value += 360;
		if (value >= 360) value -= 360;
	}
	else {
		if (value > 180) value -= 360;
		if (value <= -180) value += 360;
	}
	return float(value);
}

// slope at sample at of the parabola through from, from + 1, from + 2
template <typename F>
Vector quadratic_slope(const std::vector<Sample>& samples, F value, size_t from, size_t at)
{
	double t0 = samples[from].tick, t1 = samples[from + 1].tick, t2 = samples[from + 2].tick;
	double x = samples[at].tick;
	double l0 = (2 * x - t1 - t2) / ((t0 - t1) * (t0 - t2));
	double l1 = (2 * x - t0 - t2) / ((t1 - t0) * (t1 - t2));
	double l2 = (2 * x - t0 - t1) / ((t2 - t0) * (t2 - t1));
	return value(from) * l0 + value(from + 1) * l1 + value(from + 2) * l2;
}

}

float normalize_pitch(float pitch)
{
	return wrap(pitch, false);
}

float lerp_angle(float a, float b, double t)
{
	return wrap(float(a + shortest(a, b) * t), a >= 0 && b >= 0);
}

Vector lerp(const Vector& a, const Vector& b, double t)
{
	return { float(a.x + (b.x - a.x) * t), float(a.y + (b.y - a.y) * t), float(a.z + (b.z - a.z) * t) };
}

Vector hermite(const Vector& p1, const Vector& m1, const Vector& p2, const Vector& m2, double t)
{
	double t2 = t * t;
	double t3 = t2 * t;
	double h00 = 2 * t3 - 3 * t2 + 1;
	double h10 = t3 - 2 * t2 + t;
	double h01 = -2 * t3 + 3 * t2;
	double h11 = t3 - t2;
	auto axis = [&](float a, float ma, float b, float mb) {
		return float(h00 * a + h10 * ma + h01 * b + h11 * mb);
	};
	return { axis(p1.x, m1.x, p2.x, m2.x), axis(p1.y, m1.y, p2.y, m2.y), axis(p1.z, m1.z, p2.z, m2.z) };
}

Vector estimate_velocity(const Vector& previous, const Vector& origin, bool on_ground, double tick_interval, double gravity)
{
	Vector velocity = (origin - previous) * (1.0 / tick_interval);
	if (!on_ground) velocity.z = float(velocity.z - 0.5 * gravity * tick_interval);
	return velocity;
}

std::map<int, CommandInfo> pair_commands(const DemoTracks& demo, const PlayerTrack& player)
{
	std::map<int, CommandInfo> result;
	if (!player.is_local || demo.commands.empty()) return result;

	std::unordered_map<int, const cssdemo::UserCmd*> by_number;
	for (const auto& command : demo.commands) {
		by_number[command.cmd.command_number] = &command.cmd;
	}

	std::map<int, int> offsets;
	size_t next = 0;
	for (const auto& sample : player.samples) {
		while (next < demo.commands.size() && demo.commands[next].tick < sample.tick) next++;
		if (next == 0 || sample.tick_base <= 0) continue;
		offsets[sample.tick_base - demo.commands[next - 1].cmd.command_number]++;
	}
	if (offsets.empty()) return result;
	int offset = std::max_element(offsets.begin(), offsets.end(),
		[](const auto& a, const auto& b) { return a.second < b.second; })->first;

	auto info_for = [&](int number) -> std::optional<CommandInfo> {
		auto it = by_number.find(number);
		if (it == by_number.end()) return std::nullopt;
		return CommandInfo{ it->second->buttons, it->second->view_angles, number };
	};
	const Sample* previous = nullptr;
	for (const auto& sample : player.samples) {
		int number = sample.tick_base - offset;
		auto info = info_for(number);
		if (!info) {
			previous = nullptr;
			continue;
		}
		if (previous) {
			int ticks = sample.tick - previous->tick;
			int commands = number - (previous->tick_base - offset);
			if (ticks > 1 && commands == ticks) {
				// lost packets
				for (int t = 1; t < ticks; t++) {
					auto missing = info_for(number - ticks + t);
					if (missing) result[previous->tick + t] = *missing;
				}
			}
			else if (commands > 1 && commands < 16) {
				// several commands in one tick
				for (int n = number - commands + 1; n < number; n++) {
					auto skipped = info_for(n);
					if (skipped) info->buttons |= skipped->buttons;
				}
			}
		}
		previous = &sample;
		result[sample.tick] = *info;
	}
	return result;
}

std::vector<Tick> fill_ticks(const DemoTracks& demo, const PlayerTrack& player, int first_tick, int last_tick,
	const FillOptions& options)
{
	if (first_tick > last_tick) throw std::runtime_error("Empty tick range");
	const auto& samples = player.samples;
	double dt = demo.tick_interval;
	auto at_or_before = [&](int tick) {
		auto it = std::upper_bound(samples.begin(), samples.end(), tick,
			[](int t, const Sample& s) { return t < s.tick; });
		return it == samples.begin() ? samples.size() : size_t(it - samples.begin()) - 1;
	};
	size_t first = at_or_before(first_tick);
	size_t last = at_or_before(last_tick);
	if (first == samples.size() || last == samples.size()) {
		throw std::runtime_error(player.name + " has no snapshot at or before tick " + std::to_string(first_tick));
	}
	if (samples[last].tick < last_tick && last + 1 >= samples.size()) {
		throw std::runtime_error(player.name + " has no snapshot after tick " + std::to_string(samples[last].tick));
	}
	if (samples[last].tick < last_tick) last++;

	std::vector<std::optional<Break>> breaks(last + 1);
	for (size_t i = first; i <= last; i++) {
		if (!samples[i].playing()) {
			throw std::runtime_error(player.name + " isn't playing at tick " + std::to_string(samples[i].tick));
		}
		if (i > first) {
			const Sample* before = i > first + 1 && !breaks[i - 1] ? &samples[i - 2] : nullptr;
			breaks[i] = sample_break(player, before, samples[i - 1], samples[i], dt, options.segments);
		}
		if (breaks[i] && *breaks[i] != Break::Teleport) {
			throw std::runtime_error(std::string("Range crosses a ") + break_name(*breaks[i]) + " at tick "
				+ std::to_string(samples[i].tick));
		}
	}
	auto commands = options.use_commands ? pair_commands(demo, player) : std::map<int, CommandInfo>{};

	auto tangent = [&](size_t i, auto value) {
		bool has_previous = i > first && !breaks[i];
		bool has_next = i < last && !breaks[i + 1];
		if (has_previous && has_next) return quadratic_slope(samples, value, i - 1, i);
		if (has_next && i + 2 <= last && !breaks[i + 2]) return quadratic_slope(samples, value, i, i);
		if (has_previous && i >= first + 2 && !breaks[i - 1]) return quadratic_slope(samples, value, i - 2, i);
		if (has_next) return (value(i + 1) - value(i)) * (1.0 / (samples[i + 1].tick - samples[i].tick));
		if (has_previous) return (value(i) - value(i - 1)) * (1.0 / (samples[i].tick - samples[i - 1].tick));
		return Vector{};
	};
	auto origin_of = [&](size_t i) { return samples[i].origin; };

	std::vector<Tick> ticks;
	ticks.reserve(size_t(last_tick - first_tick + 1));
	std::vector<bool> teleported;
	size_t a = first;
	for (int tick = first_tick; tick <= last_tick; tick++) {
		while (a + 1 <= last && samples[a + 1].tick <= tick) a++;
		const Sample& sa = samples[a];
		Tick out;
		out.tick = tick;
		out.flags = sa.flags;
		bool exact = sa.tick == tick;
		bool new_path = exact && a > first && breaks[a].has_value();
		if (exact || a == last || breaks[a + 1]) {
			// snapshot, or holding before a teleport
			out.origin = sa.origin;
			out.angles = sa.angles;
			out.interpolated = !exact;
			if (sa.velocity) out.velocity = *sa.velocity;
		}
		else {
			const Sample& sb = samples[a + 1];
			double t = double(tick - sa.tick) / (sb.tick - sa.tick);
			if (options.interpolation == Interpolation::Cubic) {
				double span = sb.tick - sa.tick;
				out.origin = hermite(sa.origin, tangent(a, origin_of) * span, sb.origin, tangent(a + 1, origin_of) * span, t);
				auto angles_of = [&](size_t i) {
					double yaw = sa.angles.y + shortest(sa.angles.y, samples[i].angles.y);
					return Vector{ normalize_pitch(samples[i].angles.x), float(yaw), 0 };
				};
				Vector angles = hermite(angles_of(a), tangent(a, angles_of) * span, angles_of(a + 1),
					tangent(a + 1, angles_of) * span, t);
				out.angles.x = std::clamp(angles.x, -90.0f, 90.0f);
				out.angles.y = wrap(angles.y, sa.angles.y >= 0 && sb.angles.y >= 0);
			}
			else {
				out.origin = lerp(sa.origin, sb.origin, t);
				out.angles.x = lerp_angle(normalize_pitch(sa.angles.x), normalize_pitch(sb.angles.x), t);
				out.angles.y = lerp_angle(sa.angles.y, sb.angles.y, t);
			}
			out.interpolated = true;
			if (sa.velocity && sb.velocity) out.velocity = lerp(*sa.velocity, *sb.velocity, t);
		}
		out.angles.x = normalize_pitch(out.angles.x);
		out.velocity_estimated = !sa.velocity;

		auto command = commands.find(tick);
		if (command != commands.end()) {
			out.buttons = command->second.buttons;
			out.angles = command->second.angles;
			out.angles.x = normalize_pitch(out.angles.x);
			out.from_command = true;
		}
		else if (options.derive_duck && (sa.flags & cssdemo::FL_ANIMDUCKING)) {
			out.buttons |= IN_DUCK;
		}
		ticks.push_back(out);
		teleported.push_back(new_path);
	}

	for (size_t i = 0; i < ticks.size(); i++) {
		if (!ticks[i].velocity_estimated) continue;
		bool on_ground = (ticks[i].flags & cssdemo::FL_ONGROUND) != 0;
		if (i > 0 && !teleported[i]) {
			ticks[i].velocity = estimate_velocity(ticks[i - 1].origin, ticks[i].origin, on_ground, dt, options.gravity);
		}
		else if (i + 1 < ticks.size() && !teleported[i + 1]) {
			ticks[i].velocity = estimate_velocity(ticks[i].origin, ticks[i + 1].origin, on_ground, dt, options.gravity);
		}
	}
	for (size_t i = 1; options.derive_jump && i < ticks.size(); i++) {
		const Tick& previous = ticks[i - 1];
		Tick& current = ticks[i];
		if (current.from_command || teleported[i]) continue;
		if ((previous.flags & cssdemo::FL_ONGROUND) && !(current.flags & cssdemo::FL_ONGROUND)
			&& current.velocity.z > options.jump_speed) {
			current.buttons |= IN_JUMP;
		}
	}
	return ticks;
}

}

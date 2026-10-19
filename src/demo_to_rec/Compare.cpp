#include "Compare.h"
#include "Fill.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace demo_to_rec {

namespace {

double position_error(const rec::Frame& a, const rec::Frame& b)
{
	double sum = 0;
	for (int i = 0; i < 3; i++) {
		double d = double(a.position[i]) - b.position[i];
		sum += d * d;
	}
	return std::sqrt(sum);
}

double angle_error(float a, float b)
{
	double d = std::fmod(double(a) - b, 360.0);
	if (d > 180) d -= 360;
	if (d < -180) d += 360;
	return std::abs(d);
}

struct Collect {
	std::vector<double> position, pitch, yaw, speed, velocity_z;
	size_t buttons_equal{}, duck_equal{}, jumps{}, jumps_found{}, jumps_extra{};

	void add(const rec::Frame& a, const rec::Frame& b)
	{
		position.push_back(position_error(a, b));
		pitch.push_back(angle_error(a.angles[0], b.angles[0]));
		yaw.push_back(angle_error(a.angles[1], b.angles[1]));
		speed.push_back(std::abs(std::hypot(a.velocity[0], a.velocity[1]) - std::hypot(b.velocity[0], b.velocity[1])));
		velocity_z.push_back(std::abs(double(a.velocity[2]) - b.velocity[2]));
		buttons_equal += a.buttons == b.buttons;
		duck_equal += (a.buttons & IN_DUCK) == (b.buttons & IN_DUCK);
		jumps += (b.buttons & IN_JUMP) != 0;
		jumps_found += (a.buttons & b.buttons & IN_JUMP) != 0;
		jumps_extra += (a.buttons & ~b.buttons & IN_JUMP) != 0;
	}

	FrameErrors finish()
	{
		return { error_stats(std::move(position)), error_stats(std::move(pitch)), error_stats(std::move(yaw)),
			error_stats(std::move(speed)), error_stats(std::move(velocity_z)), buttons_equal, duck_equal, jumps,
			jumps_found, jumps_extra };
	}
};

void print_stats(std::string& out, const char* name, const ErrorStats& s)
{
	char line[200];
	std::snprintf(line, sizeof(line), "  %-10s mean %9.4f  rms %9.4f  p50 %9.4f  p99 %9.4f  max %9.4f\n", name, s.mean,
		s.rms, s.p50, s.p99, s.max);
	out += line;
}

}

ErrorStats error_stats(std::vector<double> errors)
{
	ErrorStats stats;
	stats.count = errors.size();
	if (errors.empty()) return stats;
	std::sort(errors.begin(), errors.end());
	double sum = 0, squares = 0;
	for (double e : errors) {
		sum += e;
		squares += e * e;
	}
	stats.mean = sum / errors.size();
	stats.rms = std::sqrt(squares / errors.size());
	stats.p50 = errors[errors.size() / 2];
	stats.p99 = errors[std::min(errors.size() - 1, size_t(errors.size() * 0.99))];
	stats.max = errors.back();
	return stats;
}

Comparison compare(const rec::Replay& ours, const std::vector<bool>& interpolated, const rec::Replay& reference,
	std::optional<int> offset)
{
	int n = int(ours.frames.size());
	int m = int(reference.frames.size());
	if (!offset) {
		double best = std::numeric_limits<double>::infinity();
		int min_overlap = std::max(1, std::min(n, m) / 2);
		for (int k = -n + min_overlap; k <= m - min_overlap; k++) {
			int first = std::max(0, -k);
			int last = std::min(n, m - k);
			double sum = 0;
			int count = 0;
			int step = std::max(1, (last - first) / 64);
			for (int i = first; i < last; i += step) {
				sum += position_error(ours.frames[i], reference.frames[i + k]);
				count++;
				if (sum / 64 > best) break;
			}
			if (count && sum / count < best) {
				best = sum / count;
				offset = k;
			}
		}
		if (!offset) throw std::runtime_error("Replays don't overlap");
	}

	Comparison result;
	result.offset = *offset;
	Collect all, snapshots, interpolated_frames;
	for (int i = std::max(0, -*offset); i < n && i + *offset < m; i++) {
		const auto& a = ours.frames[i];
		const auto& b = reference.frames[i + *offset];
		all.add(a, b);
		(size_t(i) < interpolated.size() && interpolated[i] ? interpolated_frames : snapshots).add(a, b);
		result.frames++;
	}
	result.all = all.finish();
	result.snapshots = snapshots.finish();
	result.interpolated = interpolated_frames.finish();
	return result;
}

std::string describe(const Comparison& c)
{
	std::string out;
	char line[200];
	std::snprintf(line, sizeof(line), "%zu frames compared, reference frame = frame + %d\n", c.frames, c.offset);
	out += line;
	auto group = [&](const char* name, const FrameErrors& e) {
		if (!e.position.count) return;
		std::snprintf(line, sizeof(line),
			"%s (%zu frames), buttons equal %.2f%%, duck equal %.2f%%, IN_JUMP on %zu of %zu, %zu extra\n", name,
			e.position.count, 100.0 * e.buttons_equal / e.position.count, 100.0 * e.duck_equal / e.position.count,
			e.jumps_found, e.jumps, e.jumps_extra);
		out += line;
		print_stats(out, "position", e.position);
		print_stats(out, "pitch", e.pitch);
		print_stats(out, "yaw", e.yaw);
		print_stats(out, "speed", e.speed);
		print_stats(out, "velocity z", e.velocity_z);
	};
	group("all", c.all);
	group("snapshot", c.snapshots);
	group("interpolated", c.interpolated);
	return out;
}

}

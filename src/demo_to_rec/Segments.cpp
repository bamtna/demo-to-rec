#include "Segments.h"
#include <algorithm>
#include <cmath>

namespace demo_to_rec {

namespace {

double distance(const Vector& a, const Vector& b)
{
	double dx = b.x - a.x;
	double dy = b.y - a.y;
	double dz = b.z - a.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}

const char* break_name(Break reason)
{
	switch (reason) {
	case Break::DemoStart: return "start";
	case Break::DemoEnd: return "end";
	case Break::Spawn: return "spawn";
	case Break::Death: return "death";
	case Break::Team: return "team";
	case Break::Teleport: return "teleport";
	case Break::Gap: return "gap";
	}
	return "?";
}

std::optional<Break> sample_break(const PlayerTrack& player, const Sample* before, const Sample& a, const Sample& b,
	double tick_interval, const SegmentOptions& options)
{
	if (!a.playing() || !b.playing()) return Break::Death;
	if (a.team != b.team) return Break::Team;
	for (const auto& event : player.events) {
		if (event.kind == PlayerEvent::Kind::Spawn && event.tick > a.tick && event.tick <= b.tick) {
			return Break::Spawn;
		}
	}
	int gap = b.tick - a.tick;
	if (gap > options.max_gap) return Break::Gap;
	double seconds = gap * tick_interval;
	if (distance(a.origin, b.origin) > options.max_speed * seconds + options.teleport_slack) return Break::Teleport;
	if (!options.split_on_interpolation_frame || a.interpolation_frame < 0
		|| a.interpolation_frame == b.interpolation_frame) {
		return std::nullopt;
	}
	// some plugins teleport every tick
	Vector predicted;
	double allowed = options.teleport_deviation;
	if (b.velocity) {
		predicted = { float(a.origin.x + b.velocity->x * seconds), float(a.origin.y + b.velocity->y * seconds),
			float(a.origin.z + b.velocity->z * seconds) };
	}
	else if (before && before->tick < a.tick) {
		double scale = double(gap) / (a.tick - before->tick);
		predicted = { float(a.origin.x + (a.origin.x - before->origin.x) * scale),
			float(a.origin.y + (a.origin.y - before->origin.y) * scale),
			float(a.origin.z + (a.origin.z - before->origin.z) * scale) };
		allowed += options.max_acceleration * gap * seconds;
	}
	else {
		predicted = a.origin;
		allowed += options.max_speed * seconds;
	}
	if (distance(predicted, b.origin) > allowed) return Break::Teleport;
	return std::nullopt;
}

std::vector<Segment> find_segments(const PlayerTrack& player, double tick_interval, const SegmentOptions& options)
{
	std::vector<Segment> segments;
	const auto& samples = player.samples;
	std::optional<Segment> open;
	Break pending = Break::DemoStart;

	auto close = [&](Break reason) {
		if (!open) return;
		open->ends = reason;
		segments.push_back(*open);
		open.reset();
	};

	for (size_t i = 0; i < samples.size(); i++) {
		const Sample& sample = samples[i];
		if (open) {
			size_t last = open->last_sample;
			const Sample* before = last > open->first_sample ? &samples[last - 1] : nullptr;
			auto reason = sample_break(player, before, samples[last], sample, tick_interval, options);
			if (reason) {
				close(*reason);
				pending = *reason;
			}
		}
		if (!sample.playing()) {
			pending = Break::Death;
			continue;
		}
		if (!open) {
			int after = i > 0 ? samples[i - 1].tick : sample.tick - 1;
			for (const auto& event : player.events) {
				if (event.kind == PlayerEvent::Kind::Spawn && event.tick > after && event.tick <= sample.tick) {
					pending = Break::Spawn;
				}
			}
			open = Segment{ sample.tick, sample.tick, i, i, sample.team, pending, Break::DemoEnd };
		}
		open->last_tick = sample.tick;
		open->last_sample = i;
	}
	close(Break::DemoEnd);
	return segments;
}

}

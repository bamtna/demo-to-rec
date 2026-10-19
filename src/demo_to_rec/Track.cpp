#include "Track.h"
#include "cssdemo/Game/GameEvents.h"
#include "cssdemo/Game/GameState.h"
#include "cssdemo/Parser/DemoParser.h"
#include <charconv>

namespace demo_to_rec {

namespace {

int entity_int(const cssdemo::Entity* entity, const char* name, int fallback)
{
	if (!entity) return fallback;
	const cssdemo::PropValue* value = entity->find(name);
	return value && !value->empty() ? value->as_int(fallback) : fallback;
}

bool parse_int(const std::string& text, int& value)
{
	auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc{} && end == text.data() + text.size();
}

} // namespace

const PlayerTrack* DemoTracks::find_player(const std::string& who) const
{
	int user_id{};
	if (parse_int(who, user_id)) {
		auto it = players.find(user_id);
		if (it != players.end()) return &it->second;
	}
	for (const auto& [id, player] : players) {
		if (player.guid == who) return &player;
	}
	for (const auto& [id, player] : players) {
		if (player.name == who) return &player;
	}
	// unique prefix, names get cut to 31 chars
	const PlayerTrack* found = nullptr;
	for (const auto& [id, player] : players) {
		if ((player.name.size() >= 4 && who.starts_with(player.name)) || player.name.starts_with(who)) {
			if (found) return nullptr;
			found = &player;
		}
	}
	return found;
}

Collector::Collector(const cssdemo::DemoParser& demo_parser)
	: parser(demo_parser)
{
}

void Collector::on_packet_end(const cssdemo::DemoFrame& frame, const cssdemo::GameState& state)
{
	if (frame.command != cssdemo::DemoCommand::Packet || state.tick == last_tick) {
		return;
	}
	if (state.tick < last_tick) return;
	last_tick = state.tick;
	if (result.first_tick < 0) result.first_tick = state.tick;
	result.last_tick = state.tick;
	result.packet_ticks.push_back(state.tick);

	for (auto& player : cssdemo::player_snapshots(state)) {
		if (player.info.is_hltv || !player.has_entity) continue;
		auto& track = result.players[player.info.user_id];
		track.user_id = player.info.user_id;
		track.entity_index = player.entity_index;
		track.name = player.info.name;
		track.guid = player.info.guid;
		track.fake_player = player.info.fake_player;
		track.is_local = track.is_local || player.is_local;

		const cssdemo::Entity* entity = state.entities.find(player.entity_index);
		Sample sample;
		sample.tick = state.tick;
		sample.origin = player.origin;
		sample.angles = player.eye_angles;
		sample.velocity = player.velocity;
		sample.origin_precise = player.origin_precise;
		sample.flags = player.flags;
		sample.move_type = player.move_type;
		sample.team = player.team;
		sample.life_state = player.life_state;
		sample.health = player.health;
		sample.observer_mode = player.observer_mode;
		sample.interpolation_frame = entity_int(entity, "m_ubInterpolationFrame", -1);
		sample.effects = entity_int(entity, "m_fEffects", 0);
		sample.tick_base = player.tick_base;
		sample.ducked = player.ducked;
		sample.ducking = player.ducking;
		track.samples.push_back(sample);
	}
}

void Collector::on_user_cmd(const cssdemo::UserCmd& cmd)
{
	result.commands.push_back({ last_tick, cmd });
}

void Collector::on_game_event(const cssdemo::GameEvent& event)
{
	PlayerEvent player_event;
	if (event.name == "player_spawn") {
		player_event.kind = PlayerEvent::Kind::Spawn;
	}
	else if (event.name == "player_death") {
		player_event.kind = PlayerEvent::Kind::Death;
	}
	else if (event.name == "player_team") {
		player_event.kind = PlayerEvent::Kind::Team;
		player_event.value = event.get_int("team");
	}
	else {
		return;
	}
	player_event.tick = parser.state().tick;
	auto& track = result.players[event.get_int("userid")];
	track.user_id = event.get_int("userid");
	track.events.push_back(player_event);
}

DemoTracks load_demo(const std::filesystem::path& path)
{
	cssdemo::DemoFile demo(path.string());
	cssdemo::DemoParser parser(demo);
	Collector collector(parser);
	parser.add_listener(collector);
	parser.run();

	DemoTracks tracks = std::move(collector.tracks());
	tracks.path = path.string();
	tracks.header = demo.header();
	tracks.kind = cssdemo::demo_kind(parser.state());
	tracks.map = parser.state().server.map_name.empty() ? demo.header().map_name : parser.state().server.map_name;
	tracks.tick_interval = parser.state().server.tick_interval;
	tracks.parse_errors = parser.errors().size();
	tracks.truncated = demo.truncated();
	std::erase_if(tracks.players, [](const auto& entry) { return entry.second.samples.empty(); });
	return tracks;
}

}

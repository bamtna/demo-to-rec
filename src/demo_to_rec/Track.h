#pragma once
#include "cssdemo/Demo/DemoFile.h"
#include "cssdemo/Game/Players.h"
#include "cssdemo/Game/UserCmd.h"
#include "cssdemo/Parser/DemoListener.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cssdemo {
class DemoParser;
}

namespace demo_to_rec {

using cssdemo::QAngle;
using cssdemo::Vector;

struct Sample {
	int tick{};
	Vector origin{};
	QAngle angles{};
	std::optional<Vector> velocity; // recording player only
	bool origin_precise{};
	int flags{};
	int move_type{};
	int team{};
	int life_state{};
	int health{};
	int observer_mode{};
	int interpolation_frame{ -1 };
	int effects{};
	int tick_base{}; // recording player only
	bool ducked{};
	bool ducking{};

	bool alive() const { return life_state == cssdemo::LIFE_ALIVE; }
	bool playing() const { return alive() && (team == 2 || team == 3) && observer_mode == 0; }
};

struct PlayerEvent {
	enum class Kind { Spawn, Death, Team };
	Kind kind{};
	int tick{};
	int value{}; // team
};

struct Command {
	int tick{}; // last packet's tick
	cssdemo::UserCmd cmd;
};

struct PlayerTrack {
	int user_id{};
	int entity_index{};
	std::string name;
	std::string guid;
	bool fake_player{};
	bool is_local{};
	std::vector<Sample> samples;
	std::vector<PlayerEvent> events;
};

struct DemoTracks {
	std::string path;
	cssdemo::DemoHeader header;
	cssdemo::DemoKind kind{};
	std::string map;
	double tick_interval{};
	int first_tick{ -1 };
	int last_tick{ -1 };
	std::vector<int> packet_ticks;
	std::map<int, PlayerTrack> players;
	std::vector<Command> commands; // client demos only
	size_t parse_errors{};
	bool truncated{};

	const PlayerTrack* find_player(const std::string& who) const;
};

class Collector : public cssdemo::DemoListener {
public:
	explicit Collector(const cssdemo::DemoParser& parser);

	void on_packet_end(const cssdemo::DemoFrame& frame, const cssdemo::GameState& state) override;
	void on_user_cmd(const cssdemo::UserCmd& cmd) override;
	void on_game_event(const cssdemo::GameEvent& event) override;

	DemoTracks& tracks() { return result; }

private:
	const cssdemo::DemoParser& parser;
	DemoTracks result;
	int last_tick = -1;
};

DemoTracks load_demo(const std::filesystem::path& path);

}

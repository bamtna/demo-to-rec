#include "Replay.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

#ifdef REC_HAS_BZIP2
#include <bzlib.h>

extern "C" void bz_internal_error(int)
{
	std::abort();
}
#endif

namespace rec {

namespace {

constexpr size_t bookmark_size = 524;
constexpr size_t text_size = 256;

uint32_t read_u32(std::span<const uint8_t> source, size_t at)
{
	return uint32_t(source[at]) | uint32_t(source[at + 1]) << 8
		| uint32_t(source[at + 2]) << 16 | uint32_t(source[at + 3]) << 24;
}

int read_i32(std::span<const uint8_t> source, size_t at)
{
	return static_cast<int>(read_u32(source, at));
}

float read_f32(std::span<const uint8_t> source, size_t at)
{
	return std::bit_cast<float>(read_u32(source, at));
}

std::string read_text(std::span<const uint8_t> source, size_t at)
{
	std::string text(reinterpret_cast<const char*>(source.data() + at), text_size);
	text.resize(std::strlen(text.c_str()));
	return text;
}

void write_u32(std::vector<uint8_t>& output, uint32_t value)
{
	for (int i = 0; i < 4; i++) {
		output.push_back(uint8_t(value >> (8 * i)));
	}
}

void write_f32(std::vector<uint8_t>& output, float value)
{
	write_u32(output, std::bit_cast<uint32_t>(value));
}

void write_text(std::vector<uint8_t>& output, const std::string& text)
{
	if (text.size() >= text_size || text.find('\0') != std::string::npos) {
		throw std::runtime_error("Bookmark text does not fit");
	}
	output.insert(output.end(), text.begin(), text.end());
	output.resize(output.size() + text_size - text.size());
}

bool finite_movement(const Frame& frame)
{
	for (float value : { frame.position[0], frame.position[1], frame.position[2], frame.angles[0],
		frame.angles[1], frame.velocity[0], frame.velocity[1], frame.velocity[2] }) {
		if (!std::isfinite(value)) return false;
	}
	return true;
}

bool compressed(std::span<const uint8_t> source)
{
	return source.size() >= 3 && source[0] == 'B' && source[1] == 'Z' && source[2] == 'h';
}

#ifdef REC_HAS_BZIP2
// joins concatenated streams like python's bz2
std::vector<uint8_t> decompress(std::span<const uint8_t> source)
{
	constexpr size_t max_size = size_t(1) << 30;
	std::vector<uint8_t> output;
	size_t used = 0;
	while (used < source.size()) {
		bz_stream stream{};
		if (BZ2_bzDecompressInit(&stream, 0, 0) != BZ_OK) {
			throw std::runtime_error("Cannot start bzip2 decompression");
		}
		stream.next_in = reinterpret_cast<char*>(const_cast<uint8_t*>(source.data())) + used;
		stream.avail_in = unsigned(std::min<size_t>(source.size() - used, (std::numeric_limits<unsigned>::max)()));
		int result = BZ_OK;
		while (result == BZ_OK) {
			size_t size = output.size();
			if (size >= max_size) {
				BZ2_bzDecompressEnd(&stream);
				throw std::runtime_error("Decompressed replay is too large");
			}
			output.resize(size + std::max<size_t>(size, 65536));
			stream.next_out = reinterpret_cast<char*>(output.data() + size);
			stream.avail_out = unsigned(output.size() - size);
			result = BZ2_bzDecompress(&stream);
			output.resize(output.size() - stream.avail_out);
			// truncated
			if (result == BZ_OK && !stream.avail_in && stream.avail_out) {
				result = BZ_UNEXPECTED_EOF;
			}
		}
		unsigned consumed = stream.avail_in;
		BZ2_bzDecompressEnd(&stream);
		if (result != BZ_STREAM_END) {
			throw std::runtime_error("Damaged bzip2 replay");
		}
		used = source.size() - consumed;
		if (!compressed(source.subspan(used))) break;
	}
	return output;
}

std::vector<uint8_t> compress_data(std::span<const uint8_t> source)
{
	if (source.size() > (std::numeric_limits<unsigned>::max)() / 2) {
		throw std::runtime_error("Replay is too large to compress");
	}
	std::vector<uint8_t> output(source.size() + source.size() / 100 + 601);
	auto size = unsigned(output.size());
	if (BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(output.data()), &size,
		reinterpret_cast<char*>(const_cast<uint8_t*>(source.data())), unsigned(source.size()), 9, 0, 0) != BZ_OK) {
		throw std::runtime_error("Cannot compress replay");
	}
	output.resize(size);
	return output;
}
#else
std::vector<uint8_t> decompress(std::span<const uint8_t>)
{
	throw std::runtime_error("Built without bzip2, can't read .bz2 replays");
}

std::vector<uint8_t> compress_data(std::span<const uint8_t>)
{
	throw std::runtime_error("Built without bzip2, can't write .bz2 replays");
}
#endif

} // namespace

bool has_bzip2()
{
#ifdef REC_HAS_BZIP2
	return true;
#else
	return false;
#endif
}

Replay parse(std::span<const uint8_t> source)
{
	if (compressed(source)) {
		auto raw = decompress(source);
		if (compressed(raw)) {
			throw std::runtime_error("Replay is compressed twice");
		}
		return parse(raw);
	}
	if (source.size() < 16) {
		throw std::runtime_error("Truncated replay header");
	}
	Replay replay;
	replay.version = read_i32(source, 0);
	replay.team = read_i32(source, 4);
	replay.declared_frames = read_i32(source, 8);
	int marks = read_i32(source, 12);
	if ((replay.version != 2 && replay.version != 3) || replay.declared_frames < 1 || marks < 0) {
		throw std::runtime_error("Invalid replay header");
	}
	int cells = 10;
	int extension = 0;
	size_t offset = 16;
	if (replay.version == 3) {
		if (source.size() < 24) {
			throw std::runtime_error("Truncated v3 header");
		}
		cells = read_i32(source, 16);
		extension = read_i32(source, 20);
		offset = 24;
		if (cells < 10 || cells > 64 || extension < 0) {
			throw std::runtime_error("Invalid replay cell count");
		}
	}
	replay.frame_cells = cells;
	if (size_t(extension) > (source.size() - offset) / 4) {
		throw std::runtime_error("Truncated v3 header");
	}
	for (int i = 0; i < extension; i++) {
		replay.extension.push_back(read_u32(source, offset + size_t(i) * 4));
	}
	offset += size_t(extension) * 4;
	size_t start = offset + size_t(marks) * bookmark_size;
	size_t frame_size = size_t(cells) * 4;
	if (size_t(marks) > source.size() / bookmark_size || start > source.size()
		|| (source.size() - start) % frame_size) {
		throw std::runtime_error("Replay frame data is incomplete or misaligned");
	}
	size_t frames = (source.size() - start) / frame_size;
	if (!frames) {
		throw std::runtime_error("Replay has no frames");
	}
	for (int i = 0; i < marks; i++) {
		size_t at = offset + size_t(i) * bookmark_size;
		Bookmark mark{ read_i32(source, at), read_i32(source, at + 4), read_i32(source, at + 8),
			read_text(source, at + 12), read_text(source, at + 12 + text_size) };
		if (mark.tick < 0 || size_t(mark.tick) > frames) {
			throw std::runtime_error("Bookmark is outside the replay");
		}
		replay.bookmarks.push_back(std::move(mark));
	}
	replay.frames.reserve(frames);
	for (size_t i = 0; i < frames; i++) {
		size_t at = start + i * frame_size;
		Frame frame{ read_u32(source, at),
			{ read_f32(source, at + 4), read_f32(source, at + 8), read_f32(source, at + 12) },
			{ read_f32(source, at + 16), read_f32(source, at + 20) },
			{ read_f32(source, at + 28), read_f32(source, at + 32), read_f32(source, at + 36) },
			read_f32(source, at + 24) };
		if (!finite_movement(frame)) {
			throw std::runtime_error("Nonfinite replay movement");
		}
		replay.frames.push_back(frame);
		for (int cell = 10; cell < cells; cell++) {
			replay.extra_cells.push_back(read_u32(source, at + size_t(cell) * 4));
		}
	}
	return replay;
}

Replay load(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		throw std::runtime_error("Cannot open replay " + path.string());
	}
	std::vector<uint8_t> source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return parse(source);
}

std::vector<uint8_t> serialise(const Replay& replay, bool compress)
{
	if (replay.version != 2 && replay.version != 3) {
		throw std::runtime_error("Replay version must be 2 or 3");
	}
	if (replay.frames.empty() || replay.declared_frames < 1) {
		throw std::runtime_error("Replay has no frames");
	}
	if (replay.frames.size() > size_t((std::numeric_limits<int>::max)())
		|| replay.bookmarks.size() > size_t((std::numeric_limits<int>::max)())) {
		throw std::runtime_error("Replay is too large");
	}
	int cells = replay.frame_cells;
	if (replay.version == 2 && (cells != 10 || !replay.extension.empty() || !replay.extra_cells.empty())) {
		throw std::runtime_error("v2 replays have ten cells and no extension");
	}
	if (cells < 10 || cells > 64 || replay.extra_cells.size() != replay.frames.size() * size_t(cells - 10)
		|| replay.extension.size() > size_t((std::numeric_limits<int>::max)())) {
		throw std::runtime_error("Replay cells do not match the frame size");
	}
	std::vector<uint8_t> output;
	write_u32(output, uint32_t(replay.version));
	write_u32(output, uint32_t(replay.team));
	write_u32(output, uint32_t(replay.declared_frames));
	write_u32(output, uint32_t(replay.bookmarks.size()));
	if (replay.version == 3) {
		write_u32(output, uint32_t(cells));
		write_u32(output, uint32_t(replay.extension.size()));
		for (uint32_t word : replay.extension) {
			write_u32(output, word);
		}
	}
	for (const auto& mark : replay.bookmarks) {
		if (mark.tick < 0 || size_t(mark.tick) > replay.frames.size()) {
			throw std::runtime_error("Bookmark is outside the replay");
		}
		write_u32(output, uint32_t(mark.tick));
		write_u32(output, uint32_t(mark.type));
		write_u32(output, uint32_t(mark.zone));
		write_text(output, mark.target);
		write_text(output, mark.classname);
	}
	output.reserve(output.size() + replay.frames.size() * size_t(cells) * 4);
	auto extra = replay.extra_cells.begin();
	for (const auto& frame : replay.frames) {
		if (!finite_movement(frame)) {
			throw std::runtime_error("Nonfinite replay movement");
		}
		write_u32(output, frame.buttons);
		for (float value : frame.position) {
			write_f32(output, value);
		}
		write_f32(output, frame.angles[0]);
		write_f32(output, frame.angles[1]);
		write_f32(output, frame.roll);
		for (float value : frame.velocity) {
			write_f32(output, value);
		}
		for (int cell = 10; cell < cells; cell++) {
			write_u32(output, *extra++);
		}
	}
	return compress ? compress_data(output) : output;
}

void save(const std::filesystem::path& path, const Replay& replay, bool compress)
{
	auto data = serialise(replay, compress);
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
	if (!file.flush()) {
		throw std::runtime_error("Cannot write replay " + path.string());
	}
}

Replay make_v2(int team, std::vector<Frame> frames)
{
	Replay replay;
	replay.version = 2;
	replay.team = team;
	replay.declared_frames = int(frames.size());
	replay.frame_cells = 10;
	replay.frames = std::move(frames);
	return replay;
}

std::string json_string(const std::string& text)
{
	std::string result = "\"";
	for (unsigned char c : text) {
		if (c == '"' || c == '\\') {
			result += '\\';
			result += char(c);
		}
		else if (c < 0x20) {
			char escape[8];
			std::snprintf(escape, sizeof(escape), "\\u%04x", c);
			result += escape;
		}
		else {
			result += char(c);
		}
	}
	return result + '"';
}

std::string json_number(double value, int digits)
{
	char text[40];
	std::snprintf(text, sizeof(text), "%.*g", digits, value);
	return text;
}

std::filesystem::path sidecar_path(const std::filesystem::path& path)
{
	auto sidecar = path;
	if (sidecar.extension() == ".bz2") {
		sidecar.replace_extension();
	}
	return sidecar.replace_extension(".json");
}

void save_export(const std::filesystem::path& path, const Replay& replay, const ExportInfo& info)
{
	bool compress = path.extension() == ".bz2";
	auto stem = path;
	if (compress) {
		stem.replace_extension();
	}
	if (stem.extension() != ".rec") {
		throw std::runtime_error("Replay must end in .rec or .rec.bz2");
	}
	if (!std::isfinite(info.tick_interval) || info.tick_interval <= 0) {
		throw std::runtime_error("Replay has no tick interval");
	}
	save(path, replay, compress);
	if (serialise(load(path), false) != serialise(replay, false)) {
		throw std::runtime_error("Written replay reads back differently");
	}
	std::ofstream json(sidecar_path(path), std::ios::binary);
	json << "{\n  \"format\": " << json_string(info.format) << ",\n  \"version\": 1,\n"
		<< "  \"map\": " << json_string(info.map) << ",\n"
		<< "  \"tick_interval\": " << json_number(info.tick_interval, 9) << ",\n"
		<< "  \"tick_rate\": " << json_number(1.0 / info.tick_interval, 17) << ",\n"
		<< "  \"source\": " << json_string(info.source) << ",\n"
		<< "  \"source_tick\": " << info.source_tick << ",\n"
		<< "  \"frames\": " << replay.frames.size();
	for (const auto& [key, value] : info.extra) {
		json << ",\n  " << json_string(key) << ": " << value;
	}
	json << "\n}\n";
	if (!json.flush()) {
		throw std::runtime_error("Cannot write replay sidecar");
	}
}

}

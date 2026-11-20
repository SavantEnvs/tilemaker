// File-input (AFL) driver for the real tilemaker CLI.
//
// src/tilemaker.cpp is compiled with -Dmain=tilemaker_main, and this file (compiled
// separately, so its own main is not renamed) calls it with a fixed argument set.
// tilemaker exit()s on several error paths, so this is a process-per-input target,
// not a libFuzzer one.
//
// Usage: tilemaker_fuzz <mode> <input-file>
//   pbf      input is an .osm.pbf: full pipeline (PbfReader -> PbfProcessor -> node/way/
//            relation stores -> Lua -> tile index -> MVT -> .mbtiles) with a fixed config
//   geojson  input is a GeoJSON FeatureCollection used as a layer "source"
//   config   input is the config JSON (validator, Config::readConfig, layer setup,
//            then a run with no OSM input)
//
// Everything tilemaker reads besides the input (config, Lua script) and writes (the
// .mbtiles) lives in a fresh mkdtemp() directory under $TMPDIR (default /tmp), removed
// at exit. The input's bytes alone decide the result.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <boost/filesystem.hpp>

int tilemaker_main(const int argc, const char* argv[]);

static std::string g_dir;

static void cleanup() {
	if (!g_dir.empty()) {
		boost::system::error_code ec;
		boost::filesystem::remove_all(g_dir, ec);
	}
}

static const char* kLua = R"LUA(
function node_function()
	Layer("points", false)
	Attribute("name", Find("name"))
	local pop = Find("population")
	if pop ~= "" then AttributeNumeric("population", tonumber(pop) or 0) end
end

function way_function()
	if IsClosed() and Find("highway") == "" then
		Layer("polygons", true)
		Attribute("name", Find("name"))
		LayerAsCentroid("labels")
	else
		Layer("lines", false)
		Attribute("name", Find("name"))
	end
	while true do
		local rel = NextRelation()
		if not rel then break end
		Attribute("route", FindInRelation("route"))
	end
end

function relation_scan_function()
	local t = Find("type")
	if t == "route" or t == "multipolygon" or t == "boundary" then Accept() end
end

function relation_function()
	local t = Find("type")
	if t == "multipolygon" or t == "boundary" then
		Layer("polygons", true)
	else
		Layer("lines", false)
	end
	Attribute("name", Find("name"))
end
)LUA";

static std::string config_json(const std::string& extraLayer) {
	std::ostringstream c;
	c << "{\"layers\":{"
	     "\"points\":{\"minzoom\":0,\"maxzoom\":10},"
	     "\"lines\":{\"minzoom\":0,\"maxzoom\":10,\"simplify_below\":8,\"simplify_level\":0.0003},"
	     "\"polygons\":{\"minzoom\":0,\"maxzoom\":10,\"simplify_below\":8,\"simplify_level\":0.0003,\"simplify_algorithm\":\"visvalingam\"},"
	     "\"labels\":{\"minzoom\":0,\"maxzoom\":10}"
	  << extraLayer
	  << "},\"settings\":{\"minzoom\":0,\"maxzoom\":10,\"basezoom\":10,\"include_ids\":true,"
	     "\"combine_below\":8,\"name\":\"fuzz\",\"version\":\"1\",\"description\":\"fuzz\","
	     "\"compress\":\"gzip\"}}";
	return c.str();
}

static bool write_file(const std::string& path, const std::string& data) {
	std::ofstream f(path, std::ios::binary);
	f << data;
	return static_cast<bool>(f);
}

int main(int argc, char** argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s pbf|geojson|config <input>\n", argv[0]);
		return 2;
	}
	const std::string mode = argv[1];
	const std::string input = argv[2];

	const char* tmp = getenv("TMPDIR");
	std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/tilemaker-fuzz.XXXXXX";
	std::vector<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');
	if (!mkdtemp(buf.data())) { perror("mkdtemp"); return 2; }
	g_dir = buf.data();
	atexit(cleanup);

	const std::string lua = g_dir + "/process.lua";
	const std::string out = g_dir + "/out.mbtiles";
	std::string config = g_dir + "/config.json";
	if (!write_file(lua, kLua)) return 2;

	std::vector<std::string> args = {"tilemaker"};
	if (mode == "pbf") {
		if (!write_file(config, config_json(""))) return 2;
		args.insert(args.end(), {"--input", input});
	} else if (mode == "geojson") {
		// tilemaker picks the GeoJSON reader by the source's extension.
		std::ifstream in(input, std::ios::binary);
		std::stringstream ss; ss << in.rdbuf();
		const std::string src = g_dir + "/source.geojson";
		if (!write_file(src, ss.str())) return 2;
		if (!write_file(config, config_json(
			",\"external\":{\"minzoom\":0,\"maxzoom\":10,\"source\":\"" + src +
			"\",\"source_columns\":true,\"index\":true,\"simplify_below\":8,\"simplify_level\":0.0003}"))) return 2;
		args.insert(args.end(), {"--bbox", "-180,-85,180,85"});
	} else if (mode == "config") {
		config = input;
		args.insert(args.end(), {"--bbox", "-1,-1,1,1"});
	} else {
		fprintf(stderr, "unknown mode %s\n", mode.c_str());
		return 2;
	}
	args.insert(args.end(), {"--output", out, "--config", config, "--process", lua,
	                         "--threads", "1", "--quiet"});

	std::vector<const char*> av;
	for (const auto& a : args) av.push_back(a.c_str());
	av.push_back(nullptr);
	return tilemaker_main(static_cast<int>(av.size() - 1), av.data());
}

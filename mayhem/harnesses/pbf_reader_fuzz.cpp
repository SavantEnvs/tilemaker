// libFuzzer harness for tilemaker's own .osm.pbf reader (src/pbf_reader.cpp).
// Mirrors test/pbf_reader.test.cpp: read the header blob, then every primitive
// block, and walk its string table, groups, nodes, ways and relations, resolving
// every key/value/role index through the string table the way pbf_processor does.
// Input stays in memory (std::istringstream); no file I/O.
#include <cstdint>
#include <cstddef>
#include <sstream>
#include <string>
#include <exception>
#include "pbf_reader.h"

static volatile size_t sink;

static void touch(const PbfReader::PrimitiveBlock& pb, uint64_t idx) {
	// Index validation is the caller's job (pbf_processor), which the
	// tilemaker-pbf target fuzzes end to end; this target fuzzes the reader alone,
	// so the lookup here is bounds-checked and a bad index rejects the input.
	const auto& s = pb.stringTable.at(idx);
	sink += s.size();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	std::istringstream in(std::string(reinterpret_cast<const char*>(data), size));
	PbfReader::PbfReader reader;
	try {
		PbfReader::BlobHeader bh = reader.readBlobHeader(in);
		protozero::data_view blob = reader.readBlob(bh.datasize, in);
		PbfReader::HeaderBlock header = reader.readHeaderBlock(blob);
		sink += header.optionalFeatures.size() + (header.hasBbox ? 1 : 0);

		while (!in.eof()) {
			bh = reader.readBlobHeader(in);
			if (bh.type == "eof") break;
			blob = reader.readBlob(bh.datasize, in);
			PbfReader::PrimitiveBlock pb = reader.readPrimitiveBlock(blob);
			for (const auto str : pb.stringTable) sink += str.size();
			for (const auto& group : pb.groups()) {
				for (const auto& node : group.nodes()) {
					sink += node.id + node.lon + node.lat;
					for (int i = node.tagStart; i < node.tagEnd; i += 2) {
						touch(pb, group.translateNodeKeyValue(i));
						touch(pb, group.translateNodeKeyValue(i + 1));
					}
				}
				for (const auto& way : group.ways()) {
					sink += way.id + way.refs.size() + way.lats.size() + way.lons.size();
					for (size_t i = 0; i < way.keys.size() && i < way.vals.size(); i++) {
						touch(pb, way.keys[i]);
						touch(pb, way.vals[i]);
					}
				}
				for (const auto& rel : group.relations()) {
					sink += rel.id + rel.memids.size() + rel.types.size();
					for (size_t i = 0; i < rel.keys.size() && i < rel.vals.size(); i++) {
						touch(pb, rel.keys[i]);
						touch(pb, rel.vals[i]);
					}
					for (size_t i = 0; i < rel.roles_sid.size(); i++) touch(pb, rel.roles_sid[i]);
				}
			}
		}
	} catch (const std::exception&) {
		return -1;
	}
	return 0;
}

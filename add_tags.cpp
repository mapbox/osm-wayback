#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <map>
#include <iterator>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#define RAPIDJSON_HAS_STDSTRING 1
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include "rapidjson/document.h"
#include <rapidjson/allocators.h>
#pragma GCC diagnostic pop

#include <osmium/geom/rapid_geojson.hpp>
#include "rocksdb/db.h"

#include "db.hpp"

const int osm_type(const std::string type) {
    if(type == "node") return 1;
    if(type == "way") return 2;
    if(type == "relation") return 3;
    return 1;
}

void write_with_history_tags(TagStore* store, const std::string line) {
    rapidjson::Document geojson_doc;
    if(geojson_doc.Parse<0>(line.c_str()).HasParseError()) {
        std::cerr << "ERROR" << std::endl;
        return;
    }

    if(!geojson_doc.HasMember("properties")) return;
    if(!geojson_doc["properties"]["@id"].IsInt() || !geojson_doc["properties"]["@version"].IsInt() || !geojson_doc["properties"]["@type"].IsString()) return;

    const auto version = geojson_doc["properties"]["@version"].GetInt();
    const auto osm_id = geojson_doc["properties"]["@id"].GetInt();
    const std::string type = geojson_doc["properties"]["@type"].GetString();

    try {
        auto object_history = store->get_object_history(osm_id, osm_type(type), version);
        geojson_doc["properties"].AddMember("@history", object_history, geojson_doc.GetAllocator());

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        geojson_doc.Accept(writer);
        std::cout << buffer.GetString() << std::endl;

    } catch (const std::exception& ex) {
        std::cerr<< ex.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " INDEX_DIR" << std::endl;
        std::exit(1);
    }

    int feature_count = 0;
    std::string index_dir = argv[1];
    TagStore store(index_dir);

    for (std::string line; std::getline(std::cin, line);) {
        write_with_history_tags(&store, line);
        feature_count++;

        if(feature_count%10000==0){
          std::cerr << "\rProcessed: " << (feature_count/1000) << " K features";
        }
    }
    std::cerr << "\rProcessed: " << feature_count << " features successfully, with " << store.not_found_count << " not found versions."<<std::endl;
}

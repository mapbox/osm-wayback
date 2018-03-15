#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#define RAPIDJSON_HAS_STDSTRING 1
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include "rapidjson/document.h"
#pragma GCC diagnostic pop

#include "rocksdb/db.h"
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/cache.h>
#include <rocksdb/write_batch.h>

#include <osmium/osm/types.hpp>
#include <osmium/visitor.hpp>

#include <chrono>
#include "pbf_encoder.hpp"

namespace osmwayback {

const std::string make_lookup(const int64_t osm_id, const int version){
  return std::to_string(osm_id) + "!" + std::to_string(version);
}

class RocksDBStore {
    protected:

    rocksdb::DB* m_db;
    rocksdb::ColumnFamilyHandle* m_cf_ways;
    rocksdb::ColumnFamilyHandle* m_cf_nodes;
    rocksdb::ColumnFamilyHandle* m_cf_relations;
    rocksdb::ColumnFamilyHandle* m_cf_changesets;
    rocksdb::WriteOptions m_write_options;

    void flush_family(const std::string type, rocksdb::ColumnFamilyHandle* cf) {
        const auto start = std::chrono::steady_clock::now();
        std::cerr << "Flushing " << type << std::endl;
        m_db->Flush(rocksdb::FlushOptions{}, cf);
        const auto end = std::chrono::steady_clock::now();
        const auto diff = end - start;
        std::cerr << "Flushed " << type << " in " << std::chrono::duration <double, std::milli> (diff).count() << " ms" << std::endl;
    }

    void compact_family(const std::string type, rocksdb::ColumnFamilyHandle* cf) {
        const auto start = std::chrono::steady_clock::now();
        std::cerr << "Compacting " << type << std::endl;
        m_db->CompactRange(rocksdb::CompactRangeOptions{}, cf, nullptr, nullptr);
        const auto end = std::chrono::steady_clock::now();
        const auto diff = end - start;
        std::cerr << "Compacted " << type << " in " << std::chrono::duration <double, std::milli> (diff).count() << " ms" << std::endl;
    }

    void report_count_stats() {
        uint64_t node_keys{0};
        m_db->GetIntProperty(m_cf_nodes, "rocksdb.estimate-num-keys", &node_keys);
        std::cerr << "Stored ~" << node_keys << "/" << stored_nodes_count << " nodes" << std::endl;

        uint64_t way_keys{0};
        m_db->GetIntProperty(m_cf_ways, "rocksdb.estimate-num-keys", &way_keys);
        std::cerr << "Stored ~" << way_keys << "/" << stored_ways_count << " ways" << std::endl;

        uint64_t relation_keys{0};
        m_db->GetIntProperty(m_cf_relations, "rocksdb.estimate-num-keys", &relation_keys);
        std::cerr << "Stored ~" << relation_keys  << "/" << stored_relations_count << " relations" << std::endl;

        uint64_t changeset_keys{0};
        m_db->GetIntProperty(m_cf_changesets, "rocksdb.estimate-num-keys", &changeset_keys);
        std::cerr << "Stored ~" << changeset_keys  << "/" << stored_changesets_count << " changesets" << std::endl;
    }

    RocksDBStore (const std::string index_dir, const bool create) {
        rocksdb::Options db_options;
        db_options.allow_mmap_writes = false;
        db_options.max_background_flushes = 4;
        db_options.PrepareForBulkLoad();

        db_options.target_file_size_base = 512 * 1024 * 1024;

        m_write_options = rocksdb::WriteOptions();
        m_write_options.disableWAL = true;
        m_write_options.sync = false;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.filter_policy = std::shared_ptr<const rocksdb::FilterPolicy>(rocksdb::NewBloomFilterPolicy(10));
        db_options.table_factory.reset(NewBlockBasedTableFactory(table_options));

        rocksdb::Status s;

        if(create) {
            // always clear out the previous tag index first
            rocksdb::DestroyDB(index_dir, db_options);
            db_options.create_if_missing = true;
            s = rocksdb::DB::Open(db_options, index_dir, &m_db);
            s = m_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "nodes", &m_cf_nodes);
            assert(s.ok());
            s = m_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "ways", &m_cf_ways);
            assert(s.ok());
            s = m_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "relations", &m_cf_relations);
            s = m_db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "changesets", &m_cf_changesets);
            assert(s.ok());
        } else {
            db_options.error_if_exists = false;
            db_options.create_if_missing = false;
            std::cout << "Open without create";
            // open DB with two column families
            std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
            // have to open default column family
            column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));
            // open the new one, too
            column_families.push_back(rocksdb::ColumnFamilyDescriptor( "nodes", rocksdb::ColumnFamilyOptions()));
            column_families.push_back(rocksdb::ColumnFamilyDescriptor( "ways", rocksdb::ColumnFamilyOptions()));
            column_families.push_back(rocksdb::ColumnFamilyDescriptor( "relations", rocksdb::ColumnFamilyOptions()));
            column_families.push_back(rocksdb::ColumnFamilyDescriptor( "changesets", rocksdb::ColumnFamilyOptions()));

            std::vector<rocksdb::ColumnFamilyHandle*> handles;

            s = rocksdb::DB::Open(db_options, index_dir, column_families, &handles, &m_db);
            assert(s.ok());

            m_cf_nodes = handles[1];
            m_cf_ways = handles[2];
            m_cf_relations = handles[3];
            m_cf_changesets = handles[4];
        }

    }

    public:

    unsigned long empty_objects_count{0};
    unsigned long stored_tags_count{0};

    unsigned long stored_nodes_count{0};
    unsigned long stored_ways_count{0};
    unsigned long stored_relations_count{0};
    unsigned long stored_changesets_count{0};

    unsigned long stored_objects_count() {
        return stored_nodes_count + stored_ways_count + stored_relations_count;
    }
};

class TagStore : public RocksDBStore {
    rocksdb::WriteBatch m_buffer_batch;

public:
    TagStore(const std::string index_dir, const bool create) : RocksDBStore(index_dir, create) {
    }
  rocksdb::Status get_tags(const int64_t osm_id, const int osm_type, const int version, std::string* json_value) {
        const auto lookup = make_lookup(osm_id, version);

        if(osm_type== 1) {
            return m_db->Get(rocksdb::ReadOptions(), m_cf_nodes, lookup, json_value);
        } else if (osm_type == 2) {
            return m_db->Get(rocksdb::ReadOptions(), m_cf_ways, lookup, json_value);
        } else {
            return m_db->Get(rocksdb::ReadOptions(), m_cf_relations, lookup, json_value);
        }
    }

    void lookup_nodes(const osmium::Way& way, const int closed_at) {
        const osmium::WayNodeList& node_refs = way.nodes();
        for (const osmium::NodeRef& node_ref : node_refs) {
            auto node_id = node_ref.ref();

            // Find all the versions
            int node_version{1};
            for(int v = 1; v < 1000; v++) {
                std::string node_json;

                auto read_status = m_db->Get(rocksdb::ReadOptions(), m_cf_nodes, make_lookup(node_id, v), &node_json);

                if(read_status.ok()) {
                    rapidjson::Document node_doc;
                    if(!node_doc.Parse<0>(node_json.c_str()).HasParseError()) {
                        if(node_doc.HasMember("@timestamp")) {
                            auto ts = node_doc["@timestamp"].GetInt();
                            if (ts > closed_at) {
                                break;
                            } else {
                                node_version = v;
                            }
                        }
                    }
                }
            }

            std::cout << "Found real node version " << node_version << std::endl;
        }
    }

    void store_tags(const osmium::Way& way) {
        // Add closed at if found
        std::string changeset_json;
        auto read_status = m_db->Get(rocksdb::ReadOptions(), m_cf_changesets, std::to_string(way.changeset()), &changeset_json);
        if (read_status.ok()) {
            rapidjson::Document changeset_doc;
            std::cout << changeset_json << std::endl;
            if(!changeset_doc.Parse<0>(changeset_json.c_str()).HasParseError()) {
                if(changeset_doc.HasMember("@closed_at")) {
                    auto closed_at = changeset_doc["@closed_at"].GetInt();
                    lookup_nodes(way, closed_at);
                }
            }
        }

        if(store_tags(way, m_cf_ways)) {
            stored_ways_count++;
        }
    }

    void store_tags(const osmium::Node& node) {
        if(store_tags(node, m_cf_nodes)) {
            stored_nodes_count++;
        }
    }

    void store_tags(const osmium::Relation& relation) {
        if(store_tags(relation, m_cf_relations)) {
            stored_relations_count++;
        }
    }

    bool store_changeset(const osmium::Changeset& changeset) {
        auto lookup = std::to_string(changeset.id());
        auto value = osmwayback::encode_changeset(changeset);
        rocksdb::Status stat = m_buffer_batch.Put(m_cf_changesets, lookup, value);

        stored_changesets_count++;
        if (m_buffer_batch.Count() > 1000) {
            m_db->Write(m_write_options, &m_buffer_batch);
            m_buffer_batch.Clear();
        }

        if (stored_changesets_count != 0 && (stored_changesets_count % 1000000) == 0) {
            flush_family("changesets", m_cf_changesets);
            report_count_stats();
        }
        return true;
    }

    bool store_tags(const osmium::OSMObject& object, rocksdb::ColumnFamilyHandle* cf) {

        const auto lookup = make_lookup(object.id(), object.version());
        if (object.tags().empty()) {
            empty_objects_count++;
            return false;
        }

        rapidjson::Document doc;
        doc.SetObject();

        rapidjson::Document::AllocatorType& a = doc.GetAllocator();

        doc.AddMember("@timestamp", static_cast<int>(object.timestamp().seconds_since_epoch()), a);
        if (object.deleted()){
          doc.AddMember("@deleted", object.deleted(), a);
        }
        doc.AddMember("@visible", object.visible(), a);
        doc.AddMember("@user", std::string{object.user()}, a);
        doc.AddMember("@uid", object.uid(), a);
        doc.AddMember("@changeset", object.changeset(), a);
        doc.AddMember("@version", object.version(), a);

        //Ignore trying to store geometries, but if we could scale that, it'd be awesome.
        const osmium::TagList& tags = object.tags();

        rapidjson::Value object_tags(rapidjson::kObjectType);
        for (const osmium::Tag& tag : tags) {
            rapidjson::Value key(rapidjson::StringRef(tag.key()));
            rapidjson::Value value(rapidjson::StringRef(tag.value()));

            object_tags.AddMember(key, value, a);
            stored_tags_count++;
        }

        doc.AddMember("@tags", object_tags, a);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        rocksdb::Status stat = m_buffer_batch.Put(cf, lookup, buffer.GetString());

        if (m_buffer_batch.Count() > 500) {
            m_db->Write(m_write_options, &m_buffer_batch);
            m_buffer_batch.Clear();
        }

        if (stored_nodes_count != 0 && (stored_nodes_count % 2000000) == 0) {
            flush_family("nodes", m_cf_nodes);
            report_count_stats();
        }
        if (stored_ways_count != 0 && (stored_ways_count % 1000000) == 0) {
            flush_family("ways", m_cf_ways);
            report_count_stats();
        }
        if (stored_relations_count != 0 && (stored_relations_count % 1000000) == 0) {
            flush_family("relations", m_cf_relations);
            report_count_stats();
        }
        return true;
    }

    void flush() {
        m_db->Write(m_write_options, &m_buffer_batch);
        m_buffer_batch.Clear();

        flush_family("nodes", m_cf_nodes);
        flush_family("ways", m_cf_ways);
        flush_family("relations", m_cf_relations);
        flush_family("changesets", m_cf_changesets);

        compact_family("nodes", m_cf_nodes);
        compact_family("ways", m_cf_ways);
        compact_family("relations", m_cf_relations);
        compact_family("changesets", m_cf_changesets);

        report_count_stats();
    }
};

}

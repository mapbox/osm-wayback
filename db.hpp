#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#define RAPIDJSON_HAS_STDSTRING 1
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include "rapidjson/document.h"
#include <rapidjson/allocators.h>
#pragma GCC diagnostic pop

#include "rocksdb/db.h"
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>
#include "rocksdb/cache.h"

typedef std::map<std::string,std::string> StringStringMap;

//https://stackoverflow.com/questions/8473009/how-to-efficiently-compare-two-maps-of-strings-in-c
struct Pair_First_Equal {
    template <typename Pair>
    bool operator() (Pair const &lhs, Pair const &rhs) const {
        return lhs.first == rhs.first;
    }
};

template <typename Map>
bool map_compare (Map const &lhs, Map const &rhs) {
    // No predicate needed because there is operator== for pairs already.
    return lhs.size() == rhs.size()
        && std::equal(lhs.begin(), lhs.end(),
                      rhs.begin());
}

const std::string make_lookup(const int osm_id, const int type, const int version){
  return std::to_string(osm_id) + "!" + std::to_string(version) + "!" + std::to_string(type);
}

class TagStore {
    rocksdb::DB* m_db;


public:
    long int empty_objects_count{0};
    long int stored_tags_count{0};
    long int stored_objects_count{0};
    long int found_count{0};
    long int not_found_count{0};
    long int parse_error_count{0};

    TagStore(const std::string index_dir) {
        rocksdb::Options options;
        options.create_if_missing = true;
        options.allow_mmap_writes = true;

        rocksdb::BlockBasedTableOptions table_opts;
        table_opts.filter_policy = std::shared_ptr<const rocksdb::FilterPolicy>(rocksdb::NewBloomFilterPolicy(10));
        options.table_factory.reset(NewBlockBasedTableFactory(table_opts));

        rocksdb::DB::Open(options, index_dir, &m_db);
    }

    rapidjson::Value get_object_history(const int osm_id, const int type, const int version) {
        rapidjson::Value object_history(rapidjson::kArrayType);
        rapidjson::Document stored_doc;

        std::vector<std::map<std::string, std::string>> tag_history;

        int hist_it_idx = 0; // Can't trust the versions because they may not be contiguous
        for(int v = 1; v < version+1; v++) { // Going up to current version so that history is complete
            const auto lookup = make_lookup(osm_id, type, v);
            std::string json;
            rocksdb::Status s = m_db->Get(rocksdb::ReadOptions(), lookup, &json);

            if (!s.ok()) {
                not_found_count++;
                continue;
            }

            found_count++;
            rapidjson::ParseResult ok = stored_doc.Parse(json.c_str());
            if (!ok) {
                parse_error_count++;
                fprintf(std::cerr, "JSON parse error: %s (%u)", rapidjson::GetParseError_En(ok.Code()), ok.Offset());
                continue;
            }

            std::map<std::string,std::string> version_tags{};
            for (rapidjson::Value::ConstMemberIterator it = stored_doc["@tags"].MemberBegin(); it != stored_doc["@tags"].MemberEnd(); it++) {
                version_tags.insert(std::make_pair( it->name.GetString(), it->value.GetString()));
            }
            //TODO: We need to be careful ^ about order? How does order matter here?
            tag_history.push_back(version_tags);

            // The first version contains all the initial tags
            if (hist_it_idx == 0) {
                //TODO: Is this the most efficient way? we just need to rename it from @tags to @new_tags
                stored_doc.AddMember("@new_tags", stored_doc["@tags"], stored_doc.GetAllocator());
                stored_doc.RemoveMember("@tags");

            } else {
                // Both v and v-1 tags are mapped in memory, so we can do map comparison

                // Check if they are exactly the same:
                const auto tagsNotChanged = map_compare( tag_history[hist_it_idx-1], tag_history[hist_it_idx] );
                if (!tagsNotChanged) {
                    // There has been one of 3 changes:
                    // 1. New tags
                    // 2. Mod tags
                    // 3. Del tags
                    StringStringMap::iterator pos;
                    rapidjson::Value mod_tags(rapidjson::kObjectType);
                    rapidjson::Value new_tags(rapidjson::kObjectType);

                    for (pos = tag_history[hist_it_idx].begin(); pos != tag_history[hist_it_idx].end(); ++pos) {

                        //First, check if the current key exists in the previous entry:
                        std::map<std::string,std::string>::iterator search = tag_history[hist_it_idx-1].find(pos->first);

                        if (search == tag_history[hist_it_idx-1].end()) {
                            //Not found, so it's a new tag
                            rapidjson::Value new_key(rapidjson::StringRef(pos->first));
                            rapidjson::Value new_val(rapidjson::StringRef(pos->second));

                        } else {
                            //It exists, check if it's the same, if not, it's a modified tag
                            if( pos->second != search->second) {
                                rapidjson::Value prev_val(rapidjson::StringRef(search->second));

                                rapidjson::Value new_val(rapidjson::StringRef(pos->second));
                                rapidjson::Value key(rapidjson::StringRef(pos->first));

                                rapidjson::Value modified_tag(rapidjson::kArrayType);
                                modified_tag.PushBack(prev_val, stored_doc.GetAllocator());
                                modified_tag.PushBack(new_val, stored_doc.GetAllocator());
                                mod_tags.AddMember(key, modified_tag, stored_doc.GetAllocator());
                            }
                            //We've dealt with it, so now erase it from the previous entry
                            tag_history[hist_it_idx-1].erase(search->first);
                        }

                    }

                    //If we have modified or new tags, add them
                    if (mod_tags.ObjectEmpty()==false){
                        stored_doc.AddMember("@mod_tags", mod_tags, stored_doc.GetAllocator());
                    }
                    if (new_tags.ObjectEmpty()==false){
                        stored_doc.AddMember("@new_tags", new_tags, stored_doc.GetAllocator());
                    }

                    // Since we've deleted keys from above, if there are any values left in the map, then we save those as deleted keys
                    const auto deletedKeysLeft = tag_history[hist_it_idx-1].empty() == false;
                    if (deletedKeysLeft) {
                        rapidjson::Value del_tags(rapidjson::kObjectType);
                        for (pos = tag_history[hist_it_idx-1].begin(); pos != tag_history[hist_it_idx-1].end(); ++pos) {
                            rapidjson::Value del_key(rapidjson::StringRef(pos->first));
                            rapidjson::Value del_val(rapidjson::StringRef(pos->second));
                            del_tags.AddMember(del_key, del_val, stored_doc.GetAllocator());
                        }
                        stored_doc.AddMember("@del_tags", del_tags, stored_doc.GetAllocator());
                    }
                }
            }

            hist_it_idx++;
            //Save the new object into the object history
            object_history.PushBack(stored_doc, stored_doc.GetAllocator());
        }

        return object_history;
    }

    void store_tags(const std::string lookup, const osmium::OSMObject& object) {
        if (object.tags().empty()) {
            empty_objects_count++;
            return;
        }

        // This use the memory pool allocator since we construct a new document each time
        rapidjson::Document doc;
        doc.SetObject();

        rapidjson::Document::AllocatorType& a = doc.GetAllocator();

        doc.AddMember("@timestamp", object.timestamp().to_iso(), a); //ISO is helpful for debugging, but we should leave it
        doc.AddMember("@deleted", object.deleted(), a);
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

            object_tags.AddMember(key, value, doc.GetAllocator());
            stored_tags_count++;
        }

        doc.AddMember("@tags", object_tags, a);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        rocksdb::Status stat = m_db->Put(rocksdb::WriteOptions(), lookup, buffer.GetString());
        stored_objects_count++;
    }
};

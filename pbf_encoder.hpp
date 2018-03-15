#pragma once

#include <protozero/pbf_writer.hpp>
#include <protozero/pbf_reader.hpp>

#include <osmium/osm/types.hpp>
#include <map>

namespace osmwayback {
    struct Changeset {
      uint64_t created_at;
      uint64_t closed_at;
      std::map<std::string, std::string> tags;
    };

    const std::string encode_changeset(const osmium::Changeset& changeset) {
      std::string data;
      protozero::pbf_writer encoder(data);

      encoder.add_fixed64(1, static_cast<int>(changeset.created_at().seconds_since_epoch()));
      encoder.add_fixed64(2, static_cast<int>(changeset.closed_at().seconds_since_epoch()));

      const osmium::TagList& tags = changeset.tags();
      for (const osmium::Tag& tag : tags) {
        encoder.add_string(17, tag.key());
        encoder.add_string(17, tag.value());
      }

      return data;
    }

    const Changeset decode_changeset(std::string data) {
      protozero::pbf_reader message(data);
      Changeset changeset{};
      changeset.tags = std::map<std::string, std::string>{};

      std::string previous_key{};
        while (message.next()) {
            switch (message.tag()) {
                case 1:
                    changeset.created_at = message.get_fixed64();
                    break;
                case 2:
                    changeset.closed_at = message.get_fixed64();
                    break;
                case 17:
                    changeset.closed_at = message.get_fixed64();
                    if (previous_key.empty()) {
                        previous_key = message.get_string();
                    } else {
                        changeset.tags[previous_key] = message.get_string();
                        previous_key = "";
                    }
                    break;
                default:
                    message.skip();
            }
        }

     return changeset;
    }
}



#include <cstdlib>  // for std::exit
#include <cstring>  // for std::strncmp
#include <iostream> // for std::cout, std::cerr
#include <sstream>
#include <chrono>

#include <osmium/io/any_input.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/handler.hpp>
#include <osmium/visitor.hpp>

#include "db.hpp"

class ChangesetHandler : public osmium::handler::Handler {
    osmwayback::TagStore* m_store;

public:
    ChangesetHandler(osmwayback::TagStore* store) : m_store(store) {}
    void changeset(const osmium::Changeset& changeset) {
        m_store->store_changeset(changeset);
    }
};

std::atomic_bool stop_progress{false};

void report_progress(const osmwayback::TagStore* store) {
    unsigned long last_changesets_count{0};
    auto start = std::chrono::steady_clock::now();

    while(true) {
        if(stop_progress) {
            auto end = std::chrono::steady_clock::now();
            auto diff = end - start;

            std::cerr << "Processed " << last_changesets_count << " changesets in " << std::chrono::duration <double, std::milli> (diff).count() << " ms" << std::endl;
            break;
        }

        auto diff_changesets_count = store->stored_changesets_count - last_changesets_count;
        std::cerr << "Processing " << diff_changesets_count << " changesets/s" << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        last_changesets_count += diff_changesets_count;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " INDEX_DIR CHANGESET_FILE" << std::endl;
        std::exit(1);
    }

    std::string index_dir = argv[1];
    std::string changeset_filename = argv[2];

    osmwayback::TagStore store(index_dir, true);
    ChangesetHandler handler(&store);

    std::thread t_progress(report_progress, &store);

    osmium::io::Reader reader{changeset_filename, osmium::osm_entity_bits::changeset};
    osmium::apply(reader, handler);

    stop_progress = true;
    t_progress.join();
    store.flush();
}

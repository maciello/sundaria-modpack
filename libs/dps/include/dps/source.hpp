#pragma once
// Where the tables come from (#118). Offline: FileSource (a tables file written from the extracted pak data).
// In game: a Source that reads the game's loaded tables (features/inventory/shared).
#include "tables.hpp"
#include <memory>
#include <string>

namespace dps {
    class Source {
    public:
        virtual ~Source() = default;
        virtual bool Load(Tables& out, std::string& error) = 0;   // false: error says what is missing
    };
    // Text tables file (format: tables_io.cpp). Path is used as given.
    std::unique_ptr<Source> FileSource(std::string path);
}

// SPDX-License-Identifier: GPL-3.0-only
#include "ServersDat.h"

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <sstream>

#include "FileSystem.h"

namespace ServersDat {

std::unique_ptr<nbt::tag_compound> parseServersDat(const QString& filename)
{
    try {
        auto res = FS::read(filename);
        if (!res) {
            return nullptr;
        }
        const auto& input = res.value();
        std::istringstream foo(std::string(input.constData(), input.size()));
        auto pair = nbt::io::read_compound(foo);

        if (pair.first != "") {
            return nullptr;
        }

        if (pair.second == nullptr) {
            return nullptr;
        }

        return std::move(pair.second);
    } catch (...) {
        return nullptr;
    }
}

bool serializeServerDat(const QString& filename, nbt::tag_compound* levelInfo)
{
    try {
        if (!FS::ensureFilePathExists(filename)) {
            return false;
        }
        std::ostringstream s;
        nbt::io::write_tag("", *levelInfo, s);
        QByteArray val(s.str().data(), (int)s.str().size());
        auto res = FS::write(filename, val);
        return res.has_value();
    } catch (...) {
        return false;
    }
}

}  // namespace ServersDat

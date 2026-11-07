// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <string>

#include <tag_compound.h>
#include <tag_primitive.h>
#include <tag_string.h>

// A Minecraft servers.dat (the multiplayer server list), shared by the Servers page and the MCP server ops.
// Moved here unchanged from ui/pages/instance/ServersPage.cpp.
namespace ServersDat {

struct Server {
    // Types
    enum class AcceptsTextures : int { ASK = 0, ALWAYS = 1, NEVER = 2 };

    // Methods
    Server() { m_name = QObject::tr("Minecraft Server"); }
    Server(const QString& name, const QString& address)
    {
        m_name = name;
        m_address = address;
    }
    Server(nbt::tag_compound& server)
    {
        std::string addressStr(server["ip"]);
        m_address = QString::fromUtf8(addressStr.c_str());

        std::string nameStr(server["name"]);
        m_name = QString::fromUtf8(nameStr.c_str());

        if (server["icon"]) {
            std::string base64str(server["icon"]);
            m_icon = QByteArray::fromBase64(base64str.c_str());
        }

        if (server.has_key("acceptTextures", nbt::tag_type::Byte)) {
            bool value = server["acceptTextures"].as<nbt::tag_byte>().get();
            if (value) {
                m_acceptsTextures = AcceptsTextures::ALWAYS;
            } else {
                m_acceptsTextures = AcceptsTextures::NEVER;
            }
        }
    }

    void serialize(nbt::tag_compound& server)
    {
        server.insert("name", m_name.trimmed().toUtf8().toStdString());
        server.insert("ip", m_address.trimmed().toUtf8().toStdString());
        if (m_icon.size()) {
            server.insert("icon", m_icon.toBase64().toStdString());
        }
        if (m_acceptsTextures != AcceptsTextures::ASK) {
            server.insert("acceptTextures", nbt::tag_byte(m_acceptsTextures == AcceptsTextures::ALWAYS));
        }
    }

    // Data - persistent and user changeable
    QString m_name;
    QString m_address;
    AcceptsTextures m_acceptsTextures = AcceptsTextures::ASK;

    // Data - persistent and automatically updated
    QByteArray m_icon;

    // Data - temporary
    std::optional<int> m_currentPlayers;  // nullopt if not calculated/calculating
};

// The root compound of a servers.dat, or nullptr when it is missing or unreadable
std::unique_ptr<nbt::tag_compound> parseServersDat(const QString& filename);
bool serializeServerDat(const QString& filename, nbt::tag_compound* levelInfo);

}  // namespace ServersDat

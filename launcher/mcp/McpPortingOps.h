// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "Result.h"

// Help for porting a mod to another Minecraft version or loader, for the user's own play only. The guide resource
// (launcher://guide/mod-porting) carries the terms; porting.analyze measures a jar so the agent can judge the work.
namespace Mcp {

class ToolRegistry;

// What the class files of a mod reference: internal class names (net/minecraft/...) taken from each class's constant pool
struct ClassReferences {
    int majorVersion = 0;
    QSet<QString> classes;
};
Result<ClassReferences> readClassReferences(const QByteArray& classFile);

// Java class-file major version to the Java release that produces it (52 -> 8, 65 -> 21, 69 -> 25)
int javaRelease(int classMajorVersion);

// Everything porting.analyze reports about one jar
Result<QJsonObject> analyzeJar(const QString& path);

// NeoForge publishes one porting primer per Minecraft version step; these are the ones between two versions, oldest first
QStringList primersBetween(const QString& fromVersion, const QString& toVersion);

void registerPortingOps(ToolRegistry& ops);

}  // namespace Mcp

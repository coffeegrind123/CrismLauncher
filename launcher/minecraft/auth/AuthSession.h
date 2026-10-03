#pragma once

#include <QString>
#include <memory>

#include "LaunchMode.h"

class MinecraftAccount;

struct AuthSession {
    bool MakeOffline(QString offline_playername);
    void MakeDemo(QString name, QString uuid);

    QString serializeUserProperties();

    // combined session ID
    QString session;
    // volatile auth token
    QString access_token;
    // profile name
    QString player_name;
    // profile ID
    QString uuid;
    // 'msa' or 'offline', depending on account type
    QString user_type;
    // the actual launch mode for this session
    LaunchMode launchMode;

    // authlib-injector: API root of the account's Yggdrasil server (empty for other accounts), its
    // base64 metadata, and the agent jar resolved during launch
    QString authlib_injector_url;
    QString authlib_injector_metadata;
    QString authlib_injector_jar;

    //! Offline and demo launches don't talk to the server, so they run without the agent
    bool usesAuthlibInjector() const { return !authlib_injector_url.isEmpty() && launchMode == LaunchMode::Normal; }
};

using AuthSessionPtr = std::shared_ptr<AuthSession>;

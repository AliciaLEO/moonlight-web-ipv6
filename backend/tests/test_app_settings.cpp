/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/AppSettings.h"

#include "mw/native/VideoPipeline.h"

#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

void run_app_settings_tests()
{
    SECTION("AppSettings");

    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    AppSettings s;
    // Redirect persistence to a hermetic temp file (m_FilePath is public).
    s.m_FilePath = tmp.path() + "/settings.json";
    CHECK_EQ(s.filePath(), s.m_FilePath);

    // Fallbacks when the file is empty / key absent.
    CHECK_EQ(s.httpPort(8080), quint16(8080));
    CHECK_EQ(s.httpsPort(8443), quint16(8443));

    // Round-trips: numbers and ports.
    s.setHttpPort(80);
    CHECK_EQ(s.httpPort(1), quint16(80));
    s.setHttpsPort(443);
    CHECK_EQ(s.httpsPort(1), quint16(443));
    s.setStreamBitrate(30000);
    CHECK_EQ(s.streamBitrate(), 30000);
    s.setStreamHeight(1440);
    CHECK_EQ(s.streamHeight(), 1440);
    s.setStreamFps(120);
    CHECK_EQ(s.streamFps(), 120);

    // Video codec enum <-> string mapping.
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::H264), QString("h264"));
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::HEVC), QString("hevc"));
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::AV1), QString("av1"));
    CHECK_EQ(AppSettings::videoCodecToString(VideoCodec::Auto), QString("auto"));
    CHECK(AppSettings::videoCodecFromString("av1") == VideoCodec::AV1);
    CHECK(AppSettings::videoCodecFromString("garbage") == VideoCodec::Auto);
    s.setVideoCodec(VideoCodec::HEVC);
    CHECK(s.videoCodec() == VideoCodec::HEVC);

    // Boolean round-trips.
    s.setGamingMode(true);
    CHECK_EQ(s.gamingMode(), true);
    s.setShowPerformanceStats(true);
    CHECK_EQ(s.showPerformanceStats(), true);
    s.setHdrEnabled(true);
    CHECK_EQ(s.hdrEnabled(), true);
    s.setChroma444Enabled(true);
    CHECK_EQ(s.chroma444Enabled(), true);
    s.setUpnpEnabled(false);
    CHECK_EQ(s.upnpEnabled(), false);
    s.setAutoIpDetection(false);
    CHECK_EQ(s.autoIpDetection(), false);
    s.setInternetAccessEnabled(true);
    CHECK_EQ(s.internetAccessEnabled(), true);
    // A LAN-only instance reads it as off, whatever the file says.
    qputenv("MW_LAN_ONLY", "1");
    CHECK_EQ(s.internetAccessEnabled(), false);
    qunsetenv("MW_LAN_ONLY");
    CHECK_EQ(s.internetAccessEnabled(), true);
    s.setCertAuthEnabled(true);
    CHECK_EQ(s.certAuthEnabled(), true);

    // String round-trips.
    s.setStreamAspect("21:9");
    CHECK_EQ(s.streamAspect(), QString("21:9"));
    s.setVideoEnhancement("on");
    CHECK_EQ(s.videoEnhancement(), QString("on"));
    s.setVideoEnhancementAlgo("fsr1");
    CHECK_EQ(s.videoEnhancementAlgo(), QString("fsr1"));
    s.setStunServer("stun:example.org:3478");
    CHECK_EQ(s.stunServer(), QString("stun:example.org:3478"));
    s.setTransportMode("webrtc-dc-udp");
    CHECK_EQ(s.transportMode(), QString("webrtc-dc-udp"));

    // The picture chain of a native session (admin → Advanced): auto until
    // the admin picks one — Windows' names and Linux's, in any case, nothing
    // else. A refused value leaves the stored one in place, and a file edited
    // by hand to something else reads as the default, not as a chain that
    // does not exist.
    CHECK_EQ(s.nativeVideoPipeline(), QString("auto"));
    CHECK(s.setNativeVideoPipeline("Vulkan"));
    CHECK_EQ(s.nativeVideoPipeline(), QString("vulkan"));
    CHECK(s.setNativeVideoPipeline("vaapi"));
    CHECK_EQ(s.nativeVideoPipeline(), QString("vaapi"));
    CHECK(s.setNativeVideoPipeline("d3d12"));
    CHECK_EQ(s.nativeVideoPipeline(), QString("d3d12"));
    CHECK(s.setNativeVideoPipeline(" D3D11 "));
    CHECK_EQ(s.nativeVideoPipeline(), QString("d3d11"));
    CHECK(!s.setNativeVideoPipeline("d3d13"));
    CHECK(!s.setNativeVideoPipeline(""));
    CHECK_EQ(s.nativeVideoPipeline(), QString("d3d11"));
    {
        QFile f(s.m_FilePath);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonObject edited = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        edited["native_video_pipeline"] = "d3d10";
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(edited).toJson());
        f.close();
        CHECK_EQ(s.nativeVideoPipeline(), QString("auto"));
    }
    // What the worker is handed is what the engine reads: every name the
    // setting can answer parses on the native side, to the chain it names.
    {
        const struct
        {
            const char* stored;
            mw::native::VideoPipeline engine;
        } names[] = {{"auto", mw::native::VideoPipeline::Auto},
                     {"d3d11", mw::native::VideoPipeline::D3d11},
                     {"d3d12", mw::native::VideoPipeline::D3d12},
                     {"vaapi", mw::native::VideoPipeline::Vaapi},
                     {"vulkan", mw::native::VideoPipeline::Vulkan}};
        for (const auto& n : names) {
            CHECK(s.setNativeVideoPipeline(n.stored));
            mw::native::VideoPipeline parsed = mw::native::VideoPipeline::Auto;
            CHECK(mw::native::parseVideoPipeline(s.nativeVideoPipeline().toStdString(), parsed));
            CHECK(parsed == n.engine);
        }
    }
    // The owner's apps in gamescope (Linux): none until some are set; stored
    // trimmed, refused whole when one entry is not valid — the file is then
    // left as it was — and an entry edited by hand into nonsense left out.
    {
        CHECK_EQ(s.gamescopeApps().size(), 0);
        QJsonArray apps;
        apps.append(QJsonObject{{"name", "  RetroArch "}, {"command", " retroarch -f "}});
        apps.append(QJsonObject{{"name", "Heroic"}, {"command", "heroic --no-gui"}});
        QString why;
        CHECK(s.setGamescopeApps(apps, &why));
        CHECK_EQ(s.gamescopeApps().size(), 2);
        CHECK_EQ(s.gamescopeApps().at(0).toObject().value("name").toString(), QString("RetroArch"));
        CHECK_EQ(s.gamescopeApps().at(0).toObject().value("command").toString(),
                 QString("retroarch -f"));

        QJsonArray bad = apps;
        bad.append(QJsonObject{{"name", "Two lines"}, {"command", "a\nb"}});
        CHECK(!s.setGamescopeApps(bad, &why));
        CHECK(why.contains("one line"));
        QJsonArray unnamed;
        unnamed.append(QJsonObject{{"name", "   "}, {"command", "x"}});
        CHECK(!s.setGamescopeApps(unnamed, &why));
        QJsonArray tooLong;
        tooLong.append(QJsonObject{{"name", QString(41, 'n')}, {"command", "x"}});
        CHECK(!s.setGamescopeApps(tooLong, &why));
        QJsonArray tooMany;
        for (int i = 0; i <= AppSettings::kGamescopeAppsMax; ++i)
            tooMany.append(QJsonObject{{"name", QString("App %1").arg(i)}, {"command", "x"}});
        CHECK(!s.setGamescopeApps(tooMany, &why));
        CHECK_EQ(s.gamescopeApps().size(), 2);

        QFile f(s.m_FilePath);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonObject edited = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonArray stored = edited["gamescope_apps"].toArray();
        stored.append(QJsonObject{{"name", "Hand-made"}});
        stored.append(QString("not an object"));
        edited["gamescope_apps"] = stored;
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(edited).toJson());
        f.close();
        CHECK_EQ(s.gamescopeApps().size(), 2);

        CHECK(s.setGamescopeApps(QJsonArray(), &why));
        CHECK_EQ(s.gamescopeApps().size(), 0);
    }

    s.setUniqueId("abcd1234");
    CHECK_EQ(s.uniqueId(), QString("abcd1234"));

    // The desktop portal's consent. Empty until one is granted — that emptiness
    // is what makes the very first session raise a dialog, so it is the answer
    // this getter has to give on a fresh install, not a placeholder.
    CHECK_EQ(s.portalRestoreToken(), QString());
    s.setPortalRestoreToken("portal-token-1");
    CHECK_EQ(s.portalRestoreToken(), QString("portal-token-1"));
    // Stored verbatim, never parsed: the token means something to the portal
    // that issued it and to nothing else, so anything it hands back survives
    // the round trip unchanged.
    s.setPortalRestoreToken("a/b+c=~ \xC3\xA9");
    CHECK_EQ(s.portalRestoreToken(), QString::fromUtf8("a/b+c=~ \xC3\xA9"));
    // Re-granted: a portal that asked again wins over what was remembered.
    s.setPortalRestoreToken("portal-token-2");
    CHECK_EQ(s.portalRestoreToken(), QString("portal-token-2"));
    // Writing the same value again is a no-op, not a rewrite of settings.json:
    // every session on the portal route would otherwise touch the file for a
    // token that did not change. Proved by re-writing the very same object by
    // hand in COMPACT form — writeAll() always indents, so a file that gained
    // newlines is a file that was written.
    {
        QFile f(s.m_FilePath);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject same = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(same).toJson(QJsonDocument::Compact));
        f.close();

        s.setPortalRestoreToken("portal-token-2");
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(!f.readAll().contains('\n'));
        f.close();
        CHECK_EQ(s.portalRestoreToken(), QString("portal-token-2"));

        // The positive control, without which the check above passes for a
        // getter that does nothing at all: a DIFFERENT token does write.
        s.setPortalRestoreToken("portal-token-3");
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll().contains('\n'));
        f.close();
        CHECK_EQ(s.portalRestoreToken(), QString("portal-token-3"));
    }

    s.setPublicIp("1.2.3.4");
    CHECK_EQ(s.publicIp(), QString("1.2.3.4"));
    s.setCertPem("MW_CERT_PEM");
    CHECK_EQ(s.certPem(), QString("MW_CERT_PEM"));
    s.setCertKey("MW_CERT_KEY");
    CHECK_EQ(s.certKey(), QString("MW_CERT_KEY"));
    s.setCertificateToken("token-xyz");
    CHECK_EQ(s.certificateToken(), QString("token-xyz"));

    // HMAC key (Base64 binary) round-trip.
    QByteArray key("\x01\x02\x03\x04binarykey", 13);
    s.setHmacKey(key);
    CHECK_EQ(s.hmacKey(), key);

    // domain(): a stored FQDN is a user-owned domain and must survive verbatim.
    // Everything else resolves to nothing — this host registers no name, and
    // handing out a sub-domain nothing points at would poison the Host-header
    // trust and every entry-point URL.
    qputenv("MW_DOMAIN", "example.test");
    s.setUniqueId("abcd1234");
    s.setDomain("MW_DOMAIN");
    CHECK_EQ(s.domain(), QString());
    s.setDomain("my.host.example.com");
    CHECK_EQ(s.domain(), QString("my.host.example.com"));
    // A stored value that is not a valid FQDN is not a domain at all.
    s.setDomain("nodot");
    CHECK_EQ(s.domain(), QString());
    // The name a pre-0.3 install registered is excluded by name: it is left in
    // settings.json on every upgraded machine, and it is not the user's.
    s.setDomain("abcd1234.example.test");
    CHECK_EQ(s.domain(), QString());
    s.setDomain("MW_DOMAIN");
    qunsetenv("MW_DOMAIN");

    // Consent record: versioned since the DNS mechanism started retiring. A
    // fresh write carries version 2 + the mechanism it was worded for; the
    // version reader maps "no record" to 0 (a legacy record without the field
    // reads as 1, the DNS-era wording).
    CHECK_EQ(s.internetConsentVersion(), 0);
    s.setInternetConsent("agreement text", "admin", "rendezvous");
    CHECK_EQ(s.internetConsentVersion(), 2);
    CHECK_EQ(s.internetConsent().value("mechanism").toString(), QString("rendezvous"));
    CHECK_EQ(s.internetConsent().value("source").toString(), QString("admin"));

    // ── Statistics reporting ──────────────────────────────────────────────
    // On unless someone says otherwise. Neither census carries a field that
    // identifies anyone, so the switch is the whole rule: no question is put,
    // and an unanswered machine is a reporting machine.
    CHECK_EQ(s.metricsConsentDecision(), QString());
    CHECK(s.sessionMetricsAllowed());
    CHECK(s.updateRelayAllowed());

    // One answer, both censuses: the disclosure describes them as one thing.
    s.setMetricsReporting(false);
    CHECK(!s.sessionMetricsAllowed());
    CHECK(!s.updateRelayAllowed());
    s.setMetricsReporting(true);
    CHECK(s.sessionMetricsAllowed());
    CHECK(s.updateRelayAllowed());

    // The switch and a hand-edited settings.json are the same control, and
    // each key still silences its own census alone.
    {
        QJsonObject obj = s.readAll();
        obj["session_metrics_enabled"] = false;
        s.writeAll(obj);
        CHECK(!s.sessionMetricsAllowed());
        CHECK(s.updateRelayAllowed()); // the other switch is untouched
        obj = s.readAll();
        obj["session_metrics_enabled"] = true;
        s.writeAll(obj);
    }

    // ── Statistics consent, dormant ───────────────────────────────────────
    // The machinery still records what it is given — wording, entry point,
    // timestamp, version — and still refuses to let an answer given to a
    // narrower question cover a wider one. What it no longer does is decide
    // anything: a refusal recorded by an older version does not silence a
    // machine, and a grant does not speak for it either.
    s.setMetricsConsent(false, "the wording that was displayed", "banner");
    CHECK_EQ(s.metricsConsentDecision(), QString("denied"));
    CHECK(s.sessionMetricsAllowed());
    CHECK(s.updateRelayAllowed());

    s.setMetricsConsent(true, "the wording that was displayed", "banner");
    CHECK_EQ(s.metricsConsentDecision(), QString("granted"));
    // The record has to say what was agreed to, and when.
    CHECK_EQ(s.metricsConsent().value("message").toString(),
             QString("the wording that was displayed"));
    CHECK_EQ(s.metricsConsent().value("source").toString(), QString("banner"));
    CHECK(!s.metricsConsent().value("at").toString().isEmpty());

    // An answer given to an older, narrower wording still reads as unasked, so
    // the day this goes back in the path it asks again instead of inheriting.
    {
        QJsonObject obj = s.readAll();
        QJsonObject consent = obj["metrics_consent"].toObject();
        consent["version"] = AppSettings::kMetricsConsentVersion - 1;
        obj["metrics_consent"] = consent;
        s.writeAll(obj);
        CHECK_EQ(s.metricsConsentDecision(), QString());
        // …and still reports, because the switch is what decides.
        CHECK(s.sessionMetricsAllowed());
        CHECK(s.updateRelayAllowed());
    }

    // Documented file-only defaults are idempotently seeded.
    s.seedDocumentedDefaults();
    s.audioTimeStretch(); // exercised (value documented as true by default)
    CHECK(s.readAll().contains("native_host_enabled"));

    // The native host is offered unless someone says otherwise — and an absent
    // key says nothing, which is the state every install before this setting is
    // in. Only an explicit false hides the card.
    {
        QJsonObject obj = s.readAll();
        obj.remove("native_host_enabled");
        s.writeAll(obj);
        CHECK(s.nativeHostEnabled());

        s.setNativeHostEnabled(false);
        CHECK(!s.nativeHostEnabled());
        s.setNativeHostEnabled(true);
        CHECK(s.nativeHostEnabled());
    }

    // ── Router ports (sticky UPnP allocation) ─────────────────────────────
    // Absent key = nothing remembered, on every axis.
    {
        QJsonObject obj = s.readAll();
        obj.remove("router_ports");
        s.writeAll(obj);
    }
    CHECK(s.rememberedTunnelPorts().isEmpty());
    CHECK_EQ(s.rememberedMediaPort(0), quint16(0));
    CHECK_EQ(s.routerLanIp(), QString());

    // Tunnel ports keep the order they were obtained in, without duplicates.
    s.rememberTunnelPort(3478);
    s.rememberTunnelPort(5349);
    s.rememberTunnelPort(3478);
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{3478, 5349}));
    s.forgetTunnelPort(3478);
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));
    s.forgetTunnelPort(4242); // never held: a no-op, not an error
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));
    s.rememberTunnelPort(0); // never a port
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));

    // Media ports are per slot, and forgetting one leaves the others alone.
    s.rememberMediaPort(0, 48010);
    s.rememberMediaPort(1, 46100);
    CHECK_EQ(s.rememberedMediaPort(0), quint16(48010));
    CHECK_EQ(s.rememberedMediaPort(1), quint16(46100));
    CHECK_EQ(s.rememberedMediaPort(2), quint16(0));
    s.forgetMediaPort(0);
    CHECK_EQ(s.rememberedMediaPort(0), quint16(0));
    CHECK_EQ(s.rememberedMediaPort(1), quint16(46100));

    // The LAN address the ports were obtained under, and the three live in
    // one object so a reader of settings.json sees them together.
    s.setRouterLanIp("192.168.1.20");
    CHECK_EQ(s.routerLanIp(), QString("192.168.1.20"));
    CHECK_EQ(s.rememberedTunnelPorts(), (QList<quint16>{5349}));
    {
        const QJsonObject rp = s.readAll().value("router_ports").toObject();
        CHECK(rp.contains("lan_ip"));
        CHECK(rp.contains("tunnel"));
        CHECK(rp.contains("media"));
    }
    // Not seeded: it is state, not a preference to discover in the file.
    {
        QJsonObject obj = s.readAll();
        obj.remove("router_ports");
        s.writeAll(obj);
        s.seedDocumentedDefaults();
        CHECK(!s.readAll().contains("router_ports"));
    }

    // The bench's engine knobs: absent, empty and never seeded; read trimmed
    // once hand-added.
    CHECK(s.nativeTuning().isEmpty());
    {
        s.seedDocumentedDefaults();
        CHECK(!s.readAll().contains("native_tuning"));
        QJsonObject obj = s.readAll();
        obj["native_tuning"] = " namedrops=1 ";
        s.writeAll(obj);
        CHECK_EQ(s.nativeTuning(), QString("namedrops=1"));
        obj.remove("native_tuning");
        s.writeAll(obj);
        CHECK(s.nativeTuning().isEmpty());
    }

    // Low-level access.
    QJsonObject all = s.readAll();
    CHECK(all.contains("http_port"));
    all["custom_key"] = 99;
    s.writeAll(all);
    CHECK_EQ(s.readAll().value("custom_key").toInt(), 99);

    // FQDN validation.
    CHECK(AppSettings::isValidFqdn("host.example.com"));
    CHECK(AppSettings::isValidFqdn("a.b"));
    CHECK(!AppSettings::isValidFqdn("nodot"));
    CHECK(!AppSettings::isValidFqdn(".leadingdot.com"));
    CHECK(!AppSettings::isValidFqdn("trailingdot."));
    CHECK(!AppSettings::isValidFqdn(""));

    // --config: every instance built afterwards uses that file, made absolute,
    // its directory created; an empty path goes back to the data directory.
    {
        const QString defaultPath = AppSettings().filePath();
        const QString custom = tmp.path() + "/managed/conf/settings.json";
        AppSettings::setFileOverride(custom);
        CHECK_EQ(AppSettings::fileOverride(), QFileInfo(custom).absoluteFilePath());
        AppSettings managed;
        CHECK_EQ(managed.filePath(), QFileInfo(custom).absoluteFilePath());
        CHECK(QFileInfo(tmp.path() + "/managed/conf").isDir());
        managed.setHttpsPort(9443);
        CHECK_EQ(AppSettings().httpsPort(1), quint16(9443));
        CHECK_EQ(s.httpsPort(1), quint16(443)); // built before: its own file

        AppSettings::setFileOverride("relative.json");
        CHECK(QFileInfo(AppSettings::fileOverride()).isAbsolute());

        AppSettings::setFileOverride(QString());
        CHECK(AppSettings::fileOverride().isEmpty());
        CHECK_EQ(AppSettings().filePath(), defaultPath);
    }
}

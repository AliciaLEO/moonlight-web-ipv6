/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "native_test_framework.h"

#include "capture/linux/GamescopeSession.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace mw::native::capture;

// Steam's Big Picture in its own gamescope (GamescopeSession.h): the version
// read off gamescope's banner, the lines of its log and of the wrapper, which
// Steam a card starts, the systemd-run command and the record a later stream
// finds. Pure, so every platform's run covers it; the processes are the
// bench's (§8s.14).

namespace {

bool contains(const std::vector<std::string>& words, const std::string& word)
{
    return std::find(words.begin(), words.end(), word) != words.end();
}

/// The word after @p flag, or "".
std::string after(const std::vector<std::string>& words, const std::string& flag)
{
    const auto it = std::find(words.begin(), words.end(), flag);
    return it == words.end() || it + 1 == words.end() ? std::string() : *(it + 1);
}

/// The value of the first "--property=<name>=…".
std::string property(const std::vector<std::string>& words, const std::string& name)
{
    const std::string prefix = "--property=" + name + "=";
    for (const std::string& w : words)
        if (w.rfind(prefix, 0) == 0) return w.substr(prefix.size());
    return std::string();
}

void testVersion()
{
    SECTION("gamescope: its version, off the banner");
    GamescopeVersion v;
    CHECK(parseGamescopeVersion("gamescope version 3.16.20 (gcc 14.2.0)", v));
    CHECK_EQ(v.text(), std::string("3.16.20"));
    // 3.16's banner, as it came out of the UM790Pro's build: a log prefix and
    // colour codes before it, a "+" and the compiler after.
    CHECK(parseGamescopeVersion("[gamescope] [\x1b[0;34mInfo\x1b[0m]  \x1b[0;37mconsole:\x1b[0m "
                                "gamescope version 3.16.31+ (gcc 13.3.0)",
                                v));
    CHECK_EQ(v.text(), std::string("3.16.31"));
    CHECK(parseGamescopeVersion("gamescope version 3.16.25+pfhdr8", v));
    CHECK_EQ(v.text(), std::string("3.16.25"));
    CHECK(parseGamescopeVersion("gamescope version 3.11", v));
    CHECK_EQ(v.text(), std::string("3.11.0"));
    // Not the compiler's version, and nothing without the word.
    CHECK(!parseGamescopeVersion("gamescope (gcc 13.3.0)", v));
    CHECK(!parseGamescopeVersion("gamescope version\n13.3.0", v));
    CHECK(!parseGamescopeVersion("", v));

    const auto recent = [](int major, int minor, int patch) {
        return GamescopeVersion{major, minor, patch}.atLeast(kGamescopeMinimum);
    };
    CHECK(recent(3, 16, 22));
    CHECK(recent(3, 16, 31));
    CHECK(recent(3, 17, 0));
    CHECK(recent(4, 0, 0));
    // Ubuntu 26.04's, and 25.04's, which stops without a screen.
    CHECK(!recent(3, 16, 20));
    CHECK(!recent(3, 16, 1));
    CHECK(!recent(3, 11, 49));
}

void testLogAndWrapper()
{
    SECTION("gamescope: the node in its log, the endpoints from its app");
    uint32_t node = 0;
    CHECK(parseGamescopeNodeLine("[gamescope] [\x1b[0;34mInfo\x1b[0m]  \x1b[0;37mpipewire:\x1b[0m "
                                 "stream available on node ID: 76",
                                 node));
    CHECK_EQ(node, 76u);
    CHECK(parseGamescopeNodeLine("pipewire: stream available on node ID:4242", node));
    CHECK_EQ(node, 4242u);
    CHECK(!parseGamescopeNodeLine("pipewire: stream state changed: streaming", node));
    CHECK(!parseGamescopeNodeLine("stream available on node ID: 0", node));
    CHECK(!parseGamescopeNodeLine("stream available on node ID: ", node));

    GamescopeEndpoints e;
    CHECK(parseGamescopeEndpoints("LIBEI_SOCKET=gamescope-0-ei\nDISPLAY=:2\n", e));
    CHECK_EQ(e.eisSocket, std::string("gamescope-0-ei"));
    CHECK_EQ(e.xDisplay, std::string(":2"));
    CHECK(!parseGamescopeEndpoints("LIBEI_SOCKET=gamescope-0-ei\n", e));
    CHECK(!parseGamescopeEndpoints("LIBEI_SOCKET=\nDISPLAY=:2\n", e));
}

SteamInstall install(SteamPackaging packaging, const std::string& home, int64_t lastUsed)
{
    SteamInstall s;
    s.packaging = packaging;
    s.home = home;
    s.lastUsed = lastUsed;
    switch (packaging) {
    case SteamPackaging::Native: s.launcher = {"/usr/bin/steam"}; break;
    case SteamPackaging::Snap: s.launcher = {"/snap/bin/steam"}; break;
    case SteamPackaging::Flatpak:
        s.launcher = {"/usr/bin/flatpak", "run", "com.valvesoftware.Steam"};
        break;
    }
    return s;
}

void testSteam()
{
    SECTION("gamescope: which Steam the card starts");
    // The UM790Pro on 01/10/2026: an unused .deb and the snap signed in to.
    const std::vector<SteamInstall> um = {
        install(SteamPackaging::Native, "/home/b", 0),
        install(SteamPackaging::Snap, "/home/b/snap/steam/common", 1759300000)};
    CHECK_EQ(chooseSteam(um, ""), 1);
    CHECK_EQ(chooseSteam(um, "/home/b/snap/steam/common"), 1);
    // The one running wins, signed in to or not.
    CHECK_EQ(chooseSteam(um, "/home/b"), 0);
    // The one signed in to last.
    const std::vector<SteamInstall> two = {
        install(SteamPackaging::Native, "/home/b", 1759000000),
        install(SteamPackaging::Flatpak, "/home/b/.var/app/com.valvesoftware.Steam", 1759300000)};
    CHECK_EQ(chooseSteam(two, ""), 1);
    // The only one, never signed in to: started, it asks for a sign-in.
    CHECK_EQ(chooseSteam({install(SteamPackaging::Snap, "/h/snap/steam/common", 0)}, ""), 0);
    // Two nobody signed in to: no guess.
    CHECK_EQ(chooseSteam({install(SteamPackaging::Native, "/h", 0),
                          install(SteamPackaging::Snap, "/h/snap/steam/common", 0)},
                         ""),
             -1);
    CHECK_EQ(chooseSteam({}, ""), -1);

    const std::vector<std::string> snap =
        steamBigPictureCommand(install(SteamPackaging::Snap, "/h/snap/steam/common", 1));
    CHECK(snap == (std::vector<std::string>{"/snap/bin/steam", "-gamepadui"}));
    const std::vector<std::string> flatpak = steamBigPictureCommand(
        install(SteamPackaging::Flatpak, "/h/.var/app/com.valvesoftware.Steam", 1));
    CHECK(flatpak == (std::vector<std::string>{"/usr/bin/flatpak", "run", "com.valvesoftware.Steam",
                                               "-gamepadui"}));

    const std::vector<std::string> remotes =
        steamRemoteCandidates(install(SteamPackaging::Snap, "/h/snap/steam/common", 1));
    CHECK_EQ(remotes.size(), size_t(2));
    CHECK_EQ(remotes[0], std::string("/h/snap/steam/common/.steam/root/ubuntu12_32/steam-runtime/"
                                     "amd64/usr/bin/steam-runtime-steam-remote"));

    CHECK(isSteamClient("/home/b/snap/steam/common/.local/share/Steam/ubuntu12_32/steam"));
    CHECK(!isSteamClient("/home/b/.local/share/Steam/steam.sh"));
    CHECK(!isSteamClient("/home/b/.local/share/Steam/ubuntu12_64/steamwebhelper"));
    CHECK(!isSteamClient("steam"));
    CHECK(isSteamGame("/home/b/.local/share/Steam/ubuntu12_32/reaper SteamLaunch AppId=730 -- x"));
    CHECK(!isSteamGame("/home/b/.local/share/Steam/ubuntu12_32/steam -srt-logger-opened"));
}

void testCommands()
{
    SECTION("gamescope: the unit's command line");
    CHECK_EQ(systemdWord("a$b%c\"d\\e"), std::string("\"a$$b%%c\\\"d\\\\e\""));
    CHECK_EQ(shellWord("it's"), std::string("'it'\\''s'"));

    const std::string stop = steamStopScript("/home/o'b/snap/steam/common", "/r/remote", 30);
    CHECK(stop.find("HOME='/home/o'\\''b/snap/steam/common' '/r/remote' -shutdown") == 0);
    CHECK(stop.find("[ $n -lt 60 ]") != std::string::npos);

    GamescopeLaunch launch;
    launch.unit = "moonlightweb-gamescope-steam";
    launch.binary = "/usr/bin/gamescope";
    launch.path = "/usr/bin:/bin";
    launch.width = 2560;
    launch.height = 1440;
    launch.refreshHz = 120;
    launch.steam = true;
    launch.app = {"/snap/bin/steam", "-gamepadui"};
    launch.infoPath = "/run/user/1000/moonlightweb-gamescope-steam.app";
    launch.logPath = "/run/user/1000/moonlightweb-gamescope-steam.log";
    launch.stopSteamHome = "/home/b/snap/steam/common";
    launch.stopSteamRemote = "/r/remote";
    launch.steamBack = {"/snap/bin/steam"};
    const std::vector<std::string> c = gamescopeUnitCommand(launch);

    CHECK_EQ(c[0], std::string("systemd-run"));
    CHECK(contains(c, "--user"));
    CHECK(contains(c, "--unit=moonlightweb-gamescope-steam"));
    CHECK(contains(c, "--property=UnsetEnvironment=DISPLAY WAYLAND_DISPLAY"));
    CHECK(contains(c, "--setenv=PATH=/usr/bin:/bin"));
    CHECK_EQ(after(c, "--backend"), std::string("headless"));
    CHECK_EQ(after(c, "-W"), std::string("2560"));
    CHECK_EQ(after(c, "-H"), std::string("1440"));
    CHECK_EQ(after(c, "-w"), std::string("2560"));
    CHECK_EQ(after(c, "-h"), std::string("1440"));
    CHECK_EQ(after(c, "-r"), std::string("120"));
    CHECK(contains(c, "--steam"));
    // The wrapper, its file, then the app, last.
    CHECK_EQ(after(c, "-c"), std::string(kGamescopeWrapper));
    CHECK_EQ(after(c, kGamescopeWrapper), launch.infoPath);
    CHECK_EQ(c[c.size() - 2], std::string("/snap/bin/steam"));
    CHECK_EQ(c.back(), std::string("-gamepadui"));
    // No ${…} for systemd-run to expand in the wrapper.
    CHECK(std::string(kGamescopeWrapper).find("${") == std::string::npos);

    const std::string execStop = property(c, "ExecStop");
    CHECK(execStop.rfind("\"/bin/sh\" \"-c\" \"HOME=", 0) == 0);
    // Every $ of the script doubled for systemd, none left alone.
    for (size_t i = 0; i < execStop.size(); ++i)
        if (execStop[i] == '$') {
            CHECK(i + 1 < execStop.size() && execStop[i + 1] == '$');
            ++i;
        }
    CHECK_EQ(property(c, "TimeoutStopSec"), std::string("45"));
    CHECK_EQ(property(c, "ExecStopPost"),
             std::string("\"systemd-run\" \"--user\" \"--collect\" \"--quiet\" "
                         "\"--unit=moonlightweb-gamescope-steam-steam-back\" \"/snap/bin/steam\""));

    // Another app: no --steam, nothing done to Steam at the end.
    launch.steam = false;
    launch.stopSteamHome.clear();
    launch.steamBack.clear();
    launch.app = {"/usr/bin/vkcube"};
    const std::vector<std::string> other = gamescopeUnitCommand(launch);
    CHECK(!contains(other, "--steam"));
    CHECK(property(other, "ExecStop").empty());
    CHECK(property(other, "ExecStopPost").empty());

    const std::vector<std::string> linger =
        gamescopeLingerCommand("moonlightweb-gamescope-steam", 600);
    CHECK(contains(linger, "--unit=moonlightweb-gamescope-steam-linger"));
    CHECK(contains(linger, "--on-active=600"));
    CHECK_EQ(linger.back(), std::string("moonlightweb-gamescope-steam.service"));
    CHECK_EQ(linger[linger.size() - 2], std::string("stop"));
}

void testCardName()
{
    SECTION("gamescope: the session name of one of the user's apps");
    CHECK_EQ(gamescopeCardName("Heroic Games Launcher"), std::string("app-heroic-games-launcher"));
    CHECK_EQ(gamescopeCardName("RetroArch!"), std::string("app-retroarch"));
    CHECK_EQ(gamescopeCardName("  Dolphin  (GC)  "), std::string("app-dolphin-gc"));
    CHECK_EQ(gamescopeCardName("\xC3\x89mulateur"), std::string("app-mulateur"));
    CHECK_EQ(gamescopeCardName("   "), std::string("app"));
    CHECK_EQ(gamescopeCardName(""), std::string("app"));
    const std::string longName = gamescopeCardName(std::string(80, 'x'));
    CHECK_EQ(longName.size(), size_t(36));
}

void testRecord()
{
    SECTION("gamescope: the record a later stream finds");
    GamescopeRecord r;
    r.unit = "moonlightweb-gamescope-steam";
    r.width = 1170;
    r.height = 2532;
    r.refreshHz = 60;
    r.node = 76;
    r.endpoints.eisSocket = "gamescope-0-ei";
    r.endpoints.xDisplay = ":2";
    const std::string text = formatGamescopeRecord(r);
    CHECK_EQ(text, std::string("mw1 moonlightweb-gamescope-steam 1170 2532 60 76 gamescope-0-ei "
                               ":2\n"));
    GamescopeRecord back;
    CHECK(parseGamescopeRecord(text, back));
    CHECK_EQ(back.unit, r.unit);
    CHECK_EQ(back.width, 1170);
    CHECK_EQ(back.height, 2532);
    CHECK_EQ(back.refreshHz, 60);
    CHECK_EQ(back.node, 76u);
    CHECK_EQ(back.endpoints.eisSocket, r.endpoints.eisSocket);
    CHECK_EQ(back.endpoints.xDisplay, r.endpoints.xDisplay);

    CHECK(!parseGamescopeRecord("mw2 u 1920 1080 60 76 gamescope-0-ei :2\n", back));
    CHECK(!parseGamescopeRecord("mw1 u 1920 1080 60 0 gamescope-0-ei :2\n", back));
    CHECK(!parseGamescopeRecord("mw1 u -1920 1080 60 76 gamescope-0-ei :2\n", back));
    CHECK(!parseGamescopeRecord("mw1 u 1920 1080 60 76 gamescope-0-ei\n", back));
    CHECK(!parseGamescopeRecord("", back));
}

} // namespace

void run_gamescope_tests()
{
    testVersion();
    testLogAndWrapper();
    testSteam();
    testCommands();
    testCardName();
    testRecord();
}

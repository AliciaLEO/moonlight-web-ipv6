/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "backend/VirtualDisplay.h"
#include "test_framework.h"

#include <QRegularExpression>

// The pure half of "MoonlightWeb Virtual Display": what an elevated helper
// will act on (the request file), what it answers (the result file), what it
// writes for the driver, and the names. No driver, no socket, every platform.

namespace {

bool isHex(const QString& s, int len)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]+$"));
    return s.size() == len && hex.match(s).hasMatch();
}

} // namespace

void run_virtual_display_tests()
{
    using namespace VirtualDisplay;

    SECTION("VirtualDisplay — one name per edition, for the card, the node and the task");
    CHECK_EQ(displayNameFor(QStringLiteral("MoonlightWeb")),
             QStringLiteral("MoonlightWeb Virtual Display"));
    CHECK_EQ(displayNameFor(QStringLiteral("MoonlightWebDev")),
             QStringLiteral("MoonlightWebDev Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWeb"), false),
             QStringLiteral("MoonlightWeb Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWebDev"), false),
             QStringLiteral("MoonlightWebDev Virtual Display"));
    CHECK_EQ(taskNameFor(QStringLiteral("MoonlightWeb"), true),
             QStringLiteral("MoonlightWeb-dev Virtual Display"));
    // The installer's task and the device node share the edition's name: the
    // --dev flag names the task only (a --dev instance never installs).
    CHECK_EQ(displayNameFor(QStringLiteral("MoonlightWeb")),
             taskNameFor(QStringLiteral("MoonlightWeb"), false));
    CHECK_EQ(applyArgument(), QStringLiteral("--vdisplay-apply"));

    SECTION("VirtualDisplay — the mode is 1080p at 120 Hz, SDR");
    CHECK_EQ(kWidth, 1920);
    CHECK_EQ(kHeight, 1080);
    CHECK_EQ(kRefreshHz, 120);

    SECTION("VirtualDisplay — only the four verbs get through the request parser");
    QString err;
    for (const char* verb : {"install", "uninstall", "activate", "deactivate"}) {
        const auto req = parseRequest(QByteArray("{\"action\":\"") + verb + "\"}", &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(toString(req->action), QString::fromLatin1(verb));
    }
    CHECK(!parseRequest("{\"action\":\"add\"}", &err).has_value());
    CHECK(!parseRequest("{\"action\":\"format-c\"}", &err).has_value());
    CHECK(!parseRequest("{}", &err).has_value());
    CHECK(!parseRequest("not json", &err).has_value());
    CHECK(!parseRequest("[1,2]", &err).has_value());
    // The one parameter: a display key to give the primary role back to,
    // compared and never interpreted — but read by an elevated process.
    {
        const auto req = parseRequest(
            "{\"action\":\"deactivate\",\"restore_primary\":\"\\\\\\\\?\\\\DISPLAY#GBT270D#5&1\"}",
            &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(req->restorePrimary, QStringLiteral("\\\\?\\DISPLAY#GBT270D#5&1"));
    }
    CHECK(!parseRequest("{\"action\":\"deactivate\",\"restore_primary\":42}", &err).has_value());
    CHECK(!parseRequest("{\"action\":\"deactivate\",\"restore_primary\":\"a\\nb\"}", &err)
               .has_value());
    {
        const QByteArray longKey = QByteArray(600, 'x');
        CHECK(!parseRequest("{\"action\":\"deactivate\",\"restore_primary\":\"" + longKey + "\"}",
                            &err)
                   .has_value());
    }
    // Round trip.
    {
        Request r;
        r.action = Request::Action::Deactivate;
        r.restorePrimary = QStringLiteral("12345");
        const auto back = parseRequest(toJson(r), &err);
        CHECK(back.has_value());
        if (back) {
            CHECK(back->action == Request::Action::Deactivate);
            CHECK_EQ(back->restorePrimary, QStringLiteral("12345"));
        }
        Request a;
        a.action = Request::Action::Activate;
        CHECK(!toJson(a).contains("restore_primary"));
    }

    SECTION("VirtualDisplay — the result file is read only once complete");
    CHECK(!parseResult("").has_value());
    CHECK(!parseResult("{\"st").has_value());
    CHECK(!parseResult("{}").has_value());
    {
        Result r;
        r.ok = true;
        r.stage = QStringLiteral("done");
        r.rebootRequired = true;
        r.display = QStringLiteral("\\\\.\\DISPLAY3");
        r.previousPrimary = QStringLiteral("\\\\?\\DISPLAY#GBT270D#5&1");
        const auto back = parseResult(toJson(r));
        CHECK(back.has_value());
        if (back) {
            CHECK(back->ok);
            CHECK(back->rebootRequired);
            CHECK_EQ(back->display, QStringLiteral("\\\\.\\DISPLAY3"));
            CHECK_EQ(back->previousPrimary, QStringLiteral("\\\\?\\DISPLAY#GBT270D#5&1"));
        }
        // One line, so a reader taking the last stdout line gets all of it.
        CHECK_EQ(toJson(r).count('\n'), 1);
    }

    SECTION("VirtualDisplay — the driver settings put our mode first, SDR, default GPU");
    {
        const QString xml = settingsXml();
        CHECK(xml.contains(QStringLiteral("<count>1</count>")));
        CHECK(xml.contains(QStringLiteral("<friendlyname>default</friendlyname>")));
        CHECK(xml.contains(QStringLiteral("<HDRPlus>false</HDRPlus>")));
        CHECK(xml.contains(QStringLiteral("<SDR10bit>false</SDR10bit>")));
        const int firstRes = xml.indexOf(QStringLiteral("<resolution>"));
        const int ours = xml.indexOf(QStringLiteral("<width>1920</width>"));
        CHECK(firstRes >= 0 && ours > firstRes);
        CHECK(xml.indexOf(QStringLiteral("<width>2560</width>")) > ours);
        CHECK(xml.indexOf(QStringLiteral("<g_refresh_rate>120</g_refresh_rate>")) <
              xml.indexOf(QStringLiteral("<g_refresh_rate>60</g_refresh_rate>")));
    }

    SECTION("VirtualDisplay — a client size is pinned, made even, or is no size at all");
    {
        int w = 2532, h = 1170;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, 2532);
        CHECK_EQ(h, 1170);
        // Odd is rounded down: a display mode is whole macroblocks to the
        // encoder that follows it.
        w = 1365;
        h = 767;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, 1364);
        CHECK_EQ(h, 766);
        // A small desktop passes as it is: 480 lines is the floor, and an
        // 800×600 client is streamed 800×600, not stretched into a shape it
        // never asked for (the client already pinned the size at its own
        // shape — this is the guard behind it).
        w = 800;
        h = 600;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, 800);
        CHECK_EQ(h, 600);
        // Out of bounds is pinned, never refused: a launch must not fail here.
        w = 99999;
        h = 12;
        CHECK(normaliseMode(w, h));
        CHECK_EQ(w, kModeMax);
        CHECK_EQ(h, kModeMin);
        // Nothing asked for: the default mode, and 0×0 says so.
        w = 0;
        h = 1080;
        CHECK(!normaliseMode(w, h));
        CHECK_EQ(w, 0);
        CHECK_EQ(h, 0);
        w = -1920;
        h = -1080;
        CHECK(!normaliseMode(w, h));
        CHECK_EQ(w, 0);
    }

    SECTION("VirtualDisplay — the size travels with the request and leads the mode list");
    {
        // "Match my screen" on a phone: the size reaches the elevated helper.
        const auto req =
            parseRequest("{\"action\":\"activate\",\"width\":2532,\"height\":1170}", &err);
        CHECK(req.has_value());
        if (req) {
            CHECK_EQ(req->width, 2532);
            CHECK_EQ(req->height, 1170);
        }
        // Junk and absence both mean "the mode that is there".
        const auto none = parseRequest("{\"action\":\"activate\"}", &err);
        CHECK(none.has_value());
        if (none) CHECK_EQ(none->width, 0);
        const auto bad =
            parseRequest("{\"action\":\"activate\",\"width\":\"big\",\"height\":8}", &err);
        CHECK(bad.has_value());
        if (bad) CHECK_EQ(bad->width, 0);
        Request r;
        r.action = Request::Action::Activate;
        r.width = 2532;
        r.height = 1170;
        const auto back = parseRequest(toJson(r), &err);
        CHECK(back.has_value());
        if (back) CHECK_EQ(back->height, 1170);
        CHECK(!toJson(Request{}).contains("width"));

        // The list the driver reads: the client's size first — it is the one
        // the desktop must come up at — then ours, then the common ones, each
        // listed once.
        const QString xml = settingsXml(2532, 1170);
        const int phone = xml.indexOf(QStringLiteral("<width>2532</width>"));
        CHECK(phone > 0);
        CHECK(xml.indexOf(QStringLiteral("<width>1920</width>")) > phone);
        CHECK(xml.contains(QStringLiteral("<height>1170</height>")));
        CHECK_EQ(settingsXml(1920, 1080).count(QStringLiteral("<width>1920</width>")), 1);
        CHECK_EQ(settingsXml(2560, 1440).count(QStringLiteral("<width>2560</width>")), 1);
        // A file we wrote is ours to rewrite; anyone else's VDD settings are
        // left exactly as they are.
        CHECK(isOurSettings(settingsXml()));
        CHECK(isOurSettings(settingsXml(2532, 1170)));
        CHECK(!isOurSettings(QStringLiteral("<?xml version=\"1.0\"?><vdd_settings/>")));
    }

    SECTION("VirtualDisplay — another VDD's mode list gains our size and keeps its own");
    {
        // A file the owner's own VDD wrote: their sizes, their refresh rates.
        const QString theirs =
            QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<vdd_settings>\n"
                           "  <resolutions>\n"
                           "    <resolution>\n      <width>800</width>\n"
                           "      <height>600</height>\n      <refresh_rate>30</refresh_rate>\n"
                           "    </resolution>\n"
                           "    <resolution>\n      <width>2560</width>\n"
                           "      <height>1440</height>\n      <refresh_rate>244</refresh_rate>\n"
                           "    </resolution>\n"
                           "  </resolutions>\n</vdd_settings>\n");
        bool changed = false;
        const QString grown = settingsWithMode(theirs, 2532, 1170, 120, &changed);
        CHECK(changed);
        // Ours is there, first — the driver reads the list in order.
        const int mine = grown.indexOf(QStringLiteral("<width>2532</width>"));
        CHECK(mine > 0);
        CHECK(grown.indexOf(QStringLiteral("<width>800</width>")) > mine);
        // And every one of theirs survived, refresh rates included.
        CHECK(grown.contains(QStringLiteral("<refresh_rate>244</refresh_rate>")));
        CHECK(grown.contains(QStringLiteral("<height>600</height>")));
        CHECK(!isOurSettings(grown));
        // A size the file already lists AT THAT RATE is added again to
        // nobody.
        settingsWithMode(grown, 2532, 1170, 120, &changed);
        CHECK(!changed);
        // The same size at another rate is another mode: the driver will not
        // offer a rate it does not list, and the next client's screen may
        // well run at 60 where this one ran at 120.
        const QString slower = settingsWithMode(grown, 2532, 1170, 60, &changed);
        CHECK(changed);
        CHECK(slower.contains(QStringLiteral("<refresh_rate>60</refresh_rate>")));
        CHECK(slower.contains(QStringLiteral("<refresh_rate>244</refresh_rate>")));
        // Nothing to add, or nowhere to add it: the file comes back untouched.
        CHECK_EQ(settingsWithMode(theirs, 0, 0, 120, &changed), theirs);
        CHECK(!changed);
        const QString noList = QStringLiteral("<vdd_settings><options/></vdd_settings>");
        CHECK_EQ(settingsWithMode(noList, 2532, 1170, 120, &changed), noList);
        CHECK(!changed);
    }

    SECTION("VirtualDisplay — the client's refresh rate travels and fills the mode list");
    {
        // Pinned into bounds, never refused — a launch must not fail over a
        // rate — and 0 means "the default rate".
        int hz = 165;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, 165);
        // The request is guarded against what the DRIVER takes: the bench's
        // MW_VDD_REFRESH reaches past the product's ceiling.
        hz = 500;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, 500);
        hz = 1000;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, kRateDriverMax);
        hz = 1;
        CHECK(normaliseRate(hz));
        CHECK_EQ(hz, kRateMin);
        hz = 0;
        CHECK(!normaliseRate(hz));
        CHECK_EQ(hz, 0);
        hz = -60;
        CHECK(!normaliseRate(hz));
        CHECK_EQ(hz, 0);

        // It reaches the elevated helper with the request, junk and absence
        // both meaning "the default rate".
        const auto req = parseRequest("{\"action\":\"activate\",\"refresh\":165}", &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(req->refresh, 165);
        const auto none = parseRequest("{\"action\":\"activate\"}", &err);
        CHECK(none.has_value());
        if (none) CHECK_EQ(none->refresh, 0);
        const auto bad = parseRequest("{\"action\":\"activate\",\"refresh\":\"fast\"}", &err);
        CHECK(bad.has_value());
        if (bad) CHECK_EQ(bad->refresh, 0);
        Request r;
        r.action = Request::Action::Activate;
        r.refresh = 60;
        const auto back = parseRequest(toJson(r), &err);
        CHECK(back.has_value());
        if (back) CHECK_EQ(back->refresh, 60);
        CHECK(!toJson(Request{}).contains("refresh"));

        // Every mode in the file we write carries it — the display exists to
        // show frames at the client's cadence, whatever the size — and the
        // driver's global list leads with it.
        const QString xml = settingsXml(0, 0, 60);
        CHECK_EQ(xml.count(QStringLiteral("<refresh_rate>120</refresh_rate>")), 0);
        CHECK(xml.count(QStringLiteral("<refresh_rate>60</refresh_rate>")) >= 4);
        CHECK(xml.indexOf(QStringLiteral("<g_refresh_rate>60</g_refresh_rate>")) <
              xml.indexOf(QStringLiteral("<g_refresh_rate>144</g_refresh_rate>")));
        // No rate measured: the default stands, and the file is the one the
        // installer has always written.
        CHECK_EQ(settingsXml(0, 0, 0), settingsXml());
    }

    SECTION("VirtualDisplay — the stream's rate under the product's ceiling, the bench's past it");
    {
        // The product: the stream's own rate, never past kRateMax, whatever
        // a page sent as its stream_fps.
        CHECK_EQ(refreshForStream(120, 0), 120);
        CHECK_EQ(refreshForStream(400, 0), kRateMax);
        CHECK_EQ(refreshForStream(10, 0), kRateMin);
        CHECK_EQ(refreshForStream(0, 0), 0);
        // MW_VDD_REFRESH: its own rate, up to the driver's ceiling, whatever
        // the stream's.
        CHECK_EQ(refreshForStream(60, 500), 500);
        CHECK_EQ(refreshForStream(60, 240), 240);
        CHECK_EQ(refreshForStream(0, 165), 165);
        CHECK_EQ(refreshForStream(60, 900), kRateDriverMax);
        // The product on Windows (decision A): kRateMax whatever the stream
        // asks, measured or not; the bench still wins.
        CHECK_EQ(refreshForStream(60, 0, true), kRateMax);
        CHECK_EQ(refreshForStream(144, 0, true), kRateMax);
        CHECK_EQ(refreshForStream(0, 0, true), kRateMax);
        CHECK_EQ(refreshForStream(60, 500, true), 500);
        // 500 travels to the elevated helper and into the driver's list.
        const auto req = parseRequest("{\"action\":\"activate\",\"refresh\":500}", &err);
        CHECK(req.has_value());
        if (req) CHECK_EQ(req->refresh, 500);
        CHECK(settingsXml(0, 0, 500).contains(
            QStringLiteral("<g_refresh_rate>500</g_refresh_rate>")));
    }

    SECTION("VirtualDisplay — the bundled driver is pinned file by file");
    CHECK_EQ(int(driverFiles().size()), 3);
    bool inf = false, cat = false, dll = false;
    for (const DriverFile& f : driverFiles()) {
        CHECK(isHex(QString::fromLatin1(f.sha256), 64));
        const QString name = QString::fromLatin1(f.name);
        CHECK(!name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')));
        inf = inf || name.endsWith(QLatin1String(".inf"), Qt::CaseInsensitive);
        cat = cat || name.endsWith(QLatin1String(".cat"), Qt::CaseInsensitive);
        dll = dll || name.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive);
    }
    CHECK(inf && cat && dll);
    CHECK(isHex(catalogSignerThumbprint(), 40));
    CHECK_EQ(hardwareId(), QStringLiteral("Root\\MttVDD"));
}

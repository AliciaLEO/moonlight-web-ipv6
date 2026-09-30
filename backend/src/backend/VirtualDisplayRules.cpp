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

#include "common/Edition.h"

#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>

// The platform-free half of VirtualDisplay: names, the request/result files,
// the driver's settings XML. Kept apart from VirtualDisplay.cpp so the
// security test target can link it without SetupAPI, the probe service or a
// driver — these are the rules that decide what an elevated process will do,
// and they are verified on every platform (tests/test_virtual_display.cpp).

namespace VirtualDisplay {

QString displayNameFor(const QString& productName)
{
    return productName + QStringLiteral(" Virtual Display");
}

QString displayName()
{
    // The installed edition's name, dev flag or not: a --dev instance is a
    // scratch copy of the same edition and streams the display the installer
    // of that edition created.
    return displayNameFor(mw::edition::productName());
}

QString taskNameFor(const QString& productName, bool devFlag)
{
    return productName + (devFlag ? QStringLiteral("-dev") : QString()) +
           QStringLiteral(" Virtual Display");
}

QString taskName()
{
    return taskNameFor(mw::edition::productName(), mw::edition::devFlag());
}

QString toString(Request::Action action)
{
    switch (action) {
    case Request::Action::Install: return QStringLiteral("install");
    case Request::Action::Uninstall: return QStringLiteral("uninstall");
    case Request::Action::Activate: return QStringLiteral("activate");
    case Request::Action::Deactivate: return QStringLiteral("deactivate");
    }
    return QString();
}

std::optional<Request> parseRequest(const QByteArray& json, QString* error)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("request is not a JSON object");
        return std::nullopt;
    }
    const QJsonObject obj = doc.object();
    Request req;
    const QString action = obj.value(QLatin1String("action")).toString();
    bool known = false;
    for (const Request::Action a : {Request::Action::Install, Request::Action::Uninstall,
                                    Request::Action::Activate, Request::Action::Deactivate}) {
        if (action == toString(a)) {
            req.action = a;
            known = true;
        }
    }
    if (!known) {
        if (error) *error = QStringLiteral("unknown action");
        return std::nullopt;
    }
    const QJsonValue restore = obj.value(QLatin1String("restore_primary"));
    if (!restore.isUndefined() && !restore.isNull() && !restore.isString()) {
        if (error) *error = QStringLiteral("restore_primary must be a string");
        return std::nullopt;
    }
    req.restorePrimary = restore.toString().trimmed();
    if (req.restorePrimary.size() > 512) {
        if (error) *error = QStringLiteral("restore_primary too long");
        return std::nullopt;
    }
    for (const QChar c : req.restorePrimary) {
        // A display key is compared, never interpreted — but an elevated
        // process reads it, and a control character has no place in one.
        if (c.category() == QChar::Other_Control) {
            if (error) *error = QStringLiteral("restore_primary contains control characters");
            return std::nullopt;
        }
    }
    // The size the client asks for. Out of bounds is pinned, not refused: a
    // launch must never fail over it, and the default mode is always there.
    req.width = obj.value(QLatin1String("width")).toInt(0);
    req.height = obj.value(QLatin1String("height")).toInt(0);
    normaliseMode(req.width, req.height);
    req.refresh = obj.value(QLatin1String("refresh")).toInt(0);
    normaliseRate(req.refresh);
    return req;
}

QByteArray toJson(const Request& req)
{
    QJsonObject obj;
    obj["action"] = toString(req.action);
    if (!req.restorePrimary.isEmpty()) obj["restore_primary"] = req.restorePrimary;
    if (req.width > 0 && req.height > 0) {
        obj["width"] = req.width;
        obj["height"] = req.height;
    }
    if (req.refresh > 0) obj["refresh"] = req.refresh;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

bool normaliseMode(int& width, int& height)
{
    if (width <= 0 || height <= 0) {
        width = height = 0;
        return false;
    }
    const auto pin = [](int v) { return std::min(kModeMax, std::max(kModeMin, v)) & ~1; };
    width = pin(width);
    height = pin(height);
    return true;
}

bool normaliseRate(int& hz)
{
    if (hz <= 0) {
        hz = 0;
        return false;
    }
    hz = std::min(kRateDriverMax, std::max(kRateMin, hz));
    return true;
}

int refreshForStream(int streamFps, int benchHz, bool faster)
{
    if (benchHz > 0) return std::min(kRateDriverMax, std::max(kRateMin, benchHz));
    if (faster) return kRateMax;
    if (streamFps <= 0) return 0;
    return std::min(kRateMax, std::max(kRateMin, streamFps));
}

std::optional<Result> parseResult(const QByteArray& json)
{
    if (json.trimmed().isEmpty()) return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return std::nullopt;
    const QJsonObject obj = doc.object();
    if (!obj.contains(QLatin1String("ok"))) return std::nullopt;
    Result res;
    res.ok = obj.value(QLatin1String("ok")).toBool(false);
    res.stage = obj.value(QLatin1String("stage")).toString();
    res.error = obj.value(QLatin1String("error")).toString();
    res.rebootRequired = obj.value(QLatin1String("reboot_required")).toBool(false);
    res.display = obj.value(QLatin1String("display")).toString();
    res.previousPrimary = obj.value(QLatin1String("previous_primary")).toString();
    return res;
}

QByteArray toJson(const Result& res)
{
    QJsonObject obj;
    obj["ok"] = res.ok;
    obj["stage"] = res.stage;
    if (!res.error.isEmpty()) obj["error"] = res.error;
    obj["reboot_required"] = res.rebootRequired;
    if (!res.display.isEmpty()) obj["display"] = res.display;
    if (!res.previousPrimary.isEmpty()) obj["previous_primary"] = res.previousPrimary;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

/// The line that makes the settings file ours. A VDD the owner installed
/// before us wrote a file without it, and that file is never touched.
static QString settingsMarker()
{
    return QStringLiteral("<!-- written by MoonlightWeb — do not edit, it is rewritten -->");
}

bool isOurSettings(const QString& xml)
{
    return xml.contains(settingsMarker());
}

/// One `<resolution>` block in the driver's own spelling: one refresh rate,
/// the one this activation wants, exactly as the file we write ourselves
/// spells it — the shape the driver was seen to accept on the bench, and the
/// only one to put in a file belonging to someone else.
static QString resolutionBlock(int width, int height, int refresh, const QString& indent)
{
    QString s;
    s += indent + QStringLiteral("<resolution>\n");
    s += indent + QStringLiteral("  <width>%1</width>\n").arg(width);
    s += indent + QStringLiteral("  <height>%1</height>\n").arg(height);
    s += indent + QStringLiteral("  <refresh_rate>%1</refresh_rate>\n").arg(refresh);
    s += indent + QStringLiteral("</resolution>\n");
    return s;
}

QString settingsWithMode(const QString& existing, int width, int height, int refresh, bool* changed)
{
    *changed = false;
    if (!normaliseMode(width, height)) return existing;
    if (!normaliseRate(refresh)) refresh = kRefreshHz;
    // Already offered: the driver lists this size AT this rate, nothing to
    // add. All three tags in the same block is what makes it this mode rather
    // than others that happen to share a number — and a size listed at
    // another rate still needs its own entry, since the rate is half of what
    // the client asked for.
    static const QRegularExpression block(QStringLiteral("<resolution>.*?</resolution>"),
                                          QRegularExpression::DotMatchesEverythingOption);
    const QString w = QStringLiteral("<width>%1</width>").arg(width);
    const QString h = QStringLiteral("<height>%1</height>").arg(height);
    const QString r = QStringLiteral("<refresh_rate>%1</refresh_rate>").arg(refresh);
    auto it = block.globalMatch(existing);
    while (it.hasNext()) {
        const QString one =
            it.next().captured(0).remove(QLatin1Char(' ')).remove(QLatin1Char('\t'));
        if (one.contains(w) && one.contains(h) && one.contains(r)) return existing;
    }
    const int open = existing.indexOf(QStringLiteral("<resolutions>"));
    if (open < 0) return existing;
    const int after = open + int(strlen("<resolutions>"));
    // The list is read in order and the first entry is the driver's default:
    // ours goes first, which is the point of adding it at all.
    QString out = existing;
    out.insert(after,
               QLatin1Char('\n') + resolutionBlock(width, height, refresh, QStringLiteral("    ")));
    *changed = true;
    return out;
}

QString settingsXml(int width, int height, int refresh)
{
    // The layout the driver ships (its own sample, verified on the bench):
    //   <monitors><count>, <gpu><friendlyname>, <global><g_refresh_rate>*,
    //   <resolutions><resolution>{width,height,refresh_rate}*, <options>.
    // Our mode goes first — the driver takes the first as the default — then
    // the common sizes, so a later mode change needs no driver reload.
    // A client size comes ahead of all of them: it is the mode this
    // activation exists for, and the desktop must come up at it. Every size
    // carries the client's own refresh rate, since that is the cadence the
    // frames this display exists to produce will be shown at.
    struct Size
    {
        int w, h;
    };
    static const Size sizes[] = {{1280, 720}, {2560, 1440}, {3840, 2160}};
    static const int rates[] = {60, 90, 144};
    const bool custom = normaliseMode(width, height);
    if (!normaliseRate(refresh)) refresh = kRefreshHz;

    QString xml;
    xml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml += settingsMarker() + QLatin1Char('\n');
    xml += QStringLiteral("<vdd_settings>\n");
    xml += QStringLiteral("  <monitors>\n    <count>1</count>\n  </monitors>\n");
    // The GPU that renders the display: the driver's own choice, unless the
    // bench names one. MW_VDD_GPU is how a GPU with no screen of its own (the
    // RTX of DualRTX) gets a display to capture, so its encoder can be measured
    // in a real stream rather than headless. Never set by the product.
    xml += QStringLiteral("  <gpu>\n    <friendlyname>%1</friendlyname>\n  </gpu>\n")
               .arg(qEnvironmentVariable("MW_VDD_GPU", QStringLiteral("default")));
    xml += QStringLiteral("  <global>\n");
    xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(refresh);
    for (const int hz : rates)
        if (hz != refresh)
            xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(hz);
    if (refresh != kRefreshHz)
        xml += QStringLiteral("    <g_refresh_rate>%1</g_refresh_rate>\n").arg(kRefreshHz);
    xml += QStringLiteral("  </global>\n  <resolutions>\n");
    const auto one = [&xml, refresh](int w, int h) {
        xml += QStringLiteral("    <resolution>\n      <width>%1</width>\n"
                              "      <height>%2</height>\n      <refresh_rate>%3</refresh_rate>\n"
                              "    </resolution>\n")
                   .arg(w)
                   .arg(h)
                   .arg(refresh);
    };
    if (custom) one(width, height);
    if (!custom || width != kWidth || height != kHeight) one(kWidth, kHeight);
    for (const Size& s : sizes)
        if (!custom || s.w != width || s.h != height) one(s.w, s.h);
    xml += QStringLiteral("  </resolutions>\n  <options>\n");
    xml += QStringLiteral("    <CustomEdid>false</CustomEdid>\n"
                          "    <PreventSpoof>false</PreventSpoof>\n"
                          "    <EdidCeaOverride>false</EdidCeaOverride>\n"
                          "    <HardwareCursor>true</HardwareCursor>\n"
                          "    <SDR10bit>false</SDR10bit>\n    <HDRPlus>false</HDRPlus>\n"
                          "    <logging>false</logging>\n    <debuglogging>false</debuglogging>\n"
                          "  </options>\n</vdd_settings>\n");
    return xml;
}

} // namespace VirtualDisplay

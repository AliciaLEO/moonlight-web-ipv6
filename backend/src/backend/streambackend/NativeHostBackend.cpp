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

#include "NativeHostBackend.h"

#include "../../backend/VirtualDisplay.h"
#include "../../common/Logger.h"
#include "../../server/AppSettings.h"

extern "C" {
#include "Limelight.h"
}

#include "HostOsProbe.h"
#include "NativeProbeService.h"

#include <QCollator>
#include <QHostInfo>

#include <algorithm>
#include <vector>

namespace {

/// One probe per call, deliberately. A display can be unplugged and a user can
/// log out between two requests, and a cached "available" would offer a host
/// that cannot start. The engine's probe is built to be cheap for this reason.
///
/// As a service the probe runs in another session and this is its latest
/// snapshot instead (see NativeProbeService) — asking still costs nothing, and
/// asking is what keeps the snapshot fresh.
mw::native::Capabilities probeEngine()
{
    return NativeProbeService::instance().snapshot();
}

/// Displays are presented to the browser as apps, so an app id has to survive
/// the round trip and come back meaning the same display. Display ids start at
/// 0 and NvApp treats id 0 as uninitialized, so the wire id is offset by one.
constexpr int kAppIdBase = 1;

int displayIdToAppId(int displayId)
{
    return displayId + kAppIdBase;
}

int appIdToDisplayId(int appId)
{
    return appId - kAppIdBase;
}

/// "MoonlightWeb Virtual Display" is a card whether or not the display exists
/// right now — it is off between streams — so it cannot be numbered after a
/// display index. A fixed id far above any display's: a machine has a handful
/// of monitors, never a thousand.
constexpr int kVirtualDisplayAppId = 1000;

/// The display @p appId stands for among @p caps, or -1 with @p why.
int displayOf(const mw::native::Capabilities& caps, int appId, QString* why)
{
    if (appId == kVirtualDisplayAppId) {
        // Our virtual display, turned on by the server before this launch
        // (main.cpp's /start): whichever display index it came up under.
        for (const mw::native::DisplayInfo& display : caps.displays)
            if (VirtualDisplay::isOurs(display)) return display.id;
        if (why) *why = QStringLiteral("The virtual display is not on");
        return -1;
    }
    const int displayId = appIdToDisplayId(appId);
    for (const mw::native::DisplayInfo& display : caps.displays)
        if (display.id == displayId) return displayId;
    // Almost always a display unplugged since the app list was fetched.
    if (why) *why = QStringLiteral("That display is no longer connected");
    return -1;
}

NvApp virtualDisplayApp(const mw::native::Capabilities& caps)
{
    NvApp app(kVirtualDisplayAppId, VirtualDisplay::displayName(), /*hdr=*/false);
    app.setHasBoxArt(false);
    app.setDevice(QJsonObject{
        {QStringLiteral("os"), HostOsProbe::toString(HostOsProbe::thisMachine())},
        {QStringLiteral("display"), QStringLiteral("virtual")},
        {QStringLiteral("model"), VirtualDisplay::displayName()},
        {QStringLiteral("key"), QStringLiteral("moonlightweb-virtual-display")},
        {QStringLiteral("battery"), caps.hasBattery},
        // What the card says about itself: MoonlightWeb's own display, not a
        // monitor somebody plugged in.
        {QStringLiteral("owned"), true},
    });
    return app;
}

} // namespace

int NativeHostBackend::virtualDisplayAppId()
{
    return kVirtualDisplayAppId;
}

int NativeHostBackend::displayForApp(int appId, QString* error)
{
    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        if (error)
            *error = caps.diagnostic.empty() ? QString::fromUtf8(mw::native::toString(caps.reason))
                                             : QString::fromStdString(caps.diagnostic);
        return -1;
    }
    return displayOf(caps, appId, error);
}

bool NativeHostBackend::isEnabled()
{
    return AppSettings().nativeHostEnabled();
}

bool NativeHostBackend::isAvailable()
{
    // The owner's answer comes first, and short-circuits: a machine that has
    // opted out must not pay for a probe — nor, as a service, spawn one in the
    // console session — to be told about an engine nobody will be offered.
    return isEnabled() && probeEngine().available;
}

QString NativeHostBackend::unavailableReason()
{
    if (!isEnabled()) return QStringLiteral("disabled by native_host_enabled in settings.json");
    const mw::native::Capabilities caps = probeEngine();
    if (caps.available) return QString();
    return caps.diagnostic.empty() ? QString::fromUtf8(mw::native::toString(caps.reason))
                                   : QString::fromStdString(caps.diagnostic);
}

int NativeHostBackend::codecModeSupport()
{
    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) return 0;

    // The union across GPUs. Which one a session actually gets is decided per
    // display by the engine's own Selector; this answers the broader question
    // the host list asks — "can this machine encode X at all".
    bool h264 = false, hevc = false, av1 = false, tenBit = false;
    bool h264_444 = false, hevc444 = false, av1_444 = false;
    for (const mw::native::GpuInfo& gpu : caps.gpus) {
        if (gpu.codecs.empty()) continue; // an API with no codec is no encoder
        for (mw::native::Codec codec : gpu.codecs) {
            switch (codec) {
            case mw::native::Codec::H264: h264 = true; break;
            case mw::native::Codec::Hevc: hevc = true; break;
            case mw::native::Codec::Av1: av1 = true; break;
            }
        }
        if (gpu.supports10Bit) tenBit = true;
        // 4:4:4 is per codec: NVENC has it on H.264 and HEVC but not AV1, and
        // advertising AV1 4:4:4 here would have the browser prefer a mode the
        // encoder then refuses.
        if (gpu.supports444(mw::native::Codec::H264)) h264_444 = true;
        if (gpu.supports444(mw::native::Codec::Hevc)) hevc444 = true;
        if (gpu.supports444(mw::native::Codec::Av1)) av1_444 = true;
    }

    int mask = 0;
    if (h264) mask |= SCM_H264;
    if (hevc) mask |= SCM_HEVC;
    if (av1) mask |= SCM_AV1_MAIN8;
    if (tenBit) {
        if (hevc) mask |= SCM_HEVC_MAIN10;
        if (av1) mask |= SCM_AV1_MAIN10;
    }
    if (h264_444) mask |= SCM_H264_HIGH8_444;
    if (hevc444) {
        mask |= SCM_HEVC_REXT8_444;
        if (tenBit) mask |= SCM_HEVC_REXT10_444;
    }
    if (av1_444) {
        mask |= SCM_AV1_HIGH8_444;
        if (tenBit) mask |= SCM_AV1_HIGH10_444;
    }
    return mask;
}

QString NativeHostBackend::hostDisplayName()
{
    QString host = QHostInfo::localHostName();
    if (host.isEmpty()) host = QStringLiteral("This PC");
    // The suffix is what makes the card unmistakable in a list that may also
    // hold Sunshine and Wolf hosts.
    return QStringLiteral("%1 — MoonlightWeb Host").arg(host);
}

BackendCapabilities NativeHostBackend::capabilities() const
{
    BackendCapabilities caps;
    // One physical screen cannot be handed to two players independently, so
    // multiUser stays false until a virtual display exists; provisioning and
    // lobbies have no meaning here at all.
    //
    // What IS true, and true only here: several displays stream at once. Each
    // has its own duplication and its own encoder session, so a second stream
    // takes nothing away from the first — unlike a GameStream host, which runs
    // one app at a time and would quit the first to start the second.
    caps.concurrentApps = true;
    return caps;
}

void NativeHostBackend::ensurePaired(BackendVoidCallback cb)
{
    // No PIN, no certificate, no association. There are not two parties here:
    // MoonlightWeb would be authenticating to itself.
    if (cb) cb(true, BackendError{});
}

void NativeHostBackend::listSeats(BackendSeatListCallback cb)
{
    if (!cb) return;

    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("Native streaming is not available on this machine")),
           {});
        return;
    }

    SeatRef seat;
    seat.id = seatId();
    seat.name = hostDisplayName();
    seat.busy = false;
    cb(true, BackendError{}, {seat});
}

void NativeHostBackend::allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb)
{
    Q_UNUSED(deviceSessionId);
    if (!cb) return;

    // There is exactly one seat and it is never owned: the machine's screen is
    // shared by whoever is streaming it, which is the same thing a single
    // Sunshine host does today.
    SeatRef seat;
    seat.id = seatId();
    seat.name = hostDisplayName();
    cb(true, BackendError{}, seat);
}

void NativeHostBackend::releaseSeat(const QString& seatId)
{
    Q_UNUSED(seatId);
}

void NativeHostBackend::getAppList(const QString& seatId, BackendAppListCallback cb)
{
    Q_UNUSED(seatId);
    if (!cb) return;

    const mw::native::Capabilities caps = probeEngine();
    // Our own virtual display is a card as long as it is installed, on and
    // off alike: opening it is what turns it on.
    const bool virtualInstalled = VirtualDisplay::probe().installed;
    if (!caps.available) {
        // A machine with nothing attached and our display to offer has one
        // card; any other reason to be unavailable has none.
        if (virtualInstalled && caps.reason == mw::native::Unavailability::NoDisplay) {
            cb(true, BackendError{}, QVector<NvApp>{virtualDisplayApp(caps)});
            return;
        }
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("Native streaming is not available on this machine")),
           {});
        return;
    }

    // One app per display. The existing app grid then IS the display picker
    // asked for in the plan — no new screen to build, and a single-monitor
    // machine shows one card that streams on one click.
    //
    // The probe lists displays in the OS's enumeration order (on Windows, GPU
    // by GPU), which puts "Display 2" before "Display 1" as soon as the second
    // one hangs off the first adapter. The grid reads left to right, so the
    // primary comes first and the rest follow by label, numbers compared as
    // numbers ("Display 10" after "Display 2"). Only the order changes: each
    // app keeps the id of its own display.
    std::vector<const mw::native::DisplayInfo*> ordered;
    ordered.reserve(caps.displays.size());
    for (const mw::native::DisplayInfo& display : caps.displays)
        ordered.push_back(&display);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const mw::native::DisplayInfo* a, const mw::native::DisplayInfo* b) {
                         if (a->primary != b->primary) return a->primary;
                         return collator.compare(QString::fromStdString(a->label),
                                                 QString::fromStdString(b->label)) < 0;
                     });

    QVector<NvApp> apps;
    apps.reserve(caps.displays.size() + 1);
    bool virtualListed = false;
    for (const mw::native::DisplayInfo* entry : ordered) {
        const mw::native::DisplayInfo& display = *entry;
        if (VirtualDisplay::isOurs(display)) {
            // On right now (a stream is running on it): the same card as
            // when it is off, under its own name and its fixed id.
            virtualListed = true;
            apps.append(virtualDisplayApp(caps));
            continue;
        }
        const bool hdrCapable = display.hdrActive && [&] {
            const mw::native::GpuInfo* gpu = caps.gpuFor(display);
            return gpu && gpu->supports10Bit;
        }();
        NvApp app(displayIdToAppId(display.id), QString::fromStdString(display.label), hdrCapable);
        // A monitor has no cover art, and there is no library here to look it up
        // in. Saying so spares the grid one 404 per display, on every render.
        app.setHasBoxArt(false);
        // What the card draws instead: the machine this display belongs to.
        // The OS is the one this server was built for — the native host is
        // this machine by definition.
        app.setDevice(QJsonObject{
            {QStringLiteral("os"), HostOsProbe::toString(HostOsProbe::thisMachine())},
            {QStringLiteral("display"), QString::fromLatin1(mw::native::toString(display.kind))},
            {QStringLiteral("model"), QString::fromStdString(display.model)},
            {QStringLiteral("key"), QString::fromStdString(display.key)},
            {QStringLiteral("battery"), caps.hasBattery},
        });
        apps.append(app);
    }
    if (virtualInstalled && !virtualListed) apps.append(virtualDisplayApp(caps));
    cb(true, BackendError{}, apps);
}

void NativeHostBackend::launch(const QString& seatId, const LaunchRequest& req,
                               BackendMediaCallback cb)
{
    Q_UNUSED(seatId);
    if (!cb) return;

    const mw::native::Capabilities caps = probeEngine();
    if (!caps.available) {
        const QString why = caps.diagnostic.empty()
                                ? QString::fromUtf8(mw::native::toString(caps.reason))
                                : QString::fromStdString(caps.diagnostic);
        Logger::warning(QStringLiteral("NativeHostBackend: launch refused — %1").arg(why));
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("Native streaming is not available on this machine")),
           MediaDescriptor{});
        return;
    }

    QString why;
    const int displayId = displayOf(caps, req.appId, &why);
    if (displayId < 0) {
        cb(false, BackendError::make(BackendError::NotFound, why), MediaDescriptor{});
        return;
    }

    // No network call, no XML, no key exchange — just the choice, handed to the
    // engine by the session that is about to build it.
    MediaDescriptor media;
    media.type = MediaType::NativeHost;
    media.nativeHost.displayId = displayId;
    media.nativeHost.hdrRequested = req.hdrEnabled;
    media.nativeHost.rideOutLoss = req.rideOutLoss;
    cb(true, BackendError{}, media);
}

void NativeHostBackend::resume(const QString& seatId, const LaunchRequest& req,
                               BackendMediaCallback cb)
{
    // Nothing survives a stream here: there is no host-side session to rejoin,
    // so resuming is starting. Keeping them identical means the launch/resume
    // self-heal in StreamSession stays correct without a special case.
    launch(seatId, req, std::move(cb));
}

void NativeHostBackend::quit(const QString& seatId, const QString& clientUniqueId,
                             BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    Q_UNUSED(clientUniqueId);
    // The engine is owned by the session, and stopping it is the session's own
    // teardown. There is no host-side state left to reclaim.
    if (cb) cb(true, BackendError{});
}

void NativeHostBackend::provisionSeat(const QJsonObject& params, BackendSeatCallback cb)
{
    Q_UNUSED(params);
    if (cb)
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("This backend has a single, permanent seat")),
           SeatRef{});
}

void NativeHostBackend::teardownSeat(const QString& seatId, BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    if (cb)
        cb(false, BackendError::make(BackendError::Unsupported,
                                     QStringLiteral("This backend has a single, permanent seat")));
}

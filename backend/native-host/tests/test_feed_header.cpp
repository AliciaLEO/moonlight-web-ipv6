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

// The guests' shared feed on its local pipe: the header both ends read, and
// the queue that keeps a slow subscriber from ever holding the feed back.

#include "mw/native/FeedWire.h"
#include "native_test_framework.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace mw::native::feed;

namespace {

std::vector<uint8_t> frameBytes(uint32_t number, bool keyframe, size_t size = 32)
{
    Header h;
    h.kind = Kind::Frame;
    h.keyframe = keyframe;
    h.frameNumber = number;
    std::vector<uint8_t> payload(size, static_cast<uint8_t>(number));
    return message(h, payload.data(), payload.size());
}

uint32_t numberOf(const FreshestQueue::Item& item)
{
    Header h;
    decodeHeader(item.bytes.data(), item.bytes.size(), h);
    return h.frameNumber;
}

} // namespace

void run_feed_header_tests()
{
    SECTION("Feed wire — a header comes back as it went, stamps and all");
    {
        Header h;
        h.kind = Kind::Frame;
        h.keyframe = true;
        h.payloadSize = 123456;
        h.frameNumber = 0xfffffff0u; // the top bits, not only the low byte
        h.presentUs = 1234567890123LL;
        h.capturedUs = h.presentUs + 110;
        h.submittedUs = h.capturedUs + 5;
        h.convertedUs = h.submittedUs + 210;
        h.encodedUs = h.convertedUs + 3070;
        h.publishedUs = h.encodedUs + 40;
        uint8_t bytes[kHeaderSize] = {};
        encodeHeader(h, bytes);
        // Little-endian on every machine: the magic reads "MWF1".
        CHECK_EQ(std::string(reinterpret_cast<const char*>(bytes), 4), std::string("MWF1"));
        Header back;
        CHECK(decodeHeader(bytes, sizeof(bytes), back) == Parse::Ok);
        CHECK(back.kind == Kind::Frame);
        CHECK(back.keyframe);
        CHECK_EQ(back.payloadSize, 123456u);
        CHECK_EQ(back.frameNumber, 0xfffffff0u);
        CHECK_EQ(back.presentUs, h.presentUs);
        CHECK_EQ(back.capturedUs, h.capturedUs);
        CHECK_EQ(back.submittedUs, h.submittedUs);
        CHECK_EQ(back.convertedUs, h.convertedUs);
        CHECK_EQ(back.encodedUs, h.encodedUs);
        CHECK_EQ(back.publishedUs, h.publishedUs);
        // A negative stamp survives too (a clock read before its epoch).
        h.presentUs = -5;
        encodeHeader(h, bytes);
        CHECK(decodeHeader(bytes, sizeof(bytes), back) == Parse::Ok);
        CHECK_EQ(back.presentUs, int64_t(-5));
    }

    SECTION("Feed wire — too short reads on, anything else wrong closes the pipe");
    {
        Header h;
        h.kind = Kind::Control;
        uint8_t bytes[kHeaderSize] = {};
        encodeHeader(h, bytes);
        Header out;
        CHECK(decodeHeader(bytes, kHeaderSize - 1, out) == Parse::NeedMore);
        CHECK(decodeHeader(bytes, kHeaderSize, out) == Parse::Ok);
        CHECK(out.kind == Kind::Control);

        std::string why;
        uint8_t wrong[kHeaderSize];
        std::copy(bytes, bytes + kHeaderSize, wrong);
        wrong[0] ^= 0xff;
        CHECK(decodeHeader(wrong, kHeaderSize, out, &why) == Parse::Bad);
        CHECK_EQ(why, std::string("not the feed's magic"));

        std::copy(bytes, bytes + kHeaderSize, wrong);
        wrong[4] = 2; // version 2
        CHECK(decodeHeader(wrong, kHeaderSize, out, &why) == Parse::Bad);

        std::copy(bytes, bytes + kHeaderSize, wrong);
        wrong[6] = 9; // no such kind
        CHECK(decodeHeader(wrong, kHeaderSize, out, &why) == Parse::Bad);

        std::copy(bytes, bytes + kHeaderSize, wrong);
        wrong[7] = 1; // a control message cannot be a keyframe
        CHECK(decodeHeader(wrong, kHeaderSize, out, &why) == Parse::Bad);

        Header huge;
        huge.payloadSize = kMaxPayload + 1;
        encodeHeader(huge, wrong);
        CHECK(decodeHeader(wrong, kHeaderSize, out, &why) == Parse::Bad);
        CHECK_EQ(why, std::string("a payload larger than any frame"));
    }

    SECTION("Feed wire — a message is its header and its payload, the size filled in");
    {
        Header h;
        h.kind = Kind::Frame;
        h.frameNumber = 7;
        h.payloadSize = 999; // whatever was set, the payload's own size wins
        const uint8_t payload[3] = {0, 0, 1};
        std::vector<uint8_t> m = message(h, payload, sizeof(payload));
        CHECK_EQ(m.size(), kHeaderSize + 3);
        Header back;
        CHECK(decodeHeader(m.data(), m.size(), back) == Parse::Ok);
        CHECK_EQ(back.payloadSize, 3u);
        CHECK_EQ(back.frameNumber, 7u);
        CHECK_EQ(m[kHeaderSize + 2], uint8_t(1));
    }

    SECTION("Feed queue — a delta waiting is replaced by the next one, a keyframe never");
    {
        FreshestQueue q;
        CHECK_EQ(q.pushFrame(frameBytes(10, true), true), 0);
        CHECK_EQ(q.pushFrame(frameBytes(11, false), false), 0);
        // The pipe still has not taken anything: 11 goes, 12 takes its place.
        CHECK_EQ(q.pushFrame(frameBytes(12, false), false), 1);
        CHECK_EQ(q.size(), 2u);
        CHECK_EQ(q.dropped(), int64_t(1));
        FreshestQueue::Item a = q.take();
        CHECK(a.keyframe);
        CHECK_EQ(numberOf(a), 10u);
        FreshestQueue::Item b = q.take();
        CHECK(!b.keyframe);
        CHECK_EQ(numberOf(b), 12u);
        CHECK(q.empty());
    }

    SECTION("Feed queue — a new keyframe replaces every picture still waiting");
    {
        FreshestQueue q;
        q.pushFrame(frameBytes(1, true), true);
        q.pushFrame(frameBytes(2, false), false);
        CHECK_EQ(q.pushFrame(frameBytes(3, true), true), 2);
        CHECK_EQ(q.size(), 1u);
        CHECK_EQ(numberOf(q.take()), 3u);
    }

    SECTION("Feed queue — control messages are never dropped, and keep their place");
    {
        FreshestQueue q;
        q.pushFrame(frameBytes(1, false), false);
        Header c;
        c.kind = Kind::Control;
        const std::string json = "{\"type\":\"cursor\"}";
        q.pushControl(message(c, reinterpret_cast<const uint8_t*>(json.data()), json.size()));
        // The delta after the control message is not the queue's last picture
        // to replace: the one before it stays, and so does the message.
        CHECK_EQ(q.pushFrame(frameBytes(2, false), false), 0);
        CHECK_EQ(q.pushFrame(frameBytes(3, false), false), 1);
        CHECK_EQ(q.size(), 3u);
        CHECK_EQ(numberOf(q.take()), 1u);
        CHECK(q.take().kind == Kind::Control);
        CHECK_EQ(numberOf(q.take()), 3u);
        // A keyframe clears the pictures, not the messages.
        q.pushFrame(frameBytes(4, false), false);
        q.pushControl(message(c, reinterpret_cast<const uint8_t*>(json.data()), json.size()));
        CHECK_EQ(q.pushFrame(frameBytes(5, true), true), 1);
        CHECK_EQ(q.size(), 2u);
        CHECK(q.take().kind == Kind::Control);
        CHECK_EQ(numberOf(q.take()), 5u);
    }
}

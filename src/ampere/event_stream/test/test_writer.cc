//
// Copyright (c) 2019 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "ampere/event_stream/event_stream.h"
#include "ampere/event_stream/event_stream.pb.h"
#include "ampere/event_stream/event_stream_proto.h"
#include "ampere/protobuf_utils/protobuf_utils.h"

using namespace std;
using namespace ampere;
using namespace ampere::event_stream;

class EventStreamTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        fd = open(test_file_name(), O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR);
        ASSERT_GT(fd, 2);
    }

    void TearDown() override { ASSERT_EQ(0, unlink(this->test_file_name())); }

    std::unique_ptr<EventStream> make_es()
    {
        EventStream::reset_global_definitions();
        EventStream::reset_global_enumerations();
        return std::make_unique<EventStreamProto>(fd);
    }

    static void verify_file_version(google::protobuf::io::ZeroCopyInputStream* stream)
    {
        std::uint32_t magic;
        std::uint32_t es_version;

        ampere::readHeaderFrom(stream, &magic, &es_version);
        ASSERT_EQ(magic, ampere::event_stream::protobuf_magic);
        ASSERT_EQ(es_version, ampere::event_stream::protobuf_es_version);
    }

    static std::unique_ptr<google::protobuf::io::FileInputStream> reopen_test_file()
    {
        int fd = open(test_file_name(), O_RDONLY);
        auto input_stream = std::make_unique<google::protobuf::io::FileInputStream>(fd);
        verify_file_version(input_stream.get());
        return input_stream;
    }

    bool read()
    {
        if (!input_stream)
            input_stream = reopen_test_file();
        return clearAndReadDelimitedFrom(input_stream.get(), &read_record, nullptr);
    }

    // The serialized form of each Event record in the test file, exactly as it was written
    static std::vector<std::string> written_event_records()
    {
        std::ifstream file(test_file_name(), std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

        std::vector<std::string> events;
        std::size_t pos = 2 * sizeof(std::uint32_t); // skip the magic number and version
        while (pos < contents.size()) {
            std::uint32_t size;
            EXPECT_LE(pos + sizeof(size), contents.size());
            std::memcpy(&size, contents.data() + pos, sizeof(size)); // little endian, as is every supported host
            pos += sizeof(size);
            EXPECT_LE(pos + size, contents.size());

            auto serialized = contents.substr(pos, size);
            pos += size;

            event_stream_proto::Record record;
            EXPECT_TRUE(record.ParseFromString(serialized));
            if (record.has_event())
                events.push_back(std::move(serialized));
        }
        return events;
    }

    int fd{0};
    event_stream_proto::Record read_record;
    std::unique_ptr<google::protobuf::io::FileInputStream> input_stream;

public:
    static const char* test_file_name()
    {
        const ::testing::TestInfo* const test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        return test_info->name();
    }
};

TEST(EventStreamBasic, OpenBadFileNameFails)
{
    ASSERT_THROW(std::make_unique<EventStreamProto>(""), runtime_error);
}

TEST_F(EventStreamTest, OpenByFileName)
{
    ASSERT_NO_THROW(std::make_unique<EventStreamProto>(test_file_name()));
}

TEST_F(EventStreamTest, PostEvent)
{
    {
        auto es = make_es();
        auto event_def = es->define_event("foo", "a foo");
        es->post_event(event_def, 200);
    }

    ASSERT_TRUE(read());
    ASSERT_TRUE(read_record.has_definition());
    EXPECT_EQ("foo", read_record.definition().name());

    ASSERT_TRUE(read());
    ASSERT_TRUE(read_record.has_event());
    EXPECT_EQ(200, read_record.event().time());
}

TEST_F(EventStreamTest, DisabledPostEvent)
{
    {
        auto es = make_es();
        es->disable();
        auto event_def = es->define_event("foo", "a foo");
        es->post_event(event_def, 200);
    }

    ASSERT_TRUE(read());
    ASSERT_TRUE(read_record.has_definition());
    EXPECT_EQ("foo", read_record.definition().name());

    ASSERT_FALSE(read());
}

TEST_F(EventStreamTest, PostEventWithOneValue)
{
    {
        auto es = make_es();
        auto event_def = es->define_event("foo", "a foo");
        auto val_def = es->define_data("bar", "a bar");
        auto event = es->open_event(event_def, 200);
        es->add_int_data(event, val_def, 1234);
        es->close_event(event);
    }

    ASSERT_TRUE(read());
    ASSERT_TRUE(read_record.has_definition());
    EXPECT_EQ("foo", read_record.definition().name());

    ASSERT_TRUE(read());
    ASSERT_TRUE(read_record.has_definition());
    EXPECT_EQ("bar", read_record.definition().name());

    ASSERT_TRUE(read());
    ASSERT_TRUE(read_record.has_event());
    EXPECT_EQ(200, read_record.event().time());
    EXPECT_EQ(1, read_record.event().values_size());
    EXPECT_EQ(1234, read_record.event().values(0).int_value());
}

TEST_F(EventStreamTest, PostEventWithTwoValues)
{
    {
        auto es = make_es();
        auto event_def = es->define_event("foo", "a foo");
        auto bar_def = es->define_data("bar", "a bar");
        auto baz_def = es->define_data("baz", "a foo");
        auto event = es->open_event(event_def, 1950);
        es->add_int_data(event, bar_def, 1234);
        es->add_int_data(event, baz_def, 5678);
        es->close_event(event);
    }

    do
        ASSERT_TRUE(read());
    while (read_record.has_definition());

    ASSERT_TRUE(read_record.has_event());
    EXPECT_EQ(1950, read_record.event().time());
    EXPECT_EQ(2, read_record.event().values_size());
    EXPECT_EQ(1234, read_record.event().values(0).int_value());
    EXPECT_EQ(5678, read_record.event().values(1).int_value());
}

TEST_F(EventStreamTest, PostStringValue)
{
    {
        auto es = make_es();
        auto event_def = es->define_event("foo", "a foo");
        auto val_def = es->define_data("bar", "a bar");
        auto event = es->open_event(event_def, 200);
        es->add_string_data(event, val_def, "hello world");
        es->close_event(event);
    }

    do
        ASSERT_TRUE(read());
    while (read_record.has_definition());

    ASSERT_TRUE(read_record.has_event());
    ASSERT_EQ(1, read_record.event().values_size());
    EXPECT_EQ(read_record.event().values(0).values_case(), event_stream_proto::Value::kStringValue);
    EXPECT_EQ(read_record.event().values(0).string_value(), "hello world");
}

TEST_F(EventStreamTest, PostAllIntegralTypes)
{
    signed char sch = std::numeric_limits<signed char>::max();
    unsigned char uch = std::numeric_limits<unsigned char>::max();
    signed short ssh = std::numeric_limits<signed short>::max();
    unsigned short ush = std::numeric_limits<unsigned short>::max();
    signed int si = std::numeric_limits<signed int>::max();
    unsigned int ui = std::numeric_limits<unsigned int>::max();
    signed long sl = std::numeric_limits<signed long>::max();
    unsigned long ul = std::numeric_limits<unsigned long>::max();
    signed long long sll = std::numeric_limits<signed long long>::max();
    unsigned long long ull = std::numeric_limits<unsigned long long>::max();

    {
        auto es = make_es();
        auto event_def = es->define_event("foo", "a foo");
        auto val_def = es->define_data("bar", "a bar");
        auto event = es->open_event(event_def, 1950);

        es->add_int_data(event, val_def, sch);
        es->add_uint_data(event, val_def, uch);
        es->add_int_data(event, val_def, ssh);
        es->add_uint_data(event, val_def, ush);
        es->add_int_data(event, val_def, si);
        es->add_uint_data(event, val_def, ui);
        es->add_int_data(event, val_def, sl);
        es->add_uint_data(event, val_def, ul);
        es->add_int_data(event, val_def, sll);
        es->add_uint_data(event, val_def, ull);

        es->close_event(event);
    }

    do
        ASSERT_TRUE(read());
    while (read_record.has_definition());

    ASSERT_TRUE(read_record.has_event());
    ASSERT_EQ(10, read_record.event().values_size());
    EXPECT_EQ(read_record.event().values(0).int_value(), sch);
    EXPECT_EQ(read_record.event().values(1).uint_value(), uch);
    EXPECT_EQ(read_record.event().values(2).int_value(), ssh);
    EXPECT_EQ(read_record.event().values(3).uint_value(), ush);
    EXPECT_EQ(read_record.event().values(4).int_value(), si);
    EXPECT_EQ(read_record.event().values(5).uint_value(), ui);
    EXPECT_EQ(read_record.event().values(6).int_value(), sl);
    EXPECT_EQ(read_record.event().values(7).uint_value(), ul);
    EXPECT_EQ(read_record.event().values(8).int_value(), sll);
    EXPECT_EQ(read_record.event().values(9).uint_value(), ull);
}

// Applies everything it is asked to both to an event being written and to the protobuf message that describes it
class MirroredEvent
{
public:
    MirroredEvent(
        EventStream& stream, event_stream_proto::Record& record, EventType type, std::int64_t id, std::uint64_t time)
        : es(stream), handle(stream.open_event(type, time)), expected(*record.mutable_event())
    {
        expected.set_definition_id(type);
        expected.set_id(id);
        expected.set_time(static_cast<std::int64_t>(time));
    }

    void add(DataType type, std::int64_t value)
    {
        es.add_int_data(handle, type, value);
        new_value(type)->set_int_value(value);
    }

    void add(DataType type, std::uint64_t value)
    {
        es.add_uint_data(handle, type, value);
        new_value(type)->set_uint_value(value);
    }

    void add(DataType type, const std::string& value)
    {
        es.add_string_data(handle, type, value);
        new_value(type)->set_string_value(value);
    }

    void close() { es.close_event(handle); }

private:
    event_stream_proto::Value* new_value(DataType type)
    {
        auto value = expected.add_values();
        value->set_definition_id(type);
        return value;
    }

    EventStream& es;
    EventHandle handle;
    event_stream_proto::Event& expected;
};

// Events are not written through protobuf messages, so check that every byte is what protobuf would have written
TEST_F(EventStreamTest, EventsAreSerializedAsProtobufSerializesThem)
{
    const std::int64_t int_values[] = {0,
                                       1,
                                       -1,
                                       127,
                                       128,
                                       -128,
                                       1234567890123,
                                       std::numeric_limits<std::int64_t>::min(),
                                       std::numeric_limits<std::int64_t>::max()};
    const std::uint64_t uint_values[] = {
        0, 1, 127, 128, 16383, 16384, 1ull << 63, std::numeric_limits<std::uint64_t>::max()};
    const std::string string_values[] = {"",
                                         "hello world",
                                         std::string(127, 'a'),
                                         std::string(128, 'b'),
                                         std::string(20000, 'c'),
                                         std::string("embedded\0nul", 13),
                                         "caf\xc3\xa9"};
    const std::uint64_t times[] = {0, 1, 127, 128, 123456789, 1ull << 63, std::numeric_limits<std::uint64_t>::max()};

    std::deque<event_stream_proto::Record> expected; // a deque, for references that stay valid as it grows
    std::int64_t id = 0;

    {
        auto es = make_es();
        const auto event_def = es->define_event("foo", "a foo");
        const auto int_def = es->define_data("bar", "a bar");
        const auto uint_def = es->define_data("baz", "a baz");
        const auto string_def = es->define_data("qux", "a qux");

        // No values at all, for each time and for event types that are zero and negative
        for (auto time : times) {
            for (EventType type : {event_def, 0, -1}) {
                MirroredEvent event(*es, expected.emplace_back(), type, id++, time);
                event.close();
            }
        }

        // One value of each kind, for data types that are zero and negative too
        for (DataType type : {int_def, 0, -1}) {
            for (auto value : int_values) {
                MirroredEvent event(*es, expected.emplace_back(), event_def, id++, 200);
                event.add(type, value);
                event.close();
            }
            for (auto value : uint_values) {
                MirroredEvent event(*es, expected.emplace_back(), event_def, id++, 200);
                event.add(type, value);
                event.close();
            }
            for (const auto& value : string_values) {
                MirroredEvent event(*es, expected.emplace_back(), event_def, id++, 200);
                event.add(type, value);
                event.close();
            }
        }

        // Every value in one event
        {
            MirroredEvent event(*es, expected.emplace_back(), event_def, id++, 1950);
            for (auto value : int_values)
                event.add(int_def, value);
            for (auto value : uint_values)
                event.add(uint_def, value);
            for (const auto& value : string_values)
                event.add(string_def, value);
            event.close();
        }

        // Events whose length takes one, two and three bytes to encode, the last one larger than the output buffer.
        // A short event follows each so that a reused handle is seen not to keep anything of its previous event.
        for (int num_values : {1, 60, 3000}) {
            MirroredEvent event(*es, expected.emplace_back(), event_def, id++, 1950);
            for (int i = 0; i < num_values; ++i)
                event.add(uint_def, static_cast<std::uint64_t>(i) << 20);
            event.close();

            MirroredEvent short_event(*es, expected.emplace_back(), event_def, id++, 1951);
            short_event.add(int_def, std::int64_t{-5});
            short_event.close();
        }

        // Two events open at once are written in the order they are closed
        {
            event_stream_proto::Record first_opened;
            MirroredEvent first(*es, first_opened, event_def, id++, 10);
            MirroredEvent second(*es, expected.emplace_back(), event_def, id++, 20);
            first.add(int_def, std::int64_t{1});
            second.add(string_def, std::string("second"));
            first.add(uint_def, std::uint64_t{2});
            second.close();
            first.close();
            expected.push_back(first_opened);
        }

        // An event posted while the stream is disabled is not written, but it does use up an id
        es->disable();
        es->post_event(event_def, 300);
        id++;
        es->enable();
        {
            MirroredEvent event(*es, expected.emplace_back(), event_def, id++, 301);
            event.close();
        }
    }

    const auto written = written_event_records();
    ASSERT_EQ(expected.size(), written.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(expected[i].SerializeAsString(), written[i])
            << "event " << i << ": " << expected[i].ShortDebugString();
}

TEST_F(EventStreamTest, PostParameters)
{
    {
        auto es = make_es();
        es->set_bool_parameter("bool_param", "something", true);
        es->set_int_parameter("int_param", "something", -4);
        es->set_uint_parameter("uint_param", "something", 9);
        es->set_string_parameter("string_param", "something", "hello");
    }

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.parameter().name(), "bool_param");
    EXPECT_EQ(read_record.parameter().bool_value(), true);

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.parameter().name(), "int_param");
    EXPECT_EQ(read_record.parameter().int_value(), -4);

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.parameter().name(), "uint_param");
    EXPECT_EQ(read_record.parameter().uint_value(), 9);

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.parameter().name(), "string_param");
    EXPECT_EQ(read_record.parameter().string_value(), "hello");
}

TEST_F(EventStreamTest, PostEnumeration)
{
    EnumMappingType values = {{1, "one"}, {2, "two"}, {3, "three"}};
    enum class SomeEnum {
        one = 1,
        two,
        three
    };
    EnumMappingType other_values = {{1, "one"}, {2, "two"}, {3, "three"}, {4, "four"}};
    enum class OtherEnum {
        one = 1,
        two,
        three,
        four
    };

    {
        auto es = make_es();
        es->define_data_with_enum<SomeEnum>("some_data", "something", values);
        es->define_data("another_data", "something");
        es->define_data_with_enum<OtherEnum>("other_data", "something", other_values);
        es->define_data_with_enum<SomeEnum>("some_other_data", "something", values);

        es->start_simulation();
    }

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.definition().kind(), event_stream_proto::Kind::VALUE);
    EXPECT_EQ(read_record.definition().name(), "some_data");
    EXPECT_EQ(read_record.definition().enumeration_id(), 0);

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.definition().kind(), event_stream_proto::Kind::VALUE);
    EXPECT_EQ(read_record.definition().name(), "another_data");
    EXPECT_FALSE(read_record.definition().has_enumeration_id());

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.definition().kind(), event_stream_proto::Kind::VALUE);
    EXPECT_EQ(read_record.definition().name(), "other_data");
    EXPECT_EQ(read_record.definition().enumeration_id(), 1);

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.definition().kind(), event_stream_proto::Kind::VALUE);
    EXPECT_EQ(read_record.definition().name(), "some_other_data");
    EXPECT_EQ(read_record.definition().enumeration_id(), 0);

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.enumeration().id(), 0);
    EXPECT_EQ(read_record.enumeration().values().size(), values.size());
    for (unsigned i = 1; i <= values.size(); ++i)
        EXPECT_EQ(read_record.enumeration().values().at(i), values[i]);

    ASSERT_TRUE(read());
    EXPECT_EQ(read_record.enumeration().id(), 1);
    EXPECT_EQ(read_record.enumeration().values().size(), other_values.size());
    for (unsigned i = 1; i <= other_values.size(); ++i)
        EXPECT_EQ(read_record.enumeration().values().at(i), other_values[i]);
}

/*
 * vim: sw=4:ts=4:et:si:cindent:cino=\:0,(0,g0,t0:
 */

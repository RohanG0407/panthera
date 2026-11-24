//
// Copyright (c) 2018-2022 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
#include "event_stream_proto.h"

#include <fcntl.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <boost/smart_ptr/make_unique.hpp>
#include <span>
#include <stdexcept>
#include <system_error>

#include "ampere/event_stream/event_stream.h"
#include "ampere/event_stream/event_stream.pb.h"
#include "ampere/protobuf_utils/protobuf_utils.h"

namespace ampere::event_stream {

namespace {

// Events are by far the most frequent record, so they are encoded straight into a reused buffer rather than
// through event_stream_proto::Event objects, which cost several allocations per event. What is written must stay
// byte for byte what protobuf serializes for the Event and Value messages of event_stream.proto: fields in
// field-number order, and a field without presence left out when it is zero. test_writer.cc compares the two.

using google::protobuf::io::CodedOutputStream;

enum class WireType : std::uint32_t {
    VARINT = 0,
    LENGTH_DELIMITED = 2
};

constexpr std::uint8_t make_tag(int field_number, WireType wire_type)
{
    const auto tag = (static_cast<std::uint32_t>(field_number) << 3) | static_cast<std::uint32_t>(wire_type);
    if (tag >= 0x80)
        throw std::logic_error("the event encoder assumes one-byte tags");
    return static_cast<std::uint8_t>(tag);
}

constexpr auto RECORD_EVENT_TAG = make_tag(event_stream_proto::Record::kEventFieldNumber, WireType::LENGTH_DELIMITED);
constexpr auto EVENT_DEFINITION_ID_TAG =
    make_tag(event_stream_proto::Event::kDefinitionIdFieldNumber, WireType::VARINT);
constexpr auto EVENT_ID_TAG = make_tag(event_stream_proto::Event::kIdFieldNumber, WireType::VARINT);
constexpr auto EVENT_TIME_TAG = make_tag(event_stream_proto::Event::kTimeFieldNumber, WireType::VARINT);
constexpr auto EVENT_VALUES_TAG = make_tag(event_stream_proto::Event::kValuesFieldNumber, WireType::LENGTH_DELIMITED);
constexpr auto VALUE_DEFINITION_ID_TAG =
    make_tag(event_stream_proto::Value::kDefinitionIdFieldNumber, WireType::VARINT);
constexpr auto VALUE_INT_VALUE_TAG = make_tag(event_stream_proto::Value::kIntValueFieldNumber, WireType::VARINT);
constexpr auto VALUE_UINT_VALUE_TAG = make_tag(event_stream_proto::Value::kUintValueFieldNumber, WireType::VARINT);
constexpr auto VALUE_STRING_VALUE_TAG =
    make_tag(event_stream_proto::Value::kStringValueFieldNumber, WireType::LENGTH_DELIMITED);

constexpr std::size_t MAX_VARINT_SIZE = 10;
constexpr std::size_t MAX_VARINT_FIELD_SIZE = 1 + MAX_VARINT_SIZE;
constexpr std::size_t MAX_LENGTH_SIZE = 5;

// Room at the front of EventHandleProto::record for RECORD_EVENT_TAG and the length of the Event
constexpr std::size_t RECORD_HEADER_SIZE = 1 + MAX_LENGTH_SIZE;

std::size_t int32_field_size(std::int32_t value)
{
    return value == 0 ? 0 : 1 + CodedOutputStream::VarintSize32SignExtended(value);
}

std::uint8_t* write_int32_field(std::uint8_t tag, std::int32_t value, std::uint8_t* target)
{
    if (value == 0)
        return target;
    *target++ = tag;
    return CodedOutputStream::WriteVarint32SignExtendedToArray(value, target);
}

std::uint8_t* write_int64_field(std::uint8_t tag, std::uint64_t value, std::uint8_t* target)
{
    if (value == 0)
        return target;
    *target++ = tag;
    return CodedOutputStream::WriteVarint64ToArray(value, target);
}

// Grow `buffer` by `max_size` bytes for `encode` to write into, then keep only the bytes up to where it stopped
template<typename Encode>
void append(std::vector<std::uint8_t>& buffer, std::size_t max_size, Encode&& encode)
{
    const auto size = buffer.size();
    buffer.resize(size + max_size);
    const auto* end = encode(buffer.data() + size);
    buffer.resize(end - buffer.data());
}

// Append a Value holding the member of its oneof that `value_tag` names. That member is written even when zero.
void append_integer_value(std::vector<std::uint8_t>& event,
                          DataType data_type,
                          std::uint8_t value_tag,
                          std::uint64_t value)
{
    append(event, 2 + 2 * MAX_VARINT_FIELD_SIZE, [&](std::uint8_t* target) {
        *target++ = EVENT_VALUES_TAG;
        auto* length = target++; // two varint fields always fit the single-byte form of a length
        target = write_int32_field(VALUE_DEFINITION_ID_TAG, data_type, target);
        *target++ = value_tag;
        target = CodedOutputStream::WriteVarint64ToArray(value, target);
        *length = static_cast<std::uint8_t>(target - length - 1);
        return target;
    });
}

} // namespace

EventStreamProto::EventStreamProto(int fd)
{
    ev_writer = std::make_unique<ProtobufStreamWriter>(fd, protobuf_magic, protobuf_es_version);
}

EventStreamProto::EventStreamProto(int fd_writer, int fd_reader)
{
    ev_writer = std::make_unique<ProtobufStreamWriter>(fd_writer, protobuf_magic, protobuf_es_version);
    ev_reader = std::make_unique<ProtobufStreamReader>(fd_reader, protobuf_magic, protobuf_es_version);
}

int EventStreamProto::open_file(const std::string& filename)
{
    int fd = ::open(filename.c_str(), O_CREAT | O_RDWR | O_TRUNC, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);

    if (fd == -1)
        throw std::system_error(errno, std::system_category(), filename);

    return fd;
}

void EventStreamProto::finalize_event_definition(EventType event_type,
                                                 const std::string& name,
                                                 const std::string& description)
{
    // Write event definition and we always expect a read
    event_stream_proto::Definition definition;

    definition.set_name(name);
    definition.set_description(description);
    definition.set_id(event_type);
    definition.set_kind(event_stream_proto::EVENT);

    write(definition);
    ++num_events;
}

void EventStreamProto::finalize_data_definition(DataType data_type,
                                                const std::string& name,
                                                const std::string& description)
{
    event_stream_proto::Definition definition;

    definition.set_name(name);
    definition.set_description(description);
    definition.set_id(data_type);
    definition.set_kind(event_stream_proto::VALUE);

    write(definition);
}

void EventStreamProto::finalize_data_definition(DataType data_type,
                                                const std::string& name,
                                                const std::string& description,
                                                EnumType enumeration)
{
    event_stream_proto::Definition definition;

    definition.set_name(name);
    definition.set_description(description);
    definition.set_id(data_type);
    definition.set_kind(event_stream_proto::VALUE);
    definition.set_enumeration_id(enumeration);

    write(definition);
}

void EventStreamProto::finalize_enum_definition(EnumType enum_type, const EnumMappingType& enumerations)
{
    event_stream_proto::Enumeration enumeration;

    enumeration.set_id(enum_type);
    enumeration.mutable_values()->insert(enumerations.begin(), enumerations.end());

    write(enumeration);
}

void EventStreamProto::set_bool_parameter(const std::string& name, const std::string& description, bool value)
{
    event_stream_proto::Parameter parameter;

    parameter.set_name(name);
    parameter.set_description(description);
    parameter.set_bool_value(value);

    write(parameter);
}

void EventStreamProto::set_int_parameter(const std::string& name, const std::string& description, std::int64_t value)
{
    event_stream_proto::Parameter parameter;

    parameter.set_name(name);
    parameter.set_description(description);
    parameter.set_int_value(value);

    write(parameter);
}

void EventStreamProto::set_uint_parameter(const std::string& name, const std::string& description, std::uint64_t value)
{
    event_stream_proto::Parameter parameter;

    parameter.set_name(name);
    parameter.set_description(description);
    parameter.set_uint_value(value);

    write(parameter);
}

void EventStreamProto::set_double_parameter(const std::string& name, const std::string& description, double value)
{
    event_stream_proto::Parameter parameter;

    parameter.set_name(name);
    parameter.set_description(description);
    parameter.set_double_value(value);

    write(parameter);
}

void EventStreamProto::set_string_parameter(const std::string& name,
                                            const std::string& description,
                                            const std::string& value)
{
    event_stream_proto::Parameter parameter;

    parameter.set_name(name);
    parameter.set_description(description);
    parameter.set_string_value(value);

    write(parameter);
}

void EventStreamProto::set_json_parameter(const std::string& name,
                                          const std::string& description,
                                          const std::string& value)
{
    event_stream_proto::Parameter parameter;

    parameter.set_name(name);
    parameter.set_description(description);
    parameter.set_json_value(value);

    write(parameter);
}

EventHandle EventStreamProto::open_event(EventType event_type, std::uint64_t time)
{
    catch_up();

    const std::uint64_t id = current_event_id++;

    if (free_event_handles.empty())
        free_event_handles.push_back(&event_handles.emplace_back());
    auto eh = free_event_handles.back();
    free_event_handles.pop_back();

    eh->record.resize(RECORD_HEADER_SIZE);
    append(eh->record, 3 * MAX_VARINT_FIELD_SIZE, [&](std::uint8_t* target) {
        target = write_int32_field(EVENT_DEFINITION_ID_TAG, event_type, target);
        target = write_int64_field(EVENT_ID_TAG, id, target);
        return write_int64_field(EVENT_TIME_TAG, time, target);
    });

    return eh;
}

void EventStreamProto::close_event(EventHandle event)
{
    auto eh = static_cast<EventHandleProto*>(event);

    if (enabled) {
        // The Event's length is only known now, so its header goes right in front of it in the room left for it
        const auto event_size = eh->record.size() - RECORD_HEADER_SIZE;
        const auto header_size = 1 + CodedOutputStream::VarintSize32(static_cast<std::uint32_t>(event_size));
        auto* header = eh->record.data() + RECORD_HEADER_SIZE - header_size;
        *header = RECORD_EVENT_TAG;
        CodedOutputStream::WriteVarint32ToArray(static_cast<std::uint32_t>(event_size), header + 1);

        std::unique_lock<std::mutex> lock{output_mtx};
        ev_writer->write_serialized({header, header_size + event_size});
    }

    free_event_handles.push_back(eh);
}

void EventStreamProto::add_int_data(EventHandle event, DataType data_type, std::int64_t value)
{
    auto eh = static_cast<EventHandleProto*>(event);
    append_integer_value(eh->record, data_type, VALUE_INT_VALUE_TAG, static_cast<std::uint64_t>(value));
}

void EventStreamProto::add_uint_data(EventHandle event, DataType data_type, std::uint64_t value)
{
    auto eh = static_cast<EventHandleProto*>(event);
    append_integer_value(eh->record, data_type, VALUE_UINT_VALUE_TAG, value);
}

void EventStreamProto::add_string_data(EventHandle event, DataType data_type, const std::string& value)
{
    auto eh = static_cast<EventHandleProto*>(event);
    const auto string_size = static_cast<std::uint32_t>(value.size());
    const auto value_size = static_cast<std::uint32_t>(int32_field_size(data_type) + 1
                                                       + CodedOutputStream::VarintSize32(string_size) + string_size);

    append(eh->record, 1 + MAX_LENGTH_SIZE + value_size, [&](std::uint8_t* target) {
        *target++ = EVENT_VALUES_TAG;
        target = CodedOutputStream::WriteVarint32ToArray(value_size, target);
        target = write_int32_field(VALUE_DEFINITION_ID_TAG, data_type, target);
        *target++ = VALUE_STRING_VALUE_TAG;
        target = CodedOutputStream::WriteVarint32ToArray(string_size, target);
        return CodedOutputStream::WriteRawToArray(value.data(), static_cast<int>(string_size), target);
    });
}

void EventStreamProto::start_simulation()
{
    catch_up();

    event_stream_proto::Control control;
    control.set_type(event_stream_proto::START_SIMULATION);
    this->write(control);
    ev_writer->flush();

    if (ev_reader) {
        event_stream_proto::Response response;

        // We should get a response back for every event definition we sent.
        for (int i = 0; i < num_events; i++) {
            ev_reader->read(response);

            int id = response.definition().id();
            bool enabled = response.definition().enable();

            auto event = Definitions::instance().get_event(id);

            if (event) {
                if (enabled)
                    event->enable();
                else
                    event->disable();
            }

            response.Clear();
        }
    }
}

void EventStreamProto::write(event_stream_proto::Definition& definition)
{
    event_stream_proto::Record record;
    record.set_allocated_definition(&definition);
    write(record);
    static_cast<void>(record.release_definition());
}

void EventStreamProto::write(event_stream_proto::Definition& definition, int id, bool& enabled)
{
    event_stream_proto::Record record;
    event_stream_proto::Response response;
    record.set_allocated_definition(&definition);
    write_read(record, response);
    enabled = response.definition().enable();
    id = response.definition().id();
    static_cast<void>(record.release_definition());
    static_cast<void>(response.release_definition());
}

void EventStreamProto::write(event_stream_proto::Control& control)
{
    event_stream_proto::Record record;
    record.set_allocated_control(&control);
    this->write(record);
    static_cast<void>(record.release_control());
}

void EventStreamProto::write(event_stream_proto::Parameter& parameter)
{
    if (enabled) {
        event_stream_proto::Record record;
        record.set_allocated_parameter(&parameter);
        write(record);
        static_cast<void>(record.release_parameter());
    }
}

void EventStreamProto::write(event_stream_proto::Enumeration& enumeration)
{
    event_stream_proto::Record record;
    record.set_allocated_enumeration(&enumeration);
    write(record);
    static_cast<void>(record.release_enumeration());
}

void EventStreamProto::write(event_stream_proto::Record& record)
{
    std::unique_lock<std::mutex> lock{output_mtx};
    ev_writer->write(record);
}

void EventStreamProto::write_read(event_stream_proto::Record& record, event_stream_proto::Response& response)
{
    std::unique_lock<std::mutex> lock{output_mtx};
    ev_writer->write(record);
    ev_writer->flush();
    if (ev_reader)
        ev_reader->read(response);
}

} // namespace ampere::event_stream

/*
 * vim: sw=4:ts=4:et:si:cindent:cino=\:0,(0,g0,t0:
 */

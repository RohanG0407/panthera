//
// Copyright (c) 2021-2022 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <boost/filesystem.hpp>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>

#include "protobuf_utils.h"

#pragma once

namespace ampere {

class ProtobufStreamWriter
{
public:
    ProtobufStreamWriter(int fd, uint32_t magic_number, uint32_t version);
    ProtobufStreamWriter(boost::filesystem::path filepath, uint32_t magic_number, uint32_t version, bool force = false);
    ~ProtobufStreamWriter();
    template<typename Item>
    bool write(const Item& item);
    // Write an item that the caller has already serialized
    bool write_serialized(std::span<const std::uint8_t> serialized_item);
    bool flush();

protected:
    boost::filesystem::path filepath;

private:
    void start(int fd, uint32_t magic_number, uint32_t version);

    // Only written to through coded_stream, which holds part of its buffer between flushes: writing to it or
    // flushing it directly would put the part of that buffer that is not yet filled into the output
    std::unique_ptr<google::protobuf::io::FileOutputStream> output_stream;
    // Kept for every item written, because a coded stream costs more to set up and tear down than a small item
    // costs to write. Bytes it holds only reach output_stream when it is trimmed, which flush() does.
    std::unique_ptr<google::protobuf::io::CodedOutputStream> coded_stream;
    FILE* fp{nullptr};
};

template<typename Item>
bool ProtobufStreamWriter::write(const Item& item)
{
    return writeDelimitedTo(item, coded_stream.get());
}

inline bool ProtobufStreamWriter::write_serialized(std::span<const std::uint8_t> serialized_item)
{
    return writeDelimitedTo(serialized_item, coded_stream.get());
}

class ProtobufStreamReader
{
public:
    ProtobufStreamReader(int fd, uint32_t magic_number, uint32_t max_version);
    ProtobufStreamReader(boost::filesystem::path filepath, uint32_t magic_number, uint32_t max_version);
    ~ProtobufStreamReader();
    uint32_t version() const { return stream_version; }
    template<typename Item>
    bool read(Item& item);

protected:
    boost::filesystem::path filepath;
    std::unique_ptr<google::protobuf::io::FileInputStream> input_stream;

private:
    FILE* fp{nullptr};
    int fd{-1};
    void verify_file_version(uint32_t magic_number, uint32_t max_version);
    uint32_t stream_version;
};

template<typename Item>
bool ProtobufStreamReader::read(Item& item)
{
    bool status = true;
    status = readDelimitedFrom(input_stream.get(), &item, nullptr);
    if (!status) {
        if (fp != nullptr) {
            int status = pclose(fp);
            if (status != 0) {
                std::string msg("error reading from xzcat or gunzip for ");
                msg += filepath.string();
                throw std::system_error(errno, std::system_category(), msg);
            }
            fp = nullptr;
        }
    }
    return status;
}

} // namespace ampere

/*
 * vim: sw=4:ts=4:et:si:cindent:cino=\:0,(0,g0,t0:
 */

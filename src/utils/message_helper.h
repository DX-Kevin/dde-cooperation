#ifndef UTILS_MESSAGE_H
#define UTILS_MESSAGE_H

#include <memory>
#include <optional>

#include <arpa/inet.h>

#include <google/protobuf/message.h>
#include <tl/expected.hpp>

#include <QByteArray>

#include "uvxx/Buffer.h"

#define SCAN_KEY "UOS-COOPERATION"

#if __BIG_ENDIAN__
#define htonll(x) (x)
#define ntohll(x) (x)
#else
#define htonll(x) (((uint64_t)htonl((x)&0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) (((uint64_t)ntohl((x)&0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif

static constexpr char MAGIC[] = "DDECPRT";

#pragma pack(push, 1)
struct MessageHeader {
    char magic_[sizeof(MAGIC)];
    uint64_t size_;

    MessageHeader(uint64_t size = 0)
        : size_(htonll(size)) {
        std::copy(&MAGIC[0], &MAGIC[sizeof(MAGIC)], &magic_[0]);
    }

    bool legal() const { return memcmp(&magic_[0], &MAGIC[0], sizeof(MAGIC)) == 0; }
    uint64_t size() const { return ntohll(size_); }
};
#pragma pack(pop)

const ssize_t header_size = sizeof(MessageHeader);

namespace MessageHelper {

enum class PARSE_ERROR {
    PARTIAL_MESSAGE,
    ILLEGAL_MESSAGE,
};

inline QByteArray genMessageQ(const google::protobuf::Message &msg) {
    MessageHeader header(msg.ByteSizeLong());

    QByteArray buff;
    buff.resize(header_size + msg.ByteSizeLong());
    memcpy(buff.data(), &header, header_size);
    msg.SerializeToArray(buff.data() + header_size, msg.ByteSizeLong());

    return buff;
}

inline std::vector<char> genMessage(const google::protobuf::Message &msg) {
    MessageHeader header(msg.ByteSizeLong());

    std::vector<char> buff(header_size + msg.ByteSizeLong());
    memcpy(buff.data(), &header, header_size);
    msg.SerializeToArray(buff.data() + header_size, msg.ByteSizeLong());

    return buff;
}

inline const MessageHeader &parseMessageHeader(const QByteArray &buffer) noexcept {
    return *reinterpret_cast<const MessageHeader *>(buffer.data());
}

template <typename T>
inline T parseMessageBody(const QByteArray &buffer) noexcept {
    T msg;
    msg.ParseFromArray(buffer.data(), buffer.size());

    return msg;
}

inline const MessageHeader &parseMessageHeader(const char *buffer) noexcept {
    return *reinterpret_cast<const MessageHeader *>(buffer);
}

template <typename T>
inline T parseMessageBody(const char *buffer, size_t size) noexcept {
    T msg;
    msg.ParseFromArray(buffer, size);

    return msg;
}

template <typename T>
inline tl::expected<T, PARSE_ERROR> parseMessage(uvxx::Buffer &buff) {
    auto header = buff.peak<MessageHeader>();
    if (!header.legal()) {
        return tl::make_unexpected(PARSE_ERROR::ILLEGAL_MESSAGE);
    }

    if (buff.size() < header_size + header.size()) {
        return tl::make_unexpected(PARSE_ERROR::PARTIAL_MESSAGE);
    }

    T msg;
    msg.ParseFromArray(buff.data() + header_size, header.size());
    buff.retrieve(header_size + header.size());

    return msg;
}

} // namespace MessageHelper

#endif // !UTILS_MESSAGE_H

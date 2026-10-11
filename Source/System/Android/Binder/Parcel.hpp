#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace arm64emu::yggdrasil {
// The service-manager subset uses Android 15 kernel-Binder parcels, not
// the pre-AIDL service-manager protocol. All reads are bounded and aligned.
class AndroidParcelReader {
  public:
    explicit AndroidParcelReader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
    template <class T> T read() {
        if (position_ > bytes_.size() || sizeof(T) > bytes_.size() - position_)
            throw std::invalid_argument("short Android parcel");
        T value;
        std::memcpy(&value, bytes_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return value;
    }
    std::string ascii_string(size_t limit) {
        int32_t count = read<int32_t>();
        if (count < 0 || static_cast<size_t>(count) > limit)
            throw std::invalid_argument("invalid Android string length");
        std::string value;
        for (int32_t i = 0; i < count; ++i) {
            auto c = read<uint16_t>();
            if (!c || c > 127)
                throw std::invalid_argument("non-ASCII service name");
            value.push_back(static_cast<char>(c));
        }
        if (read<uint16_t>())
            throw std::invalid_argument("unterminated Android string");
        while (position_ & 3)
            if (read<uint8_t>())
                throw std::invalid_argument("invalid Android string padding");
        return value;
    }
    size_t position() const { return position_; }
    void finish() const {
        if (position_ != bytes_.size())
            throw std::invalid_argument("trailing Android parcel data");
    }

  private:
    const std::vector<uint8_t> &bytes_;
    size_t position_ = 0;
};
class AndroidParcelWriter {
  public:
    template <class T> void write(T value) {
        size_t offset = bytes.size();
        bytes.resize(offset + sizeof(value));
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
    void ascii_string(const std::string &value) {
        write<int32_t>(value.size());
        for (unsigned char c : value)
            write<uint16_t>(c);
        write<uint16_t>(0);
        while (bytes.size() & 3)
            write<uint8_t>(0);
    }
    std::vector<uint8_t> bytes;
};
} // namespace arm64emu::yggdrasil

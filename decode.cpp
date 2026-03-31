#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <cstdint>
#include <cerrno>
#include <iconv.h>
#include <text_encoding>

template <typename CharT>
constexpr bool is_wchar(const CharT*) {
    return std::is_same_v<std::remove_cv_t<CharT>, wchar_t>;
}

template<typename T>
inline constexpr bool always_false_v = false;

enum class wide_platform_id {
    utf16le,
    utf32
};

namespace nrx {
    inline wide_platform_id current_platform() {
        if constexpr (sizeof(wchar_t) == 2) {
            return wide_platform_id::utf16le;
        } else if constexpr (sizeof(wchar_t) == 4) {
            return wide_platform_id::utf32;
        } else {
            static_assert(always_false_v<wchar_t>, "unsupported wchar_t size");
        }
    }
}

namespace text {
    struct env {
        std::text_encoding narrow;
        wide_platform_id   wide;
    };

    inline env cur_env() {
        return {
            std::text_encoding::environment(),
            nrx::current_platform()
        };
    }
}

template<typename RetT>
RetT make_ret(const std::u8string& u8) {
    if constexpr (std::is_same_v<RetT, std::u8string>) {
        return u8;
    } else if constexpr (std::is_same_v<RetT, std::string>) {
        std::string s;
        s.reserve(u8.size());
        for (char8_t ch : u8) {
            s.push_back(static_cast<char>(ch));
        }
        return s;
    } else {
        static_assert(always_false_v<RetT>, "RetT must be std::u8string or std::string");
    }
}

inline void append_utf8(std::u8string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char8_t>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char8_t>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        if (0xD800 <= cp && cp <= 0xDFFF) {
            throw std::runtime_error("invalid surrogate code point");
        }
        out.push_back(static_cast<char8_t>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char8_t>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
    } else {
        throw std::runtime_error("code point out of range");
    }
}

inline std::u8string utf8_bytes_to_u8(std::string_view sv) {
    std::u8string out;
    out.reserve(sv.size());
    for (unsigned char ch : sv) {
        out.push_back(static_cast<char8_t>(ch));
    }
    return out;
}

inline std::u8string iconv_to_u8(std::string_view sv, const char* from_enc) {
    iconv_t cd = iconv_open("UTF-8", from_enc);
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        throw std::runtime_error(std::string("iconv_open failed: ") + from_enc);
    }

    std::string out(sv.size() * 4 + 16, '\0');

    char* inbuf = const_cast<char*>(sv.data());
    size_t inleft = sv.size();

    char* outbuf = out.data();
    size_t outleft = out.size();

    while (inleft > 0) {
        size_t rc = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
        if (rc != static_cast<size_t>(-1)) {
            break;
        }

        if (errno == E2BIG) {
            std::size_t used = static_cast<std::size_t>(outbuf - out.data());
            out.resize(out.size() * 2);
            outbuf  = out.data() + used;
            outleft = out.size() - used;
            continue;
        }

        iconv_close(cd);
        throw std::runtime_error(std::string("iconv conversion failed: ") + from_enc);
    }

    std::size_t written = out.size() - outleft;
    iconv_close(cd);

    return std::u8string(
        reinterpret_cast<const char8_t*>(out.data()),
        reinterpret_cast<const char8_t*>(out.data() + written)
    );
}

inline std::u8string backend_from_euckr(std::string_view sv) {
    return iconv_to_u8(sv, "EUC-KR");
}

inline std::u8string backend_from_shiftjis(std::string_view sv) {
    return iconv_to_u8(sv, "SHIFT-JIS");
}

inline std::u8string utf16le_wstring_to_u8(std::wstring_view sv) {
    if constexpr (sizeof(wchar_t) != 2) {
        throw std::runtime_error("utf16le path requires 2-byte wchar_t");
    }

    std::u8string out;

    for (std::size_t i = 0; i < sv.size(); ++i) {
        uint32_t u = static_cast<uint16_t>(sv[i]);

        if (0xD800 <= u && u <= 0xDBFF) {
            if (i + 1 >= sv.size()) {
                throw std::runtime_error("dangling high surrogate");
            }

            uint32_t v = static_cast<uint16_t>(sv[i + 1]);
            if (!(0xDC00 <= v && v <= 0xDFFF)) {
                throw std::runtime_error("invalid low surrogate");
            }

            char32_t cp = 0x10000 + (((u - 0xD800) << 10) | (v - 0xDC00));
            append_utf8(out, cp);
            ++i;
        } else if (0xDC00 <= u && u <= 0xDFFF) {
            throw std::runtime_error("unexpected low surrogate");
        } else {
            append_utf8(out, static_cast<char32_t>(u));
        }
    }

    return out;
}

inline std::u8string utf32_wstring_to_u8(std::wstring_view sv) {
    if constexpr (sizeof(wchar_t) != 4) {
        throw std::runtime_error("utf32 path requires 4-byte wchar_t");
    }

    std::u8string out;
    for (wchar_t wc : sv) {
        append_utf8(out, static_cast<char32_t>(wc));
    }
    return out;
}

template<typename RetT>
RetT sdecode(std::string_view sv, const text::env& env) {
    switch (env.narrow.mib()) {
    case std::text_encoding::id::UTF8:
        return make_ret<RetT>(utf8_bytes_to_u8(sv));

    case std::text_encoding::id::EUCKR:
        return make_ret<RetT>(backend_from_euckr(sv));

    case std::text_encoding::id::ShiftJIS:
        return make_ret<RetT>(backend_from_shiftjis(sv));

    default:
        throw std::runtime_error(
            std::string("unsupported narrow encoding: ") + std::string(env.narrow.name())
        );
    }
}

template<typename RetT>
RetT wdecode(std::wstring_view sv, const text::env& env) {
    switch (env.wide) {
    case wide_platform_id::utf16le:
        return make_ret<RetT>(utf16le_wstring_to_u8(sv));

    case wide_platform_id::utf32:
        return make_ret<RetT>(utf32_wstring_to_u8(sv));
    }

    throw std::runtime_error("unsupported wide platform");
}

template<typename RetT, typename CharT>
RetT decode(const CharT* str) {
    if (!str) {
        return RetT{};
    }

    const auto env = text::cur_env();

    if constexpr (is_wchar(static_cast<const CharT*>(nullptr))) {
        return wdecode<RetT>(std::wstring_view{str}, env);
    } else if constexpr (std::is_same_v<std::remove_cv_t<CharT>, char>) {
        return sdecode<RetT>(std::string_view{str}, env);
    } else {
        static_assert(always_false_v<CharT>, "decode only supports char / wchar_t");
    }
}

int main() {
    auto a = decode<std::u8string>("hello");
    auto b = decode<std::string>("hello");
    auto c = decode<std::u8string>(L"hello");

    std::cout << b << '\n';
    std::cout << reinterpret_cast<const char*>(a.c_str()) << '\n';
    std::cout << reinterpret_cast<const char*>(c.c_str()) << '\n';
}

#include "System/Graphics/Thunks/GraphicThunk/ShaderIndexConversion.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace arm64emu::shader_fix {
namespace {

bool ident_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool digit(char c) { return c >= '0' && c <= '9'; }

// `close` is the index of a ')' or ']'. Returns the index of its opener, or npos when unbalanced.
size_t match_open(std::string_view s, size_t close) {
    int depth = 0;
    for (size_t i = close + 1; i-- > 0;) {
        const char c = s[i];
        if (c == ')' || c == ']') depth++;
        else if (c == '(' || c == '[') {
            if (--depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

// Start of the postfix expression (identifier, call, array access, field chain) that ends just
// before `end`; `end` itself when there is none.
size_t postfix_start(std::string_view s, size_t end) {
    size_t start = end;
    for (;;) {
        if (start == 0) break;
        const char c = s[start - 1];
        if (c == ')' || c == ']') {
            const size_t open = match_open(s, start - 1);
            if (open == std::string_view::npos) return end;
            start = open;
            continue;
        }
        if (!ident_char(c)) break;
        while (start > 0 && ident_char(s[start - 1])) start--;
        if (start > 0 && s[start - 1] == '.') { start--; continue; }
        break;
    }
    return start;
}

}  // namespace

bool is_es3_source(std::string_view source) {
    const size_t at = source.find("#version");
    if (at == std::string_view::npos || at > 256) return false;
    size_t line_end = source.find('\n', at);
    if (line_end == std::string_view::npos) line_end = source.size();
    std::string_view line = source.substr(at + 8, line_end - (at + 8));
    auto token = [&line]() {
        const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
        while (!line.empty() && blank(line.front())) line.remove_prefix(1);
        size_t n = 0;
        while (n < line.size() && !blank(line[n])) n++;
        const std::string_view word = line.substr(0, n);
        line.remove_prefix(n);
        return word;
    };
    const std::string_view number = token();
    const std::string_view profile = token();
    return (number == "300" || number == "310" || number == "320") && profile == "es" && token().empty();
}

std::string wrap_scaled_index_operands(std::string_view s) {
    if (!is_es3_source(s)) return {};
    std::vector<std::pair<size_t, const char*>> inserts;  // (position, text), ascending by scan order
    for (size_t close = s.find(']'); close != std::string_view::npos; close = s.find(']', close + 1)) {
        // ... * N + M]   scanned right to left from the ']'
        size_t i = close;
        const size_t m_end = i;
        while (i > 0 && digit(s[i - 1])) i--;
        if (i == m_end) continue;
        if (i < 3 || s.compare(i - 3, 3, " + ") != 0) continue;
        i -= 3;
        const size_t n_end = i;
        while (i > 0 && digit(s[i - 1])) i--;
        if (i == n_end) continue;
        if (i < 3 || s.compare(i - 3, 3, " * ") != 0) continue;
        const size_t operand_end = i - 3;
        const size_t operand_start = postfix_start(s, operand_end);
        if (operand_start == operand_end || operand_start == 0 || s[operand_start - 1] != '[') continue;
        inserts.emplace_back(operand_start, "int(");
        inserts.emplace_back(operand_end, ")");
    }
    if (inserts.empty()) return {};
    std::stable_sort(inserts.begin(), inserts.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out;
    out.reserve(s.size() + inserts.size() * 4);
    size_t at = 0;
    for (const auto& [pos, text] : inserts) {
        out.append(s.substr(at, pos - at));
        out += text;
        at = pos;
    }
    out.append(s.substr(at));
    return out;
}

}  // namespace arm64emu::shader_fix

#include "fsw.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>

namespace fsw {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

std::uint32_t codepoint(std::string_view text, std::size_t& offset, Position at) {
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80) return first;
    const unsigned count = first >= 0xc2 && first <= 0xdf ? 1u :
                           first >= 0xe0 && first <= 0xef ? 2u : first >= 0xf0 && first <= 0xf4 ? 3u : 0u;
    if (!count || text.size() - offset < count) throw Error(at, "invalid UTF-8 source");
    std::uint32_t value = first & (0x7fu >> count);
    for (unsigned i = 0; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[offset++]);
        if ((next & 0xc0) != 0x80) throw Error(at, "invalid UTF-8 source");
        value = (value << 6) | (next & 0x3f);
    }
    if (value < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        throw Error(at, "invalid UTF-8 source");
    return value;
}

std::string string_literal(Token token) {
    const bool verbatim = token.text.front() == '@';
    const auto text = token.text.substr(verbatim ? 2 : 1, token.text.size() - (verbatim ? 3 : 2));
    std::string result;
    auto unit = [&](std::uint32_t value) {
        result.push_back(static_cast<char>(value & 0xff));
        result.push_back(static_cast<char>(value >> 8));
    };
    for (std::size_t i = 0; i < text.size();) {
        auto value = codepoint(text, i, token.position);
        if (verbatim && value == '"') ++i;
        else if (!verbatim && value == '\\') {
            if (i == text.size()) throw Error(token.position, "incomplete string escape");
            const char escape = text[i++];
            switch (escape) {
            case '\\': value = '\\'; break;
            case '"': value = '"'; break;
            case '\'': value = '\''; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case 'b': value = '\b'; break;
            case 'a': value = '\a'; break;
            case 'f': value = '\f'; break;
            case 'v': value = '\v'; break;
            case 'u': case 'U': {
                const std::size_t count = escape == 'u' ? 4u : 8u;
                if (text.size() - i < count) throw Error(token.position, "incomplete Unicode escape");
                const auto parsed = std::from_chars(text.data() + i, text.data() + i + count, value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + i + count || value > 0x10ffff)
                    throw Error(token.position, "invalid Unicode escape");
                i += count;
                break;
            }
            default:
                if (!digit(escape) || text.size() - i < 2 || !digit(text[i]) || !digit(text[i + 1]))
                    throw Error(token.position, "invalid string escape");
                value = static_cast<unsigned>((escape - '0') * 100 + (text[i] - '0') * 10 + text[i + 1] - '0');
                if (value > 255) throw Error(token.position, "decimal string escape exceeds 255");
                i += 2;
                break;
            }
        }
        if (value <= 0xffff) unit(value);
        else {
            value -= 0x10000;
            unit(0xd800 + (value >> 10)); unit(0xdc00 + (value & 0x3ff));
        }
    }
    return result;
}

class Lexer {
    std::string_view source;
    Position at;
    std::vector<Token> tokens;
    std::vector<int> indents{0};
    std::vector<std::string_view> delimiters;
    bool line_has_code = false;

    char peek(std::size_t n = 0) const {
        return at.offset + n < source.size() ? source[at.offset + n] : '\0';
    }
    bool starts(std::string_view text) const { return source.substr(at.offset, text.size()) == text; }
    void advance() { ++at.offset; ++at.column; }
    void push(TokenKind kind, Position start, std::size_t length = 0) {
        if (tokens.size() >= 2 * 1024 * 1024)
            throw Error(start, "source exceeds the token limit");
        tokens.push_back({kind, source.substr(start.offset, length), start});
    }
    void newline() {
        if (line_has_code && delimiters.empty()) push(TokenKind::Newline, at);
        if (peek() == '\r') advance();
        if (peek() == '\n') advance();
        ++at.line;
        at.column = 1;
        line_has_code = false;
    }
    void indent() {
        if (line_has_code || !delimiters.empty()) return;
        const int width = at.column - 1;
        if (width > indents.back()) {
            if (indents.size() >= static_cast<std::size_t>(max_depth))
                throw Error(at, "indentation is too deeply nested");
            indents.push_back(width);
            push(TokenKind::Indent, at);
        } else {
            while (width < indents.back()) {
                indents.pop_back();
                push(TokenKind::Dedent, at);
            }
            if (width != indents.back()) throw Error(at, "inconsistent indentation");
        }
    }
public:
    explicit Lexer(std::string_view source) : source(source) {}

    std::vector<Token> run() {
        if (source.size() > max_source_bytes) throw Error(at, "source exceeds the 16 MiB limit");
        Position validation;
        for (std::size_t i = starts("\xef\xbb\xbf") ? 3 : 0; i < source.size();) {
            validation.offset = i;
            const auto start = i;
            if (static_cast<unsigned char>(source[i]) < 0x80) {
                if (source[i] == '\0') throw Error(validation, "NUL is not allowed in source");
                if (source[i] == '\n' || source[i] == '\r') {
                    if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n') ++i;
                    ++i; ++validation.line; validation.column = 1;
                    continue;
                }
                ++i;
            } else codepoint(source, i, validation);
            validation.column += static_cast<int>(i - start);
        }
        if (starts("\xef\xbb\xbf")) at.offset = 3;
        while (at.offset < source.size()) {
            const char c = peek();
            if (c == ' ') { advance(); continue; }
            if (c == '\t') throw Error(at, "tabs are not allowed in F# indentation; use spaces");
            if (c == '\n' || c == '\r') { newline(); continue; }
            if (starts("//")) {
                while (at.offset < source.size() && peek() != '\n' && peek() != '\r') advance();
                continue;
            }
            if (starts("(*")) {
                const Position start = at;
                int level = 1;
                advance(); advance();
                while (at.offset < source.size() && level != 0) {
                    if (starts("(*")) { ++level; advance(); advance(); }
                    else if (starts("*)")) { --level; advance(); advance(); }
                    else if (peek() == '\n' || peek() == '\r') newline();
                    else advance();
                }
                if (level) throw Error(start, "unterminated block comment");
                continue;
            }
            indent();
            line_has_code = true;
            const Position start = at;
            if (letter(c)) {
                do { advance(); } while (letter(peek()) || digit(peek()) || peek() == '\'');
                push(TokenKind::Name, start, at.offset - start.offset);
            } else if (starts("``")) {
                advance(); advance();
                while (at.offset < source.size() && !starts("``") && peek() != '\n' && peek() != '\r')
                    advance();
                if (!starts("``")) throw Error(start, "unterminated quoted identifier");
                if (at.offset == start.offset + 2) throw Error(start, "empty quoted identifier");
                advance(); advance();
                push(TokenKind::Name, start, at.offset - start.offset);
            } else if (digit(c)) {
                if (starts("0x") || starts("0X") || starts("0b") || starts("0B") ||
                    starts("0o") || starts("0O")) {
                    advance(); advance();
                    while (letter(peek()) || digit(peek())) advance();
                } else {
                    while (digit(peek()) || peek() == '_') advance();
                    if (peek() == '.' && peek(1) != '.') {
                        advance();
                        while (digit(peek()) || peek() == '_') advance();
                    }
                    if (peek() == 'e' || peek() == 'E') {
                        advance();
                        if (peek() == '+' || peek() == '-') advance();
                        while (digit(peek()) || peek() == '_') advance();
                    }
                    while (letter(peek()) || digit(peek())) advance();
                }
                push(TokenKind::Number, start, at.offset - start.offset);
            } else if (c == '"' || starts("@\"")) {
                const bool verbatim = c == '@';
                if (verbatim) advance();
                advance();
                while (at.offset < source.size() && peek() != '\n' && peek() != '\r') {
                    if (verbatim && starts("\"\"")) { advance(); advance(); continue; }
                    if (peek() == '"') break;
                    if (!verbatim && peek() == '\\') {
                        advance();
                        if (!peek() || peek() == '\n' || peek() == '\r') break;
                    }
                    advance();
                }
                if (peek() != '"') throw Error(start, "unterminated string literal");
                advance();
                push(TokenKind::String, start, at.offset - start.offset);
            } else {
                static constexpr std::string_view operators[] = {
                    "<<<", ">>>", "&&&", "|||", "^^^", "~~~", "[<", ">]", "[|", "|]",
                    "->", "<-", "<=", ">=", "<>", "&&", "||", "|>", "<|", "..",
                    "(", ")", "[", "]", "+", "-", "*", "/", "%", "=", "<", ">", ":",
                    ";", ",", ".", "|"
                };
                std::string_view op;
                for (auto candidate : operators) if (starts(candidate)) { op = candidate; break; }
                if (op.empty()) throw Error(at, "unsupported character in source");
                for (std::size_t i = 0; i < op.size(); ++i) advance();
                if (op == "(" || op == "[" || op == "[|" || op == "[<") {
                    if (delimiters.size() >= static_cast<std::size_t>(max_depth))
                        throw Error(start, "delimiters are too deeply nested");
                    delimiters.push_back(op);
                } else if (op == ")" || op == "]" || op == "|]" || op == ">]") {
                    const std::string_view expected = op == ")" ? "(" : op == "]" ? "[" :
                                                      op == "|]" ? "[|" : "[<";
                    if (delimiters.empty() || delimiters.back() != expected)
                        throw Error(start, "mismatched closing delimiter");
                    delimiters.pop_back();
                }
                push(TokenKind::Symbol, start, op.size());
            }
        }
        if (!delimiters.empty()) throw Error(at, "unclosed delimiter");
        if (line_has_code) push(TokenKind::Newline, at);
        while (indents.size() > 1) { indents.pop_back(); push(TokenKind::Dedent, at); }
        push(TokenKind::End, at);
        return std::move(tokens);
    }
};

std::string identifier(Token token) {
    if (token.text.substr(0, 2) == "``") return std::string(token.text.substr(2, token.text.size() - 4));
    return std::string(token.text);
}

ExprPtr node(Kind kind, Token token) { return std::make_unique<Expr>(kind, token); }

ExprPtr finish(ExprPtr value) {
    for (const auto& arg : value->args) value->depth = std::max(value->depth, arg->depth + 1);
    for (const auto& arm : value->arms) {
        value->depth = std::max(value->depth, arm.body->depth + 1);
        if (arm.guard) value->depth = std::max(value->depth, arm.guard->depth + 1);
    }
    if (value->depth > max_depth) throw Error(value->token.position, "expression is too deeply nested");
    return value;
}

ExprPtr literal(Token token, bool negative = false) {
    auto result = node(Kind::Literal, token);
    std::string text(token.text);
    if (text == "true" || text == "false") {
        result->value = Value{Type::Bool, text == "true" ? 1u : 0u};
        return result;
    }
    if (text.empty()) {
        result->value = Value{};
        return result;
    }
    if (text.front() == '_' || text.back() == '_' || text.find("__") != std::string::npos)
        throw Error(token.position, "invalid numeric separator");
    text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
    const bool based = text.size() > 2 && text[0] == '0' &&
                       (text[1] == 'x' || text[1] == 'X' || text[1] == 'b' ||
                        text[1] == 'B' || text[1] == 'o' || text[1] == 'O');
    if (!based && text.find_first_of(".eE") != std::string::npos) {
        if (negative) text.insert(text.begin(), '-');
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        double value = 0;
        stream >> value;
        if (!stream || !stream.eof() || !std::isfinite(value))
            throw Error(token.position, "invalid or out-of-range float literal");
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        result->value = Value{Type::Float, bits};
        return result;
    }
    const bool wide = text.back() == 'L';
    if (wide) text.pop_back();
    int base = 10;
    std::size_t begin = 0;
    if (based) {
        base = text[1] == 'x' || text[1] == 'X' ? 16 : text[1] == 'b' || text[1] == 'B' ? 2 : 8;
        begin = 2;
    }
    std::uint64_t value = 0;
    auto parsed = std::from_chars(text.data() + begin, text.data() + text.size(), value, base);
    const std::uint64_t limit = wide ? std::uint64_t{1} << 63 : std::uint64_t{1} << 31;
    const std::uint64_t maximum = based && !negative ?
        (wide ? std::numeric_limits<std::uint64_t>::max() : 0xffffffffu) : limit - (negative ? 0u : 1u);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > maximum)
        throw Error(token.position, "invalid or out-of-range integer literal");
    if (negative) value = std::uint64_t{0} - value;
    if (!wide) value &= 0xffffffffu;
    result->value = Value{wide ? Type::Int64 : Type::Int, value};
    return result;
}

class Parser {
    std::vector<Token> tokens;
    std::size_t cursor = 0;
    int nesting = 0;
    int pipe_id = 0;
    int group = 0;
    Program program;

    struct Depth {
        Parser& parser;
        explicit Depth(Parser& parser) : parser(parser) {
            if (++parser.nesting > max_depth)
                throw Error(parser.peek().position, "syntax is too deeply nested");
        }
        ~Depth() { --parser.nesting; }
    };

    Token peek(std::size_t n = 0) const { return tokens[std::min(cursor + n, tokens.size() - 1)]; }
    bool is(std::string_view text) const { return peek().text == text && !text.empty(); }
    bool kind(TokenKind kind) const { return peek().kind == kind; }
    Token take() { return tokens[cursor++]; }
    bool accept(std::string_view text) { if (!is(text)) return false; ++cursor; return true; }
    bool accept(TokenKind kind) { if (!this->kind(kind)) return false; ++cursor; return true; }
    Token require(std::string_view text) {
        if (!is(text)) throw Error(peek().position, "expected '" + std::string(text) + "'");
        return take();
    }
    Token name() {
        if (!kind(TokenKind::Name)) throw Error(peek().position, "expected an identifier");
        static constexpr std::string_view reserved[] = {
            "let", "rec", "and", "in", "if", "then", "else", "elif", "while", "for", "do",
            "to", "downto", "match", "with", "when", "true", "false", "fun", "function",
            "type", "module", "namespace", "open", "mutable", "private", "public", "extern",
            "abstract", "as", "assert", "base", "begin", "class", "default", "delegate", "done",
            "downcast", "end", "exception", "finally", "fixed", "global", "inherit", "inline",
            "interface", "internal", "lazy", "member", "new", "null", "of", "or", "override",
            "return", "static", "struct", "try", "upcast", "use", "val", "void", "yield",
            "asr", "land", "lor", "lsl", "lsr", "lxor", "mod", "sig", "break", "checked",
            "component", "const", "constraint", "continue", "event", "external", "include",
            "mixin", "parallel", "process", "protected", "pure", "sealed", "tailcall", "trait", "virtual"
        };
        for (auto word : reserved) if (is(word)) throw Error(peek().position, "expected a non-keyword identifier");
        return take();
    }
    void newlines() { while (accept(TokenKind::Newline)) {} }
    void endline() {
        if (!kind(TokenKind::End) && !accept(TokenKind::Newline))
            throw Error(peek().position, "expected end of declaration");
        newlines();
    }
    unsigned type(bool external_result = false) {
        if (is("void")) {
            const Token token = take();
            if (!external_result) throw Error(token.position, "'void' is supported only as an extern return type");
            return mask(Type::Unit);
        }
        const Token token = name();
        unsigned result = 0;
        if (token.text == "int" || token.text == "int32") result = mask(Type::Int);
        else if (token.text == "int64") result = mask(Type::Int64);
        else if (token.text == "float" || token.text == "float64" || token.text == "double") result = mask(Type::Float);
        else if (token.text == "bool") result = mask(Type::Bool);
        else if (token.text == "unit" || token.text == "void") result = mask(Type::Unit);
        else if (token.text == "string") result = mask(Type::String);
        else throw Error(token.position, "unsupported type '" + std::string(token.text) + "'");
        const bool bracket = accept("[");
        if (bracket || accept("array")) {
            if (bracket) require("]");
            if ((result & elements) == 0) throw Error(token.position, "arrays require int, int64, float or bool elements");
            result <<= 5;
        }
        return result;
    }
    static int precedence(std::string_view op) {
        if (op == "<-") return 1;
        if (op == "||") return 3;
        if (op == "&&") return 4;
        if (op == "=" || op == "<>" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
            op == "|>" || op == "<|" || op == "|||" || op == "&&&" || op == "<<<" || op == ">>>") return 5;
        if (op == "^^^") return 6;
        if (op == "+" || op == "-") return 10;
        if (op == "*" || op == "/" || op == "%") return 11;
        return 0;
    }
    bool argument() const {
        if (kind(TokenKind::Number) || kind(TokenKind::String) || is("(") || is("[|")) return true;
        if ((is("-") || is("+")) && cursor > 0 &&
            tokens[cursor - 1].position.offset + tokens[cursor - 1].text.size() < peek().position.offset &&
            peek().position.offset + 1 == peek(1).position.offset)
            return peek(1).kind == TokenKind::Number || peek(1).kind == TokenKind::Name || peek(1).text == "(";
        if (!kind(TokenKind::Name)) return false;
        static constexpr std::string_view stops[] = {
            "then", "else", "elif", "in", "do", "to", "downto", "with", "when", "and",
            "let", "if", "while", "for", "match", "fun", "function", "type", "module",
            "open", "extern"
        };
        for (auto word : stops) if (is(word)) return false;
        return true;
    }
    ExprPtr application_argument() {
        if (is("-") || is("+")) {
            const Token sign = take();
            if (kind(TokenKind::Number)) return literal(take(), sign.text == "-");
            auto value = node(Kind::Unary, sign);
            value->args.push_back(atom());
            return finish(std::move(value));
        }
        return atom();
    }
    ExprPtr apply(ExprPtr function, ExprPtr arg, Token at) {
        if (function->kind != Kind::Name && function->kind != Kind::Call)
            throw Error(at.position, "only named, fully applied functions are supported");
        if (function->args.size() >= max_parameters) throw Error(at.position, "too many function arguments");
        function->kind = Kind::Call;
        function->args.push_back(std::move(arg));
        return finish(std::move(function));
    }
    ExprPtr primary() {
        Depth depth(*this);
        const Token token = peek();
        if (accept("(")) {
            if (accept(")")) {
                auto value = node(Kind::Literal, token);
                value->value = Value{};
                return value;
            }
            auto value = sequence(false);
            if (accept(",")) {
                auto tuple = node(Kind::Tuple, token);
                tuple->args.push_back(std::move(value));
                do { tuple->args.push_back(expression()); } while (accept(","));
                value = finish(std::move(tuple));
            }
            if (accept(":")) {
                auto annotated = node(Kind::Annotate, token);
                annotated->annotation = type();
                annotated->args.push_back(std::move(value));
                value = finish(std::move(annotated));
            }
            require(")");
            return value;
        }
        if (kind(TokenKind::Number) || is("true") || is("false")) return literal(take());
        if (kind(TokenKind::String)) {
            auto value = node(Kind::String, take());
            value->name = string_literal(value->token);
            return value;
        }
        if (accept("[|")) {
            auto value = node(Kind::Array, token);
            if (!is("|]")) {
                do {
                    value->args.push_back(expression());
                    if (is("|]")) break;
                    require(";");
                } while (!is("|]"));
            }
            require("|]");
            return finish(std::move(value));
        }
        auto value = node(Kind::Name, name());
        value->name = identifier(value->token);
        return value;
    }
    ExprPtr atom() {
        auto value = primary();
        while (accept(".")) {
            const Token at = tokens[cursor - 1];
            if (accept("[")) {
                auto access = node(Kind::Index, at);
                access->args.push_back(std::move(value));
                access->args.push_back(expression());
                require("]");
                value = finish(std::move(access));
            } else {
                const auto member = identifier(name());
                if (member == "Length") {
                    auto length = node(Kind::Length, at);
                    length->args.push_back(std::move(value));
                    value = finish(std::move(length));
                } else if (value->kind == Kind::Name) {
                    value->name += "." + member;
                    value->flag = true;
                }
                else throw Error(at.position, "unsupported member '" + member + "'");
            }
        }
        if (value->kind == Kind::Name && value->name == "Array.zeroCreate" && accept("<")) {
            value->element_annotation = type();
            if ((value->element_annotation & elements) == 0)
                throw Error(value->token.position, "unsupported array element type");
            require(">");
        }
        return value;
    }
    ExprPtr prefix() {
        Depth depth(*this);
        const Token token = peek();
        if (is("-") || is("+") || is("~~~")) {
            take();
            if (token.text == "-" && kind(TokenKind::Number)) return literal(take(), true);
            auto value = node(Kind::Unary, token);
            value->args.push_back(prefix());
            return finish(std::move(value));
        }
        if (is("if") || is("elif")) {
            take();
            auto value = node(Kind::If, token);
            value->args.push_back(expression());
            require("then");
            value->args.push_back(body());
            const auto after_then = cursor;
            newlines();
            if (!is("else") && !is("elif")) cursor = after_then;
            if (accept("else")) value->args.push_back(body());
            else if (is("elif")) value->args.push_back(prefix());
            else {
                auto unit = node(Kind::Literal, token);
                unit->value = Value{};
                value->args.push_back(std::move(unit));
            }
            return finish(std::move(value));
        }
        if (accept("while")) {
            auto value = node(Kind::While, token);
            value->args.push_back(expression());
            require("do");
            value->args.push_back(body());
            return finish(std::move(value));
        }
        if (accept("do")) {
            auto value = node(Kind::Do, token);
            value->args.push_back(kind(TokenKind::Newline) ? body() : sequence(false));
            return finish(std::move(value));
        }
        if (accept("for")) {
            auto value = node(Kind::For, token);
            value->name = identifier(name());
            const bool range = accept("in");
            if (!range) require("=");
            value->args.push_back(expression());
            if (range) require("..");
            else if (accept("downto")) value->flag = true;
            else require("to");
            value->args.push_back(expression());
            require("do");
            value->args.push_back(body());
            return finish(std::move(value));
        }
        if (accept("match")) {
            auto value = node(Kind::Match, token);
            value->args.push_back(expression());
            require("with");
            newlines();
            const bool indented = accept(TokenKind::Indent);
            do {
                accept("|");
                Arm arm;
                do {
                    if (kind(TokenKind::String)) {
                        auto pattern = node(Kind::String, take());
                        pattern->name = string_literal(pattern->token);
                        arm.patterns.push_back(std::move(pattern));
                    } else if (kind(TokenKind::Number) || is("true") || is("false")) arm.patterns.push_back(literal(take()));
                    else if (accept("-")) {
                        if (!kind(TokenKind::Number)) throw Error(peek().position, "expected a numeric pattern");
                        arm.patterns.push_back(literal(take(), true));
                    } else {
                        auto pattern = node(Kind::Name, name());
                        pattern->name = identifier(pattern->token);
                        arm.patterns.push_back(std::move(pattern));
                    }
                } while (accept("|"));
                if (accept("when")) arm.guard = expression();
                require("->");
                arm.body = body();
                value->arms.push_back(std::move(arm));
                if (value->arms.size() > static_cast<std::size_t>(max_depth))
                    throw Error(token.position, "too many match arms");
                newlines();
            } while (is("|"));
            if (indented && !accept(TokenKind::Dedent))
                throw Error(peek().position, "expected end of match block");
            return finish(std::move(value));
        }
        if (accept("let")) {
            auto value = node(Kind::Let, token);
            value->flag = accept("mutable");
            if (is("rec")) throw Error(peek().position, "local recursive functions are not supported");
            value->name = identifier(name());
            if (accept(":")) value->annotation = type();
            require("=");
            value->args.push_back(body());
            if (accept("in")) value->args.push_back(sequence(false));
            return finish(std::move(value));
        }
        auto value = atom();
        while (argument()) value = apply(std::move(value), application_argument(), token);
        return value;
    }
    ExprPtr expression(int minimum = 1) {
        Depth depth(*this);
        auto left = prefix();
        for (;;) {
            const Token op = peek();
            const int priority = precedence(op.text);
            if (priority < minimum || priority == 0) break;
            take();
            auto right = expression(priority + ((op.text == "^^^" || op.text == "<-") ? 0 : 1));
            if (op.text == "|>" || op.text == "<|") {
                if (op.text == "<|") {
                    left = apply(std::move(left), std::move(right), op);
                } else {
                    const std::string temporary = "$pipe" + std::to_string(pipe_id++);
                    auto bind = node(Kind::Let, op);
                    bind->name = temporary;
                    bind->args.push_back(std::move(left));
                    auto reference = node(Kind::Name, op);
                    reference->name = temporary;
                    auto block = node(Kind::Block, op);
                    block->args.push_back(finish(std::move(bind)));
                    block->args.push_back(apply(std::move(right), std::move(reference), op));
                    left = finish(std::move(block));
                }
            } else {
                auto value = node(op.text == "<-" ? Kind::Assign : Kind::Binary, op);
                value->args.push_back(std::move(left));
                value->args.push_back(std::move(right));
                left = finish(std::move(value));
            }
        }
        return left;
    }
    ExprPtr sequence(bool lines) {
        auto value = expression();
        auto block = node(Kind::Block, value->token);
        block->args.push_back(std::move(value));
        for (;;) {
            const bool separator = accept(";") || (lines && accept(TokenKind::Newline));
            if (separator && lines) newlines();
            if (kind(TokenKind::Dedent) || kind(TokenKind::End) || is(")") || is("else") ||
                is("elif") || is("|") || is("in")) break;
            if (!separator) {
                const auto& previous = block->args.back();
                if (!lines || peek().position.line <= previous->token.position.line) break;
            }
            block->args.push_back(expression());
        }
        if (block->args.size() == 1 && block->args.front()->kind != Kind::Let)
            return std::move(block->args.front());
        return finish(std::move(block));
    }
    ExprPtr body() {
        if (accept(TokenKind::Newline)) {
            newlines();
            if (!accept(TokenKind::Indent)) throw Error(peek().position, "expected an indented expression");
            auto value = sequence(true);
            if (!accept(TokenKind::Dedent)) throw Error(peek().position, "expected end of indented expression");
            return value;
        }
        return expression();
    }
    Local parameter() {
        Local local;
        local.token = peek();
        if (accept("(")) {
            if (accept(")")) { local.annotation = mask(Type::Unit); return local; }
            local.token = name();
            local.name = identifier(local.token);
            if (accept(":")) local.annotation = type();
            require(")");
        } else {
            local.token = name();
            local.name = identifier(local.token);
        }
        return local;
    }
    void binding(bool recursive) {
        const bool private_value = accept("private");
        accept("public");
        const bool mutable_value = accept("mutable");
        const Token token = name();
        const std::string binding_name = identifier(token);
        std::vector<Local> parameters;
        while (kind(TokenKind::Name) || is("(")) {
            parameters.push_back(parameter());
            if (parameters.size() > max_parameters) throw Error(token.position, "too many parameters");
        }
        unsigned annotation = all_types;
        if (accept(":")) annotation = type();
        require("=");
        auto value = body();
        if (parameters.empty()) {
            if (recursive) throw Error(token.position, "recursive values are not supported");
            Global global;
            global.token = token;
            global.name = binding_name;
            global.body = std::move(value);
            global.annotation = annotation;
            global.group = group;
            global.mutable_value = mutable_value;
            global.private_value = private_value;
            program.globals.push_back(std::move(global));
        } else {
            if (mutable_value) throw Error(token.position, "functions cannot be mutable");
            Function function;
            function.token = token;
            function.name = binding_name;
            function.parameters = parameters.size();
            function.locals = std::move(parameters);
            function.body = std::move(value);
            function.annotation = annotation;
            function.group = group;
            function.recursive = recursive;
            function.private_value = private_value;
            program.functions.push_back(std::move(function));
        }
    }
    std::string quoted() {
        if (!kind(TokenKind::String)) throw Error(peek().position, "expected a string literal");
        const Token token = take();
        const bool verbatim = token.text.front() == '@';
        if (!verbatim && token.text.find('\\') != std::string_view::npos)
            throw Error(token.position, "escapes are not supported in import names");
        const auto contents = token.text.substr(verbatim ? 2 : 1, token.text.size() - (verbatim ? 3 : 2));
        std::string result;
        for (std::size_t i = 0; i < contents.size(); ++i) {
            if (verbatim && contents[i] == '"') ++i;
            result.push_back(contents[i]);
        }
        return result;
    }
    void import() {
        require("[<");
        const Token attribute = name();
        if (attribute.text != "DllImport") throw Error(attribute.position, "only DllImport attributes are supported");
        require("(");
        const std::string module = quoted();
        std::optional<std::string> entry;
        if (accept(",")) {
            require("EntryPoint");
            require("=");
            entry = quoted();
        }
        require(")");
        require(">]");
        newlines();
        require("extern");
        Function function;
        function.imported = true;
        function.tupled = true;
        function.private_value = accept("private");
        function.annotation = type(true);
        function.token = name();
        function.name = identifier(function.token);
        function.import_module = module;
        function.import_name = entry ? *entry : function.name;
        function.group = group;
        require("(");
        if (!is(")")) {
            do {
                Local param;
                param.annotation = type();
                if (param.annotation == mask(Type::Unit))
                    throw Error(peek().position, "use empty parentheses instead of unit extern parameters");
                param.token = name();
                param.name = identifier(param.token);
                function.locals.push_back(std::move(param));
                if (function.locals.size() > max_parameters)
                    throw Error(function.token.position, "too many parameters");
            } while (accept(","));
        } else {
            Local unit;
            unit.token = function.token;
            unit.annotation = mask(Type::Unit);
            function.locals.push_back(unit);
        }
        require(")");
        function.parameters = function.locals.size();
        program.functions.push_back(std::move(function));
    }
public:
    explicit Parser(std::vector<Token> tokens) : tokens(std::move(tokens)) {}
    Program run() {
        newlines();
        if (accept("module")) {
            program.module = identifier(name());
            while (accept(".")) program.module += "." + identifier(name());
            endline();
        }
        while (!kind(TokenKind::End)) {
            ++group;
            if (accept("open")) {
                std::string path = identifier(name());
                while (accept(".")) path += "." + identifier(name());
                if (path != "System" && path != "System.Runtime.InteropServices")
                    throw Error(peek().position, "unsupported open declaration '" + path + "'");
                endline();
                continue;
            }
            if (is("[<")) import();
            else {
                require("let");
                const bool recursive = accept("rec");
                binding(recursive);
                newlines();
                while (accept("and")) {
                    if (!recursive) throw Error(peek().position, "'and' requires 'let rec'");
                    binding(true);
                    newlines();
                }
            }
            if (kind(TokenKind::Newline)) newlines();
            else if (!kind(TokenKind::End) && !is("let") && !is("[<") && !is("open"))
                throw Error(peek().position, "expected a top-level declaration");
            if (!kind(TokenKind::End)) {
                std::size_t previous = cursor;
                while (previous > 0 && (tokens[previous - 1].kind == TokenKind::Newline ||
                       tokens[previous - 1].kind == TokenKind::Indent || tokens[previous - 1].kind == TokenKind::Dedent))
                    --previous;
                if (peek().position.column != 1 ||
                    (previous && peek().position.line <= tokens[previous - 1].position.line))
                    throw Error(peek().position, "top-level declarations must start on separate, unindented lines");
            }
        }
        return std::move(program);
    }
};

} // namespace

std::string type_name(Type type) {
    switch (type) {
    case Type::Unit: return "unit";
    case Type::Bool: return "bool";
    case Type::Int: return "int";
    case Type::Int64: return "int64";
    case Type::Float: return "float";
    case Type::String: return "string";
    case Type::BoolArray: return "bool array";
    case Type::IntArray: return "int array";
    case Type::Int64Array: return "int64 array";
    case Type::FloatArray: return "float array";
    }
    throw std::logic_error("invalid type");
}

int Types::root(int id) {
    int result = id;
    while (variables.at(static_cast<std::size_t>(result)).parent != result)
        result = variables[static_cast<std::size_t>(result)].parent;
    while (id != result) {
        auto& variable = variables[static_cast<std::size_t>(id)];
        const int next = variable.parent;
        variable.parent = result;
        id = next;
    }
    return result;
}

int Types::add(unsigned choices) {
    const int id = static_cast<int>(variables.size());
    variables.push_back({id, choices, {}, 0});
    return id;
}

void Types::constrain(int id, unsigned choices, Position at) {
    auto& variable = variables[static_cast<std::size_t>(root(id))];
    const unsigned intersection = variable.choices & choices;
    if (!intersection) throw Error(at, "type mismatch");
    if (variable.choices != intersection) {
        variable.choices = intersection;
        propagate(variable.relations);
    }
}

void Types::unify(int a, int b, Position at) {
    a = root(a); b = root(b);
    if (a == b) return;
    if (variables[static_cast<std::size_t>(a)].rank < variables[static_cast<std::size_t>(b)].rank) std::swap(a, b);
    auto& target = variables[static_cast<std::size_t>(a)];
    auto& source = variables[static_cast<std::size_t>(b)];
    const unsigned intersection = target.choices & source.choices;
    if (!intersection) throw Error(at, "type mismatch");
    const bool changed = target.choices != intersection;
    target.choices = intersection;
    if (target.rank == source.rank) ++target.rank;
    variables[static_cast<std::size_t>(b)].parent = a;
    auto moved = std::move(source.relations);
    target.relations.insert(target.relations.end(), moved.begin(), moved.end());
    propagate(changed ? target.relations : std::move(moved));
}

void Types::array_of(int array, int element, Position at) {
    constrain(array, arrays, at);
    constrain(element, elements, at);
    variables[static_cast<std::size_t>(root(array))].relations.push_back(relations.size());
    variables[static_cast<std::size_t>(root(element))].relations.push_back(relations.size());
    relations.push_back({array, element, at});
    propagate({relations.size() - 1});
}

void Types::propagate(std::vector<std::size_t> pending) {
    while (!pending.empty()) {
        const auto relation = relations[pending.back()];
        pending.pop_back();
        auto& array = variables[static_cast<std::size_t>(root(relation.array))];
        auto& element = variables[static_cast<std::size_t>(root(relation.element))];
        const unsigned allowed = array.choices & (element.choices << 5);
        if (!allowed) throw Error(relation.at, "array element type mismatch");
        if (array.choices != allowed) {
            array.choices = allowed;
            pending.insert(pending.end(), array.relations.begin(), array.relations.end());
        }
        if (element.choices != (allowed >> 5)) {
            element.choices = allowed >> 5;
            pending.insert(pending.end(), element.relations.begin(), element.relations.end());
        }
    }
}

Type Types::get(int id, Position at) {
    auto& variable = variables[static_cast<std::size_t>(root(id))];
    if ((variable.choices & (variable.choices - 1u)) != 0) {
        if ((variable.choices & ~(elements | mask(Type::String))) == 0 && (variable.choices & mask(Type::Int)) != 0)
            variable.choices = mask(Type::Int);
        else if ((variable.choices & ~arrays) == 0 && (variable.choices & mask(Type::IntArray)) != 0)
            variable.choices = mask(Type::IntArray);
        else throw Error(at, "ambiguous type; add a parameter or return type annotation");
        propagate(variable.relations);
    }
    for (auto type : {Type::Unit, Type::Bool, Type::Int, Type::Int64, Type::Float, Type::String,
                      Type::BoolArray, Type::IntArray, Type::Int64Array, Type::FloatArray})
        if (variable.choices == mask(type)) return type;
    throw std::logic_error("empty type constraint");
}

Program parse(std::string_view source) { return Parser(Lexer(source).run()).run(); }

Output compile(std::string_view source, const Options& options) {
    auto program = parse(source);
    check(program);
    return emit(program, options);
}

} // namespace fsw

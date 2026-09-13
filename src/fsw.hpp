#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fsw {

constexpr std::size_t max_source_bytes = 16 * 1024 * 1024;
constexpr int max_depth = 256;
constexpr std::size_t max_parameters = 256;
constexpr std::size_t max_string_bytes = 64 * 1024 * 1024;

struct Position {
    std::size_t offset = 0;
    int line = 1;
    int column = 1;
};

struct Error : std::runtime_error {
    Position position;
    Error(Position at, const std::string& message) : std::runtime_error(message), position(at) {}
};

enum class TokenKind { End, Newline, Indent, Dedent, Name, Number, String, Symbol };

struct Token {
    TokenKind kind = TokenKind::End;
    std::string_view text;
    Position position;
};

enum class Type : unsigned { Unit, Bool, Int, Int64, Float, String, BoolArray, IntArray, Int64Array, FloatArray };
constexpr unsigned mask(Type type) { return 1u << static_cast<unsigned>(type); }
constexpr unsigned integers = mask(Type::Int) | mask(Type::Int64);
constexpr unsigned numbers = integers | mask(Type::Float);
constexpr unsigned elements = numbers | mask(Type::Bool);
constexpr unsigned arrays = elements << 5;
constexpr unsigned references = arrays | mask(Type::String);
constexpr unsigned all_types = elements | mask(Type::Unit) | references;
constexpr bool is_array(Type type) { return (mask(type) & arrays) != 0; }
constexpr bool is_reference(Type type) { return (mask(type) & references) != 0; }
constexpr unsigned element_size(Type type) { return type == Type::String ? 2 : type == Type::Int64Array || type == Type::FloatArray ? 8 : 4; }
std::string type_name(Type type);

struct Value {
    Type type = Type::Unit;
    std::uint64_t bits = 0;
};

class Types {
    struct Variable { int parent; unsigned choices; std::vector<std::size_t> relations; unsigned rank; };
    struct Relation { int array; int element; Position at; };
    std::vector<Variable> variables;
    std::vector<Relation> relations;
    int root(int id);
    void propagate(std::vector<std::size_t> pending);
public:
    int add(unsigned choices = all_types);
    void constrain(int id, unsigned choices, Position at);
    void unify(int a, int b, Position at);
    void array_of(int array, int element, Position at);
    Type get(int id, Position at);
};

enum class Kind {
    Literal, Name, Unary, Binary, Call, If, Block, Let, Assign,
    While, For, Match, Tuple, Annotate, String, Array, Index, Length, Do, Lambda
};

enum class Builtin {
    Not, Int, Int64, Float, Abs, Min, Max, Sqrt, Floor, Ceil, Truncate, Ignore,
    ArrayZero, ArrayCreate, ArrayLength, StringLength
};

std::int64_t signed_value(Value value);
double float_value(Value value);

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Arm {
    std::vector<ExprPtr> patterns;
    ExprPtr guard;
    ExprPtr body;
};

struct Expr {
    Kind kind;
    Token token;
    std::string name;
    std::vector<ExprPtr> args;
    std::vector<Arm> arms;
    std::optional<Value> value;
    unsigned annotation = all_types;
    unsigned element_annotation = elements;
    int type = -1;
    int index = -1;
    int depth = 1;
    bool flag = false;
    bool global = false;
    Expr(Kind kind, Token token) : kind(kind), token(token), name(token.text) {}
};

ExprPtr node(Kind kind, Token token);
ExprPtr finish(ExprPtr value);

struct Local {
    std::string name;
    Token token;
    unsigned annotation = all_types;
    int type = -1;
    bool mutable_value = false;
};

struct Function {
    Token token;
    std::string name;
    std::vector<Local> locals;
    std::size_t parameters = 0;
    unsigned annotation = all_types;
    int result = -1;
    int group = 0;
    bool recursive = false;
    bool private_value = false;
    bool imported = false;
    bool tupled = false;
    std::string import_module;
    std::string import_name;
    ExprPtr body;
};

struct Global {
    Token token;
    std::string name;
    ExprPtr body;
    unsigned annotation = all_types;
    int type = -1;
    int group = 0;
    bool mutable_value = false;
    bool private_value = false;
};

struct Program {
    std::string module;
    Types types;
    std::vector<Function> functions;
    std::vector<Global> globals;
    std::vector<std::string> strings;
};

struct Options {
    bool optimize = true;
    std::vector<std::string> exports;
    unsigned max_memory_pages = 256;
};

struct Output {
    std::vector<std::uint8_t> bytes;
    std::size_t functions = 0;
};

Program parse(std::string_view source);
void check(Program& program);
Output emit(Program& program, const Options& options = {});
Output compile(std::string_view source, const Options& options = {});

} // namespace fsw

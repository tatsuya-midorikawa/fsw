#include "fsw.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace fsw {
namespace {

struct Bytes {
    std::vector<std::uint8_t> data;
    void byte(std::uint8_t value) { data.push_back(value); }
    void append(const std::vector<std::uint8_t>& value) { data.insert(data.end(), value.begin(), value.end()); }
    void u32(std::size_t value) {
        do {
            auto next = static_cast<std::uint8_t>(value & 0x7f);
            value >>= 7;
            byte(static_cast<std::uint8_t>(next | (value ? 0x80 : 0)));
        } while (value);
    }
    void s64(std::int64_t value) {
        for (;;) {
            const auto next = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) & 0x7f);
            const bool negative = value < 0;
            const bool remainder = value % 128 != 0;
            value /= 128;
            if (negative && remainder) --value;
            const bool done = (value == 0 && (next & 0x40) == 0) || (value == -1 && (next & 0x40) != 0);
            byte(static_cast<std::uint8_t>(next | (done ? 0 : 0x80)));
            if (done) return;
        }
    }
    void text(const std::string& value) {
        u32(value.size());
        data.insert(data.end(), value.begin(), value.end());
    }
    void section(std::uint8_t id, const Bytes& contents) {
        byte(id); u32(contents.data.size()); append(contents.data);
    }
};

std::uint8_t wasm_type(Type type) {
    if (type == Type::Unit) return 0x40;
    if (type == Type::Int64) return 0x7e;
    if (type == Type::Float) return 0x7c;
    return 0x7f;
}

void constant(Bytes& out, Value value) {
    if (value.type == Type::Unit) return;
    if (value.type == Type::Float) {
        out.byte(0x44);
        for (unsigned shift = 0; shift < 64; shift += 8)
            out.byte(static_cast<std::uint8_t>(value.bits >> shift));
    } else {
        out.byte(value.type == Type::Int64 ? 0x42 : 0x41);
        out.s64(signed_value(value));
    }
}

std::uint8_t operation(const std::string& op, Type type) {
    struct Entry { const char* name; std::uint8_t i32; std::uint8_t i64; std::uint8_t f64; };
    static constexpr Entry operations[] = {
        {"=", 0x46, 0x51, 0x61}, {"<>", 0x47, 0x52, 0x62},
        {"<", 0x48, 0x53, 0x63}, {">", 0x4a, 0x55, 0x64},
        {"<=", 0x4c, 0x57, 0x65}, {">=", 0x4e, 0x59, 0x66},
        {"+", 0x6a, 0x7c, 0xa0}, {"-", 0x6b, 0x7d, 0xa1},
        {"*", 0x6c, 0x7e, 0xa2}, {"/", 0x6d, 0x7f, 0xa3},
        {"%", 0x6f, 0x81, 0}, {"&&&", 0x71, 0x83, 0}, {"|||", 0x72, 0x84, 0},
        {"^^^", 0x73, 0x85, 0}, {"<<<", 0x74, 0x86, 0}, {">>>", 0x75, 0x87, 0}
    };
    for (auto entry : operations) if (op == entry.name) {
        const auto code = type == Type::Float ? entry.f64 : type == Type::Int64 ? entry.i64 : entry.i32;
        if (code) return code;
        break;
    }
    throw std::logic_error("invalid typed operation: " + op);
}

enum class Helper : std::size_t { Allocate, Check, Concat, Compare };

Bytes runtime(Helper helper, const std::array<int, 4>& indices, std::size_t heap, unsigned max_pages) {
    Bytes code;
    const unsigned local_count = helper == Helper::Allocate || helper == Helper::Concat ? 3 :
                                 helper == Helper::Compare ? 5 : 0;
    code.u32(local_count ? 1 : 0);
    if (local_count) { code.u32(local_count); code.byte(0x7f); }
    auto get = [&](unsigned index) { code.byte(0x20); code.u32(index); };
    auto set = [&](unsigned index) { code.byte(0x21); code.u32(index); };
    auto number = [&](std::int64_t value) { code.byte(0x41); code.s64(value); };
    auto load = [&](unsigned offset = 0) { code.byte(0x28); code.u32(2); code.u32(offset); };
    auto store = [&](unsigned offset = 0) { code.byte(0x36); code.u32(2); code.u32(offset); };
    auto global = [&] { code.byte(0x23); code.u32(heap); };
    auto call = [&](Helper target) { code.byte(0x10); code.u32(static_cast<std::size_t>(indices[static_cast<std::size_t>(target)])); };
    auto fail = [&] { code.byte(0x04); code.byte(0x40); code.byte(0x00); code.byte(0x0b); };
    auto check_string = [&](unsigned index) {
        get(index); number(static_cast<unsigned>(Type::String)); number(2); call(Helper::Check); code.byte(0x1a);
    };
    if (helper == Helper::Check) {
        get(0); number(8); code.byte(0x49); fail();
        get(0); number(7); code.byte(0x71); fail();
        get(0); global(); number(8); code.byte(0x6b); code.byte(0x4b); fail();
        get(0); load(4); get(1); code.byte(0x47); fail();
        get(0); load(); number(0); code.byte(0x48); fail();
        get(0); load(); global(); get(0); code.byte(0x6b); number(8); code.byte(0x6b);
        get(2); code.byte(0x6e); code.byte(0x4b); fail();
        get(0);
    } else if (helper == Helper::Allocate) {
        // ponytail: escaping allocations retain bump-heap storage; add tracing GC for long-lived object graphs.
        get(0); number(0); code.byte(0x48); fail();
        global(); set(3);
        get(3); number(static_cast<std::int64_t>(max_pages) * 65536 - 8); code.byte(0x4b); fail();
        get(0); number(static_cast<std::int64_t>(max_pages) * 65536); get(3); code.byte(0x6b);
        number(8); code.byte(0x6b); get(2); code.byte(0x6e); code.byte(0x4b); fail();
        get(3); number(8); code.byte(0x6a); get(0); get(2); code.byte(0x6c); code.byte(0x6a);
        number(7); code.byte(0x6a); number(-8); code.byte(0x71); set(4);
        get(4); number(65535); code.byte(0x6a); number(16); code.byte(0x76); set(5);
        get(5); code.byte(0x3f); code.byte(0); code.byte(0x4b);
        code.byte(0x04); code.byte(0x40);
        get(5); code.byte(0x3f); code.byte(0); code.byte(0x6b);
        code.byte(0x40); code.byte(0); number(-1); code.byte(0x46); fail();
        code.byte(0x0b);
        get(3); get(0); store();
        get(3); get(1); store(4);
        get(3); number(8); code.byte(0x6a); number(0); get(0); get(2); code.byte(0x6c);
        code.byte(0xfc); code.u32(11); code.byte(0);
        get(4); code.byte(0x24); code.u32(heap);
        get(3);
    } else if (helper == Helper::Concat) {
        check_string(0); check_string(1);
        get(0); load(); set(2); get(1); load(); set(3);
        get(2); get(3); code.byte(0x6a); number(static_cast<unsigned>(Type::String)); number(2);
        call(Helper::Allocate); set(4);
        get(4); number(8); code.byte(0x6a); get(0); number(8); code.byte(0x6a);
        get(2); number(1); code.byte(0x74);
        code.byte(0xfc); code.u32(10); code.byte(0); code.byte(0);
        get(4); number(8); code.byte(0x6a); get(2); number(1); code.byte(0x74); code.byte(0x6a);
        get(1); number(8); code.byte(0x6a); get(3); number(1); code.byte(0x74);
        code.byte(0xfc); code.u32(10); code.byte(0); code.byte(0);
        get(4);
    } else {
        check_string(0); check_string(1);
        get(0); load(); set(2); get(1); load(); set(3);
        code.byte(0x02); code.byte(0x40); code.byte(0x03); code.byte(0x40);
        get(4); get(2); code.byte(0x4f); get(4); get(3); code.byte(0x4f); code.byte(0x72);
        code.byte(0x0d); code.u32(1);
        for (unsigned i = 0; i < 2; ++i) {
            get(i); get(4); number(1); code.byte(0x74); code.byte(0x6a);
            code.byte(0x2f); code.u32(1); code.u32(8); set(5 + i);
        }
        get(5); get(6); code.byte(0x47);
        code.byte(0x04); code.byte(0x40);
        get(5); get(6); code.byte(0x49);
        code.byte(0x04); code.byte(0x7f); number(-1); code.byte(0x05); number(1); code.byte(0x0b);
        code.byte(0x0f); code.byte(0x0b);
        get(4); number(1); code.byte(0x6a); set(4); code.byte(0x0c); code.u32(0);
        code.byte(0x0b); code.byte(0x0b);
        get(2); get(3); code.byte(0x46);
        code.byte(0x04); code.byte(0x7f); number(0); code.byte(0x05);
        get(2); get(3); code.byte(0x49);
        code.byte(0x04); code.byte(0x7f); number(-1); code.byte(0x05); number(1); code.byte(0x0b);
        code.byte(0x0b);
    }
    code.byte(0x0b);
    return code;
}

class Emitter {
    Program& program;
    const Options& options;
    std::vector<int> function_indices;
    std::vector<int> global_indices;
    std::vector<bool> live_functions;
    std::vector<bool> live_globals;
    std::set<std::string> exports;
    std::vector<std::size_t> pending;
    bool memory = false;
    std::array<bool, 4> helpers{};
    std::array<int, 4> helper_indices{{-1, -1, -1, -1}};
    std::vector<bool> live_strings;
    std::vector<std::size_t> string_offsets;
    std::size_t heap_start = 8;
    std::size_t heap_global = 0;

    void need(Helper helper) {
        memory = true;
        helpers[static_cast<std::size_t>(helper)] = true;
        if (helper == Helper::Concat) need(Helper::Allocate);
        if (helper == Helper::Concat || helper == Helper::Compare) need(Helper::Check);
    }
    void mark_value(Value value) {
        if (value.type == Type::String) {
            live_strings.at(static_cast<std::size_t>(value.bits)) = true;
            memory = true;
        }
    }
    void value(Bytes& code, Value value) {
        if (value.type == Type::String) value = {Type::Int, string_offsets.at(static_cast<std::size_t>(value.bits))};
        constant(code, value);
    }

    Type type(const Expr& expr) { return program.types.get(expr.type, expr.token.position); }
    Type type(const Local& local) { return program.types.get(local.type, local.token.position); }

    void mark_function(std::size_t index) {
        if (live_functions[index]) return;
        live_functions[index] = true;
        pending.push_back(index);
    }
    void mark(const Expr& expr) {
        if (expr.value && (options.optimize || expr.kind == Kind::String || expr.kind == Kind::Literal)) {
            mark_value(*expr.value);
            return;
        }
        if (expr.kind == Kind::Array) need(Helper::Allocate);
        if (expr.kind == Kind::Index || expr.kind == Kind::Length) need(Helper::Check);
        if (expr.kind == Kind::Binary && type(*expr.args[0]) == Type::String)
            need(expr.name == "+" ? Helper::Concat : Helper::Compare);
        if (expr.kind == Kind::Match && type(*expr.args[0]) == Type::String) need(Helper::Compare);
        if (expr.kind == Kind::Call && expr.index < 0) {
            const auto builtin = static_cast<Builtin>(-1 - expr.index);
            if (builtin == Builtin::ArrayZero || builtin == Builtin::ArrayCreate) need(Helper::Allocate);
            if (builtin == Builtin::ArrayLength || builtin == Builtin::StringLength) need(Helper::Check);
        }
        if (expr.kind == Kind::Call && expr.index >= 0) mark_function(static_cast<std::size_t>(expr.index));
        if (expr.kind == Kind::Name && expr.global) live_globals[static_cast<std::size_t>(expr.index)] = true;
        if (options.optimize && expr.kind == Kind::If && expr.args[0]->value) {
            mark(*expr.args[expr.args[0]->value->bits ? 1 : 2]);
            return;
        }
        for (const auto& arg : expr.args) mark(*arg);
        for (const auto& arm : expr.arms) {
            for (const auto& pattern : arm.patterns) if (pattern->value) mark_value(*pattern->value);
            if (arm.guard) mark(*arm.guard);
            mark(*arm.body);
        }
    }
    bool tail_call(const Expr& expr, int index) {
        if (expr.value) return false;
        if (expr.kind == Kind::Call) return expr.index == index;
        if (expr.kind == Kind::If) return tail_call(*expr.args[1], index) || tail_call(*expr.args[2], index);
        if (expr.kind == Kind::Block || expr.kind == Kind::Annotate)
            return tail_call(*expr.args.back(), index);
        if (expr.kind == Kind::Let && expr.args.size() == 2) return tail_call(*expr.args[1], index);
        if (expr.kind == Kind::Match)
            for (const auto& arm : expr.arms) if (tail_call(*arm.body, index)) return true;
        return false;
    }
    bool addition_call(const Expr& expr, int index) {
        return expr.kind == Kind::Binary && expr.name == "+" && (mask(type(expr)) & integers) != 0 &&
               expr.args[1]->kind == Kind::Call && expr.args[1]->index == index;
    }
    bool tail_addition(const Expr& expr, int index) {
        if (expr.value) return false;
        if (addition_call(expr, index)) return true;
        if (expr.kind == Kind::If) return tail_addition(*expr.args[1], index) || tail_addition(*expr.args[2], index);
        if (expr.kind == Kind::Block || expr.kind == Kind::Annotate)
            return tail_addition(*expr.args.back(), index);
        if (expr.kind == Kind::Let && expr.args.size() == 2) return tail_addition(*expr.args[1], index);
        if (expr.kind == Kind::Match)
            for (const auto& arm : expr.arms) if (tail_addition(*arm.body, index)) return true;
        return false;
    }
    bool leaf_region(const Expr& expr, int index) {
        if (expr.kind == Kind::Call && expr.index >= 0 && expr.index != index) return false;
        if (expr.kind == Kind::Assign && expr.args[0]->kind == Kind::Name && expr.args[0]->global) return false;
        for (const auto& arg : expr.args) if (!leaf_region(*arg, index)) return false;
        for (const auto& arm : expr.arms) {
            if (arm.guard && !leaf_region(*arm.guard, index)) return false;
            if (!leaf_region(*arm.body, index)) return false;
        }
        return true;
    }
    bool allocates(const Expr& expr) {
        if (expr.value) return false;
        if (expr.kind == Kind::Array) return true;
        if (expr.kind == Kind::Call && expr.index < 0) {
            const auto builtin = static_cast<Builtin>(-1 - expr.index);
            if (builtin == Builtin::ArrayZero || builtin == Builtin::ArrayCreate) return true;
        }
        if (expr.kind == Kind::Binary && expr.name == "+" && type(expr) == Type::String) return true;
        for (const auto& arg : expr.args) if (allocates(*arg)) return true;
        for (const auto& arm : expr.arms) {
            if (arm.guard && allocates(*arm.guard)) return true;
            if (allocates(*arm.body)) return true;
        }
        return false;
    }

    class Body {
        Emitter& emitter;
        Function& function;
        int index;
        Bytes code;
        std::vector<int> locals;
        std::vector<Type> local_types;
        std::size_t parameter_count = 0;
        int controls = 0;
        int tail_label = -1;
        int accumulator = -1;
        int heap_mark = -1;
        bool safe_region = false;
        bool reference_parameters = false;

        Type type(const Expr& expr) { return emitter.type(expr); }
        int temporary(Type type) {
            const int result = static_cast<int>(local_types.size());
            local_types.push_back(type);
            return result;
        }
        void get(int local) { code.byte(0x20); code.u32(static_cast<std::size_t>(local)); }
        void set(int local) { code.byte(0x21); code.u32(static_cast<std::size_t>(local)); }
        void begin(std::uint8_t op, Type result = Type::Unit) {
            code.byte(op); code.byte(wasm_type(result)); ++controls;
        }
        void end() { code.byte(0x0b); --controls; }
        void branch(unsigned depth, bool conditional = false) {
            code.byte(conditional ? 0x0d : 0x0c); code.u32(depth);
        }
        void zero(Type type) { constant(code, {type, 0}); }
        void one(Type type) { constant(code, {type, 1}); }
        void normalize_bool() { code.byte(0x45); code.byte(0x45); }
        void helper(Helper helper) {
            code.byte(0x10);
            code.u32(static_cast<std::size_t>(emitter.helper_indices[static_cast<std::size_t>(helper)]));
        }
        void checked_reference(Type type) {
            constant(code, {Type::Int, static_cast<unsigned>(type)});
            constant(code, {Type::Int, element_size(type)});
            helper(Helper::Check);
        }
        bool validated_parameter(const Expr& expr) const {
            return safe_region && expr.kind == Kind::Name && !expr.global && expr.index >= 0 &&
                   static_cast<std::size_t>(expr.index) < function.parameters;
        }
        void restore_heap() {
            get(heap_mark); code.byte(0x24); code.u32(emitter.heap_global);
        }
        void allocate(Type type) {
            constant(code, {Type::Int, static_cast<unsigned>(type)});
            constant(code, {Type::Int, element_size(type)});
            helper(Helper::Allocate);
        }
        void load(Type type, unsigned offset = 0) {
            code.byte(type == Type::Int64 ? 0x29 : type == Type::Float ? 0x2b : 0x28);
            code.u32(type == Type::Int64 || type == Type::Float ? 3 : 2); code.u32(offset);
            if (type == Type::Bool) normalize_bool();
        }
        void store(Type type, unsigned offset = 0) {
            code.byte(type == Type::Int64 ? 0x37 : type == Type::Float ? 0x39 : 0x36);
            code.u32(type == Type::Int64 || type == Type::Float ? 3 : 2); code.u32(offset);
        }
        void index_access(Expr& expr, Expr* assigned = nullptr) {
            const Type array_type = type(*expr.args[0]);
            const Type element = type(expr);
            const int pointer = temporary(Type::Int), index = temporary(Type::Int);
            expression(*expr.args[0]); set(pointer);
            expression(*expr.args[1]); set(index);
            int value = -1;
            if (assigned) {
                value = temporary(element);
                expression(*assigned); set(value);
            }
            if (!validated_parameter(*expr.args[0])) {
                get(pointer); checked_reference(array_type); set(pointer);
            }
            get(index); get(pointer); load(Type::Int);
            code.byte(0x4f); begin(0x04); code.byte(0x00); end();
            get(pointer); constant(code, {Type::Int, 8}); code.byte(0x6a);
            get(index); constant(code, {Type::Int, element_size(array_type) == 8 ? 3u : 2u});
            code.byte(0x74); code.byte(0x6a);
            if (assigned) { get(value); store(element); }
            else load(element);
        }

        void builtin(Expr& expr) {
            const auto operation_id = static_cast<Builtin>(-1 - expr.index);
            const Type arg_type = type(*expr.args[0]);
            expression(*expr.args[0]);
            if (operation_id == Builtin::ArrayLength || operation_id == Builtin::StringLength) {
                if (!validated_parameter(*expr.args[0])) checked_reference(arg_type);
                load(Type::Int);
                return;
            }
            if (operation_id == Builtin::ArrayZero) { allocate(type(expr)); return; }
            if (operation_id == Builtin::ArrayCreate) {
                const Type element = type(*expr.args[1]);
                const int length = temporary(Type::Int), initial = temporary(element);
                const int pointer = temporary(Type::Int), index = temporary(Type::Int);
                set(length); expression(*expr.args[1]); set(initial);
                get(length); allocate(type(expr)); set(pointer);
                zero(Type::Int); set(index);
                begin(0x02); begin(0x03);
                get(index); get(length); code.byte(0x4f); branch(1, true);
                get(pointer); get(index); constant(code, {Type::Int, element_size(type(expr)) == 8 ? 3u : 2u});
                code.byte(0x74); code.byte(0x6a); get(initial); store(element, 8);
                get(index); one(Type::Int); code.byte(0x6a); set(index); branch(0);
                end(); end();
                get(pointer);
                return;
            }
            if (operation_id == Builtin::Ignore) {
                if (arg_type != Type::Unit) code.byte(0x1a);
                return;
            }
            if (operation_id == Builtin::Not) { code.byte(0x45); return; }
            if (operation_id == Builtin::Int || operation_id == Builtin::Int64 || operation_id == Builtin::Float) {
                const Type target = type(expr);
                if (arg_type == target) return;
                if (arg_type == Type::Float) {
                    code.byte(0xfc); code.u32(target == Type::Int ? 2 : 6);
                } else if (target == Type::Float) code.byte(arg_type == Type::Int ? 0xb7 : 0xb9);
                else code.byte(target == Type::Int ? 0xa7 : 0xac);
                return;
            }
            if (operation_id == Builtin::Abs) {
                if (arg_type == Type::Float) { code.byte(0x99); return; }
                const int temp = temporary(arg_type);
                code.byte(0x22); code.u32(static_cast<std::size_t>(temp));
                constant(code, {arg_type, arg_type == Type::Int ? std::uint64_t{1} << 31 : std::uint64_t{1} << 63});
                code.byte(operation("=", arg_type));
                begin(0x04); code.byte(0x00); end();
                zero(arg_type); get(temp); code.byte(operation("-", arg_type));
                get(temp); get(temp); zero(arg_type);
                code.byte(operation("<", arg_type)); code.byte(0x1b);
                return;
            }
            if (operation_id == Builtin::Min || operation_id == Builtin::Max) {
                const int a = temporary(arg_type), b = temporary(arg_type);
                set(a); expression(*expr.args[1]); set(b);
                get(a); get(b); get(a); get(b);
                code.byte(operation(operation_id == Builtin::Min ? "<" : ">", arg_type));
                code.byte(0x1b);
                return;
            }
            switch (operation_id) {
            case Builtin::Sqrt: code.byte(0x9f); break;
            case Builtin::Floor: code.byte(0x9c); break;
            case Builtin::Ceil: code.byte(0x9b); break;
            case Builtin::Truncate: code.byte(0x9d); break;
            default: throw std::logic_error("unknown builtin");
            }
        }

        bool sum_loop(Expr& expr, int first, int last) {
            const auto& body = *expr.args[2];
            if (!emitter.options.optimize || body.kind != Kind::Assign || body.args[0]->kind != Kind::Name ||
                body.args[0]->global || type(*body.args[0]) != Type::Int) return false;
            const auto& rhs = *body.args[1];
            if (rhs.kind != Kind::Binary || rhs.name != "+") return false;
            const int target = body.args[0]->index;
            const auto is_local = [](const Expr& value, int index) {
                return value.kind == Kind::Name && !value.global && value.index == index;
            };
            if (!((is_local(*rhs.args[0], target) && is_local(*rhs.args[1], expr.index)) ||
                  (is_local(*rhs.args[1], target) && is_local(*rhs.args[0], expr.index)))) return false;
            get(first); get(last); code.byte(operation(expr.flag ? ">=" : "<=", Type::Int));
            begin(0x04);
            get(locals[static_cast<std::size_t>(target)]);
            get(first); code.byte(0xac); get(last); code.byte(0xac); code.byte(0x7c);
            get(expr.flag ? first : last); code.byte(0xac);
            get(expr.flag ? last : first); code.byte(0xac); code.byte(0x7d);
            one(Type::Int64); code.byte(0x7c);
            // For signed i32 endpoints, count * (first + last) fits signed i64 and is even.
            code.byte(0x7e); constant(code, {Type::Int64, 2}); code.byte(0x7f);
            code.byte(0xa7); code.byte(0x6a); set(locals[static_cast<std::size_t>(target)]);
            end();
            return true;
        }

        void match(Expr& expr, std::size_t arm_index, bool tail) {
            if (arm_index == expr.arms.size()) { code.byte(0x00); return; }
            auto& arm = expr.arms[arm_index];
            if (arm.patterns.size() == 1 && arm.patterns[0]->kind == Kind::Name && !arm.guard) {
                expression(*arm.body, tail);
                return;
            }
            for (std::size_t i = 0; i < arm.patterns.size(); ++i) {
                if (arm.patterns[i]->kind == Kind::Name) one(Type::Bool);
                else {
                    get(locals[static_cast<std::size_t>(expr.index)]);
                    emitter.value(code, *arm.patterns[i]->value);
                    if (type(*expr.args[0]) == Type::String) {
                        helper(Helper::Compare); zero(Type::Int); code.byte(0x46);
                    } else code.byte(operation("=", type(*expr.args[0])));
                }
                if (i) code.byte(0x72);
            }
            if (arm.guard) {
                begin(0x04, Type::Bool);
                expression(*arm.guard);
                code.byte(0x05); zero(Type::Bool);
                end();
            }
            begin(0x04, type(expr));
            expression(*arm.body, tail);
            code.byte(0x05);
            match(expr, arm_index + 1, tail);
            end();
        }

        void expression(Expr& expr, bool tail = false) {
            if (expr.kind == Kind::Literal || expr.kind == Kind::String || (emitter.options.optimize && expr.value)) {
                emitter.value(code, *expr.value);
                return;
            }
            switch (expr.kind) {
            case Kind::Literal: case Kind::String: break;
            case Kind::Array: {
                const int pointer = temporary(Type::Int);
                constant(code, {Type::Int, expr.args.size()}); allocate(type(expr)); set(pointer);
                for (std::size_t i = 0; i < expr.args.size(); ++i) {
                    get(pointer); expression(*expr.args[i]);
                    store(type(*expr.args[i]), static_cast<unsigned>(8 + i * element_size(type(expr))));
                }
                get(pointer);
                break;
            }
            case Kind::Index:
                index_access(expr);
                break;
            case Kind::Length:
                expression(*expr.args[0]);
                if (!validated_parameter(*expr.args[0])) checked_reference(type(*expr.args[0]));
                load(Type::Int);
                break;
            case Kind::Name:
                if (type(expr) == Type::Unit) break;
                if (expr.global) {
                    code.byte(0x23);
                    code.u32(static_cast<std::size_t>(emitter.global_indices[static_cast<std::size_t>(expr.index)]));
                    if (type(expr) == Type::Bool) normalize_bool();
                } else get(locals[static_cast<std::size_t>(expr.index)]);
                break;
            case Kind::Unary:
                if (expr.name == "-" && type(expr) != Type::Float) zero(type(expr));
                expression(*expr.args[0]);
                if (expr.name == "-")
                    code.byte(type(expr) == Type::Float ? 0x9a : operation("-", type(expr)));
                else if (expr.name == "~~~") {
                    constant(code, {type(expr), ~std::uint64_t{0}});
                    code.byte(operation("^^^", type(expr)));
                }
                break;
            case Kind::Binary:
                if (tail && accumulator >= 0 && emitter.addition_call(expr, index)) {
                    get(accumulator); expression(*expr.args[0]); code.byte(operation("+", type(expr))); set(accumulator);
                    expression(*expr.args[1], true);
                    break;
                }
                expression(*expr.args[0]);
                if (expr.name == "&&" || expr.name == "||") {
                    begin(0x04, Type::Bool);
                    if (expr.name == "&&") expression(*expr.args[1]); else one(Type::Bool);
                    code.byte(0x05);
                    if (expr.name == "||") expression(*expr.args[1]); else zero(Type::Bool);
                    end();
                } else {
                    expression(*expr.args[1]);
                    if (type(*expr.args[0]) == Type::String) {
                        helper(expr.name == "+" ? Helper::Concat : Helper::Compare);
                        if (expr.name != "+") { zero(Type::Int); code.byte(operation(expr.name, Type::Int)); }
                    } else {
                        if ((expr.name == "<<<" || expr.name == ">>>") && type(*expr.args[0]) == Type::Int64)
                            code.byte(0xad);
                        if (expr.name == "%") {
                            const Type operand = type(*expr.args[0]);
                            const std::uint64_t minimum = std::uint64_t{1} << (operand == Type::Int ? 31 : 63);
                            const bool safe = (expr.args[0]->value && expr.args[0]->value->bits != minimum) ||
                                              (expr.args[1]->value && signed_value(*expr.args[1]->value) != -1);
                            if (!emitter.options.optimize || !safe) {
                                const int a = temporary(operand), b = temporary(operand);
                                set(b); set(a);
                                get(a); constant(code, {operand, minimum}); code.byte(operation("=", operand));
                                get(b); constant(code, {operand, ~std::uint64_t{0}}); code.byte(operation("=", operand));
                                code.byte(0x71); begin(0x04); code.byte(0x00); end();
                                get(a); get(b);
                            }
                        }
                        code.byte(operation(expr.name, type(*expr.args[0])));
                    }
                }
                break;
            case Kind::Call:
                if (expr.index < 0) builtin(expr);
                else {
                    for (auto& arg : expr.args) expression(*arg);
                    if (tail && expr.index == index && tail_label >= 0) {
                        for (std::size_t i = function.parameters; i-- > 0;)
                            if (locals[i] >= 0) set(locals[i]);
                        if (reference_parameters) {
                            for (std::size_t i = 0; i < function.parameters; ++i) {
                                if (!is_reference(emitter.type(function.locals[i]))) continue;
                                get(locals[i]); checked_reference(emitter.type(function.locals[i])); code.byte(0x1a);
                            }
                        }
                        if (heap_mark >= 0 && !reference_parameters) restore_heap();
                        branch(static_cast<unsigned>(controls - tail_label));
                    } else {
                        code.byte(0x10);
                        code.u32(static_cast<std::size_t>(emitter.function_indices[static_cast<std::size_t>(expr.index)]));
                        if (type(expr) == Type::Bool &&
                            emitter.program.functions[static_cast<std::size_t>(expr.index)].imported) normalize_bool();
                    }
                }
                break;
            case Kind::If:
                if (emitter.options.optimize && expr.args[0]->value) {
                    expression(*expr.args[expr.args[0]->value->bits ? 1 : 2], tail);
                } else if (tail && tail_label >= 0 &&
                           ((expr.args[2]->kind == Kind::Call && expr.args[2]->index == index) ||
                            (accumulator >= 0 && emitter.addition_call(*expr.args[2], index)))) {
                    expression(*expr.args[0]);
                    begin(0x04);
                    expression(*expr.args[1], true);
                    if (accumulator >= 0) { get(accumulator); code.byte(operation("+", type(expr))); }
                    if (heap_mark >= 0) restore_heap();
                    code.byte(0x0f);
                    end();
                    expression(*expr.args[2], true);
                } else {
                    expression(*expr.args[0]);
                    begin(0x04, type(expr));
                    expression(*expr.args[1], tail);
                    code.byte(0x05);
                    expression(*expr.args[2], tail);
                    end();
                }
                break;
            case Kind::Block:
                for (std::size_t i = 0; i < expr.args.size(); ++i)
                    expression(*expr.args[i], tail && i + 1 == expr.args.size());
                break;
            case Kind::Let:
                expression(*expr.args[0]);
                if (type(*expr.args[0]) != Type::Unit) set(locals[static_cast<std::size_t>(expr.index)]);
                if (expr.args.size() == 2) expression(*expr.args[1], tail);
                break;
            case Kind::Assign: {
                auto& target = *expr.args[0];
                if (target.kind == Kind::Index) {
                    index_access(target, expr.args[1].get());
                    break;
                }
                expression(*expr.args[1]);
                if (type(target) != Type::Unit) {
                    code.byte(target.global ? 0x24 : 0x21);
                    code.u32(static_cast<std::size_t>(target.global ?
                        emitter.global_indices[static_cast<std::size_t>(target.index)] :
                        locals[static_cast<std::size_t>(target.index)]));
                }
                break;
            }
            case Kind::While:
                begin(0x02); begin(0x03);
                expression(*expr.args[0]); code.byte(0x45); branch(1, true);
                expression(*expr.args[1]); branch(0);
                end(); end();
                break;
            case Kind::For: {
                const int counter = locals[static_cast<std::size_t>(expr.index)];
                const int bound = temporary(Type::Int);
                expression(*expr.args[0]); set(counter);
                expression(*expr.args[1]); set(bound);
                if (sum_loop(expr, counter, bound)) break;
                begin(0x02); begin(0x03);
                get(counter); get(bound);
                code.byte(operation(expr.flag ? "<" : ">", Type::Int)); branch(1, true);
                expression(*expr.args[2]);
                // Test the terminal value before incrementing, including Int32.Min/MaxValue.
                get(counter); get(bound); code.byte(0x46); branch(1, true);
                get(counter); one(Type::Int); code.byte(expr.flag ? 0x6b : 0x6a); set(counter);
                branch(0);
                end(); end();
                break;
            }
            case Kind::Match:
                expression(*expr.args[0]);
                if (type(*expr.args[0]) != Type::Unit) set(locals[static_cast<std::size_t>(expr.index)]);
                match(expr, 0, tail);
                break;
            case Kind::Annotate:
                expression(*expr.args[0], tail);
                break;
            case Kind::Do:
                expression(*expr.args[0]);
                if (type(*expr.args[0]) != Type::Unit) code.byte(0x1a);
                break;
            case Kind::Tuple:
                throw std::logic_error("unexpected tuple in code generation");
            }
        }
    public:
        Body(Emitter& emitter, Function& function, int index) : emitter(emitter), function(function), index(index) {
            safe_region = emitter.options.optimize && emitter.leaf_region(*function.body, index);
            for (std::size_t i = 0; i < function.locals.size(); ++i) {
                const auto type = emitter.type(function.locals[i]);
                if (i < function.parameters && is_reference(type)) reference_parameters = true;
                if (type == Type::Unit) locals.push_back(-1);
                else {
                    locals.push_back(static_cast<int>(local_types.size()));
                    local_types.push_back(type);
                    if (i < function.parameters) ++parameter_count;
                }
            }
        }
        Bytes run() {
            const Type result = emitter.program.types.get(function.result, function.token.position);
            for (std::size_t i = 0; i < function.parameters; ++i) {
                if (emitter.type(function.locals[i]) == Type::Bool) {
                    get(locals[i]); normalize_bool(); set(locals[i]);
                } else if (is_reference(emitter.type(function.locals[i]))) {
                    get(locals[i]); checked_reference(emitter.type(function.locals[i])); code.byte(0x1a);
                }
            }
            if (safe_region && !is_reference(result) && emitter.allocates(*function.body) &&
                emitter.helpers[static_cast<std::size_t>(Helper::Allocate)]) {
                heap_mark = temporary(Type::Int);
                code.byte(0x23); code.u32(emitter.heap_global); set(heap_mark);
            }
            const bool additive = emitter.options.optimize && emitter.tail_addition(*function.body, index);
            if (additive) accumulator = temporary(result);
            const bool loop = emitter.options.optimize && (additive || emitter.tail_call(*function.body, index));
            if (loop) {
                begin(0x03, result);
                tail_label = controls;
            }
            expression(*function.body, true);
            if (loop) end();
            if (accumulator >= 0) { get(accumulator); code.byte(operation("+", result)); }
            if (heap_mark >= 0) restore_heap();
            code.byte(0x0b);
            Bytes body;
            std::vector<std::pair<std::size_t, Type>> groups;
            for (std::size_t i = parameter_count; i < local_types.size(); ++i) {
                if (!groups.empty() && groups.back().second == local_types[i]) ++groups.back().first;
                else groups.emplace_back(1, local_types[i]);
            }
            body.u32(groups.size());
            for (auto group : groups) { body.u32(group.first); body.byte(wasm_type(group.second)); }
            body.append(code.data);
            return body;
        }
    };
public:
    Emitter(Program& program, const Options& options) : program(program), options(options),
        function_indices(program.functions.size(), -1), global_indices(program.globals.size(), -1),
        live_functions(program.functions.size()), live_globals(program.globals.size()) {}

    Output run() {
        if (!options.max_memory_pages || options.max_memory_pages > 32767)
            throw Error({}, "maximum memory must be between 1 and 32767 pages");
        live_strings.resize(program.strings.size());
        string_offsets.resize(program.strings.size());
        for (const auto& name : options.exports)
            if (!exports.insert(name).second) throw Error({}, "duplicate export '" + name + "'");
        if (options.exports.empty()) {
            for (const auto& function : program.functions)
                if (!function.private_value && !function.imported) exports.insert(function.name);
            for (const auto& global : program.globals)
                if (!global.private_value && global.name != "_") exports.insert(global.name);
        }
        auto missing = exports;
        for (std::size_t i = 0; i < program.functions.size(); ++i) {
            if (exports.count(program.functions[i].name)) { mark_function(i); missing.erase(program.functions[i].name); }
        }
        for (std::size_t i = 0; i < program.globals.size(); ++i) {
            if (exports.count(program.globals[i].name)) { live_globals[i] = true; missing.erase(program.globals[i].name); }
        }
        if (!missing.empty()) throw Error({}, "unknown export '" + *missing.begin() + "'");
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& function = program.functions[pending[i]];
            if (!function.imported)
                for (std::size_t j = 0; j < function.parameters; ++j)
                    if (is_reference(type(function.locals[j]))) need(Helper::Check);
            if (function.body) mark(*function.body);
        }
        for (std::size_t i = 0; i < program.globals.size(); ++i)
            if (live_globals[i]) mark_value(*program.globals[i].body->value);
        Bytes data;
        for (std::size_t i = 0; i < live_strings.size(); ++i) {
            if (!live_strings[i]) continue;
            string_offsets[i] = heap_start;
            const auto& text = program.strings[i];
            const auto length = static_cast<std::uint32_t>(text.size() / 2);
            for (unsigned shift = 0; shift < 32; shift += 8) data.byte(static_cast<std::uint8_t>(length >> shift));
            data.byte(static_cast<unsigned>(Type::String)); data.byte(0); data.byte(0); data.byte(0);
            data.data.insert(data.data.end(), text.begin(), text.end());
            while (data.data.size() % 8) data.byte(0);
            heap_start = 8 + data.data.size();
        }
        if (memory && exports.count("memory")) throw Error({}, "'memory' is reserved for linear memory exports");
        if (heap_start > static_cast<std::size_t>(options.max_memory_pages) * 65536)
            throw Error({}, "string data exceeds the configured memory limit");
        std::vector<std::size_t> ordered;
        std::size_t imports = 0;
        for (bool imported : {true, false}) {
            for (std::size_t i = 0; i < program.functions.size(); ++i) {
                if (live_functions[i] && program.functions[i].imported == imported) {
                    function_indices[i] = static_cast<int>(ordered.size());
                    ordered.push_back(i);
                    if (imported) ++imports;
                }
            }
        }
        std::size_t helper_count = 0;
        for (std::size_t i = 0; i < helpers.size(); ++i)
            if (helpers[i]) helper_indices[i] = static_cast<int>(ordered.size() + helper_count++);
        std::map<std::vector<std::uint8_t>, std::size_t> interned;
        std::vector<Bytes> signatures;
        std::vector<std::size_t> signature_indices;
        for (auto index : ordered) {
            const auto& function = program.functions[index];
            Bytes signature;
            signature.byte(0x60);
            std::size_t count = 0;
            for (std::size_t i = 0; i < function.parameters; ++i)
                if (type(function.locals[i]) != Type::Unit) ++count;
            signature.u32(count);
            for (std::size_t i = 0; i < function.parameters; ++i)
                if (type(function.locals[i]) != Type::Unit) signature.byte(wasm_type(type(function.locals[i])));
            const auto result = program.types.get(function.result, function.token.position);
            signature.u32(result == Type::Unit ? 0 : 1);
            if (result != Type::Unit) signature.byte(wasm_type(result));
            const auto found = interned.emplace(signature.data, signatures.size());
            signature_indices.push_back(found.first->second);
            if (found.second) signatures.push_back(std::move(signature));
        }
        std::array<std::size_t, 4> helper_signatures{};
        for (std::size_t i = 0; i < helpers.size(); ++i) {
            if (!helpers[i]) continue;
            const unsigned params = i <= static_cast<std::size_t>(Helper::Check) ? 3 : 2;
            Bytes signature;
            signature.byte(0x60); signature.u32(params);
            for (unsigned p = 0; p < params; ++p) signature.byte(0x7f);
            signature.u32(1); signature.byte(0x7f);
            const auto found = interned.emplace(signature.data, signatures.size());
            helper_signatures[i] = found.first->second;
            if (found.second) signatures.push_back(std::move(signature));
        }
        Bytes module;
        module.data = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
        if (!signatures.empty()) {
            Bytes section;
            section.u32(signatures.size());
            for (const auto& signature : signatures) section.append(signature.data);
            module.section(1, section);
        }
        if (imports) {
            Bytes section;
            section.u32(imports);
            for (std::size_t i = 0; i < imports; ++i) {
                const auto& function = program.functions[ordered[i]];
                section.text(function.import_module); section.text(function.import_name);
                section.byte(0); section.u32(signature_indices[i]);
            }
            module.section(2, section);
        }
        if (ordered.size() > imports || helper_count) {
            Bytes section;
            section.u32(ordered.size() - imports + helper_count);
            for (std::size_t i = imports; i < ordered.size(); ++i) section.u32(signature_indices[i]);
            for (std::size_t i = 0; i < helpers.size(); ++i) if (helpers[i]) section.u32(helper_signatures[i]);
            module.section(3, section);
        }
        if (memory) {
            Bytes section;
            section.u32(1); section.byte(1);
            section.u32((heap_start + 65535) / 65536); section.u32(options.max_memory_pages);
            module.section(5, section);
        }
        std::size_t global_count = 0;
        for (std::size_t i = 0; i < program.globals.size(); ++i)
            if (live_globals[i]) global_indices[i] = static_cast<int>(global_count++);
        heap_global = global_count;
        if (global_count || helper_count) {
            Bytes section;
            section.u32(global_count + (helper_count ? 1 : 0));
            for (std::size_t i = 0; i < program.globals.size(); ++i) {
                if (!live_globals[i]) continue;
                const auto& global = program.globals[i];
                auto value = *global.body->value;
                if (value.type == Type::Unit) value = {Type::Int, 0};
                section.byte(wasm_type(value.type)); section.byte(global.mutable_value ? 1 : 0);
                this->value(section, value); section.byte(0x0b);
            }
            if (helper_count) {
                section.byte(0x7f); section.byte(1);
                constant(section, {Type::Int, heap_start}); section.byte(0x0b);
            }
            module.section(6, section);
        }
        if (!exports.empty() || memory) {
            Bytes section;
            section.u32(exports.size() + (memory ? 1 : 0));
            for (std::size_t i = 0; i < program.functions.size(); ++i) {
                if (!exports.count(program.functions[i].name)) continue;
                section.text(program.functions[i].name);
                section.byte(0); section.u32(static_cast<std::size_t>(function_indices[i]));
            }
            for (std::size_t i = 0; i < program.globals.size(); ++i) {
                if (!exports.count(program.globals[i].name)) continue;
                section.text(program.globals[i].name);
                section.byte(3); section.u32(static_cast<std::size_t>(global_indices[i]));
            }
            if (memory) { section.text("memory"); section.byte(2); section.u32(0); }
            module.section(7, section);
        }
        if (ordered.size() > imports || helper_count) {
            Bytes section;
            section.u32(ordered.size() - imports + helper_count);
            for (std::size_t i = imports; i < ordered.size(); ++i) {
                auto body = Body(*this, program.functions[ordered[i]], static_cast<int>(ordered[i])).run();
                section.u32(body.data.size()); section.append(body.data);
            }
            for (std::size_t i = 0; i < helpers.size(); ++i) {
                if (!helpers[i]) continue;
                auto body = runtime(static_cast<Helper>(i), helper_indices, global_count, options.max_memory_pages);
                section.u32(body.data.size()); section.append(body.data);
            }
            module.section(10, section);
        }
        if (!data.data.empty()) {
            Bytes section;
            section.u32(1); section.byte(0); constant(section, {Type::Int, 8}); section.byte(0x0b);
            section.u32(data.data.size()); section.append(data.data);
            module.section(11, section);
        }
        return {std::move(module.data), ordered.size() - imports + helper_count};
    }
};

} // namespace

Output emit(Program& program, const Options& options) { return Emitter(program, options).run(); }

} // namespace fsw

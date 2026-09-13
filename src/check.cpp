#include "fsw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace fsw {

std::int64_t signed_value(Value value) {
    if (value.type == Type::Int64) {
        std::int64_t result = 0;
        std::memcpy(&result, &value.bits, sizeof(result));
        return result;
    }
    const auto bits = static_cast<std::uint32_t>(value.bits);
    std::int32_t result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

double float_value(Value value) {
    double result = 0;
    std::memcpy(&result, &value.bits, sizeof(result));
    return result;
}

namespace {

Value floating(double number) {
    Value result{Type::Float, 0};
    std::memcpy(&result.bits, &number, sizeof(number));
    return result;
}

Value integer(Type type, std::uint64_t bits) {
    return {type, type == Type::Int ? bits & 0xffffffffu : bits};
}

std::optional<Value> unary(std::string_view op, Value value) {
    if (op == "+") return value;
    if (op == "-") {
        if (value.type == Type::Float) return floating(-float_value(value));
        return integer(value.type, std::uint64_t{0} - value.bits);
    }
    if (op == "~~~") return integer(value.type, ~value.bits);
    return {};
}

std::optional<Value> binary(std::string_view op, Value a, Value b) {
    if (op == "&&") return Value{Type::Bool, a.bits && b.bits ? 1u : 0u};
    if (op == "||") return Value{Type::Bool, a.bits || b.bits ? 1u : 0u};
    if (op == "=" || op == "<>" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        bool result = false;
        if (a.type == Type::Float) {
            const auto x = float_value(a), y = float_value(b);
            if (op == "=") result = x == y;
            if (op == "<>") result = x != y;
            if (op == "<") result = x < y;
            if (op == ">") result = x > y;
            if (op == "<=") result = x <= y;
            if (op == ">=") result = x >= y;
        } else {
            const auto x = signed_value(a), y = signed_value(b);
            if (op == "=") result = x == y;
            if (op == "<>") result = x != y;
            if (op == "<") result = x < y;
            if (op == ">") result = x > y;
            if (op == "<=") result = x <= y;
            if (op == ">=") result = x >= y;
        }
        return Value{Type::Bool, result ? 1u : 0u};
    }
    if (a.type == Type::Float) {
        const auto x = float_value(a), y = float_value(b);
        if (op == "+") return floating(x + y);
        if (op == "-") return floating(x - y);
        if (op == "*") return floating(x * y);
        if (op == "/") return floating(x / y);
        return {};
    }
    if (op == "+") return integer(a.type, a.bits + b.bits);
    if (op == "-") return integer(a.type, a.bits - b.bits);
    if (op == "*") return integer(a.type, a.bits * b.bits);
    if (op == "&&&") return integer(a.type, a.bits & b.bits);
    if (op == "|||") return integer(a.type, a.bits | b.bits);
    if (op == "^^^") return integer(a.type, a.bits ^ b.bits);
    if (op == "/" || op == "%") {
        const auto x = signed_value(a), y = signed_value(b);
        const auto minimum = a.type == Type::Int ? std::numeric_limits<std::int32_t>::min() :
                                                  std::numeric_limits<std::int64_t>::min();
        if (!y || (x == minimum && y == -1)) return {};
        return integer(a.type, static_cast<std::uint64_t>(op == "/" ? x / y : x % y));
    }
    if (op == "<<<" || op == ">>>") {
        const unsigned width = a.type == Type::Int ? 32u : 64u;
        const auto shift = static_cast<unsigned>(b.bits & (width - 1u));
        std::uint64_t bits = op == "<<<" ? a.bits << shift : a.bits >> shift;
        if (op == ">>>" && shift && (a.bits & (std::uint64_t{1} << (width - 1u))))
            bits |= ~std::uint64_t{0} << (width - shift);
        return integer(a.type, bits);
    }
    return {};
}

struct Symbol {
    int index;
    int type;
    bool global;
    bool mutable_value;
    std::optional<Value> value;
};

class Checker {
    Program& program;
    Types& types;
    Function* function = nullptr;
    int current_group = 0;
    std::unordered_map<std::string, int> functions;
    std::unordered_map<std::string, int> globals;
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
    std::unordered_map<std::string, std::size_t> strings;
    std::size_t string_bytes = 0;
    unsigned array_id = 0;

    Value string(const std::string& text, Position at) {
        const auto found = strings.find(text);
        if (found != strings.end()) return {Type::String, found->second};
        if (text.size() > max_string_bytes - string_bytes)
            throw Error(at, "constant string storage exceeds the 64 MiB compiler limit");
        const auto index = program.strings.size();
        strings.emplace(text, index);
        string_bytes += text.size();
        program.strings.push_back(text);
        return {Type::String, index};
    }
    void length(Expr& expr) {
        result(*expr.args[0], references);
        result(expr, mask(Type::Int));
        if (expr.args[0]->value && expr.args[0]->value->type == Type::String)
            expr.value = Value{Type::Int, program.strings[expr.args[0]->value->bits].size() / 2};
    }

    std::string unqualified(const Expr& expr) const {
        const auto& name = expr.name;
        if (expr.flag && !program.module.empty() && name.substr(0, program.module.size() + 1) == program.module + ".")
            return name.substr(program.module.size() + 1);
        return name;
    }
    int local(const std::string& name, Token token, int type, bool mutable_value = false) {
        if (!function) throw Error(token.position, "top-level values require constant initializers");
        const int index = static_cast<int>(function->locals.size());
        Local variable;
        variable.name = name;
        variable.token = token;
        variable.type = type;
        variable.mutable_value = mutable_value;
        function->locals.push_back(std::move(variable));
        return index;
    }
    void bind(const std::string& name, Symbol symbol) {
        if (name != "_" && !name.empty()) scopes.back()[name] = std::move(symbol);
    }
    std::optional<Symbol> find_local(const std::string& name) {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) return found->second;
        }
        return {};
    }
    Symbol resolve(const Expr& expr) {
        if (!expr.flag) {
            if (auto found = find_local(expr.name)) return *found;
        }
        const auto found = globals.find(unqualified(expr));
        if (found != globals.end()) {
            auto& global = program.globals[static_cast<std::size_t>(found->second)];
            if (global.group >= current_group)
                throw Error(expr.token.position, "value '" + expr.name + "' is used before its definition");
            return {found->second, global.type, true, global.mutable_value,
                    global.mutable_value ? std::optional<Value>{} : global.body->value};
        }
        if (functions.count(unqualified(expr)))
            throw Error(expr.token.position, "function '" + expr.name + "' must be fully applied");
        throw Error(expr.token.position, "undefined value '" + expr.name + "'");
    }
    void result(Expr& expr, unsigned choices) { types.constrain(expr.type, choices, expr.token.position); }
    void same(Expr& a, Expr& b) { types.unify(a.type, b.type, b.token.position); }
    void visit_args(Expr& expr) { for (auto& arg : expr.args) visit(*arg); }

    void array_combinator(Expr& expr, const std::string& operation) {
        const bool folding = operation == "Array.fold";
        const bool mapping = operation == "Array.map";
        const std::size_t arity = folding ? 3u : 2u;
        if (expr.args.size() != arity)
            throw Error(expr.token.position, "'" + operation + "' expects " + std::to_string(arity) + " argument(s)");
        const Token at = expr.token;
        const std::string prefix = std::string(1, '\0') + "array" + std::to_string(array_id++);
        auto reference = [&](const std::string& name) {
            auto result = node(Kind::Name, at);
            result->name = name;
            return result;
        };
        auto number = [&](std::uint64_t value) {
            auto result = node(Kind::Literal, at);
            result->value = Value{Type::Int, value};
            return result;
        };
        auto unary_node = [&](Kind kind, ExprPtr arg) {
            auto result = node(kind, at);
            result->args.push_back(std::move(arg));
            return finish(std::move(result));
        };
        auto binary_node = [&](Kind kind, ExprPtr left, ExprPtr right, std::string name = "") {
            auto result = node(kind, at);
            result->name = std::move(name);
            result->args.push_back(std::move(left));
            result->args.push_back(std::move(right));
            return finish(std::move(result));
        };
        auto binding = [&](const std::string& name, ExprPtr value, bool mutable_value = false) {
            auto result = unary_node(Kind::Let, std::move(value));
            result->name = name;
            result->flag = mutable_value;
            return result;
        };
        auto block = node(Kind::Block, at);
        auto callback = std::move(expr.args[0]);
        const std::size_t callback_arity = folding ? 2u : 1u;
        std::vector<std::string> cached;
        if (callback->kind == Kind::Lambda) {
            if (callback->args.size() != callback_arity + 1)
                throw Error(callback->token.position, "callback has the wrong number of parameters");
        } else if (callback->kind == Kind::Call || callback->kind == Kind::Name) {
            for (auto& arg : callback->args) {
                const auto name = prefix + ":arg" + std::to_string(cached.size());
                block->args.push_back(binding(name, std::move(arg)));
                cached.push_back(name);
            }
            callback->args.clear();
        } else throw Error(callback->token.position, "use a lambda or a statically known function as the array callback");
        const auto input = prefix + ":input", length = prefix + ":length";
        const auto output = prefix + ":output", counter = prefix + ":index", state = prefix + ":state";
        if (folding) block->args.push_back(binding(state, std::move(expr.args[1]), true));
        block->args.push_back(binding(input, std::move(expr.args.back())));
        block->args.push_back(binding(length, unary_node(Kind::Length, reference(input))));
        if (mapping) {
            auto allocate = unary_node(Kind::Call, reference(length));
            allocate->name = std::string(1, '\0') + "Array.zeroCreate";
            block->args.push_back(binding(output, std::move(allocate)));
        }
        std::vector<ExprPtr> arguments;
        if (folding) arguments.push_back(reference(state));
        arguments.push_back(binary_node(Kind::Index, reference(input), reference(counter)));
        ExprPtr applied;
        if (callback->kind == Kind::Lambda) {
            applied = node(Kind::Block, callback->token);
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                auto parameter = binding(callback->args[i]->name, std::move(arguments[i]));
                parameter->token = callback->args[i]->token;
                parameter->annotation = callback->args[i]->annotation;
                applied->args.push_back(std::move(parameter));
            }
            applied->args.push_back(std::move(callback->args.back()));
            applied = finish(std::move(applied));
        } else {
            callback->kind = Kind::Call;
            for (const auto& name : cached) callback->args.push_back(reference(name));
            for (auto& argument : arguments) callback->args.push_back(std::move(argument));
            applied = finish(std::move(callback));
        }
        auto loop = node(Kind::For, at);
        loop->name = counter;
        loop->args.push_back(number(0));
        loop->args.push_back(binary_node(Kind::Binary, reference(length), number(1), "-"));
        if (mapping)
            loop->args.push_back(binary_node(Kind::Assign,
                binary_node(Kind::Index, reference(output), reference(counter)), std::move(applied)));
        else if (folding)
            loop->args.push_back(binary_node(Kind::Assign, reference(state), std::move(applied)));
        else loop->args.push_back(std::move(applied));
        block->args.push_back(finish(std::move(loop)));
        if (mapping || folding) block->args.push_back(reference(mapping ? output : state));
        else {
            auto unit = node(Kind::Literal, at);
            unit->value = Value{};
            block->args.push_back(std::move(unit));
        }
        expr = std::move(*finish(std::move(block)));
        visit(expr);
    }

    void call(Expr& expr) {
        static const std::unordered_map<std::string, Builtin> builtins{
            {"not", Builtin::Not}, {"int", Builtin::Int}, {"int32", Builtin::Int},
            {"int64", Builtin::Int64}, {"float", Builtin::Float}, {"float64", Builtin::Float},
            {"double", Builtin::Float}, {"abs", Builtin::Abs}, {"min", Builtin::Min},
            {"max", Builtin::Max}, {"sqrt", Builtin::Sqrt}, {"floor", Builtin::Floor},
            {"ceil", Builtin::Ceil}, {"truncate", Builtin::Truncate}, {"ignore", Builtin::Ignore},
            {"Array.zeroCreate", Builtin::ArrayZero}, {"Array.create", Builtin::ArrayCreate},
            {"Array.length", Builtin::ArrayLength}, {"String.length", Builtin::StringLength},
            {std::string(1, '\0') + "Array.zeroCreate", Builtin::ArrayZero}
        };
        if (expr.token.kind == TokenKind::Symbol) {
            if (expr.args.size() != 2)
                throw Error(expr.token.position, "binary operator functions require two arguments");
            expr.kind = Kind::Binary;
            visit(expr);
            return;
        }
        if (!expr.flag && find_local(expr.name))
            throw Error(expr.token.position, "local value '" + expr.name + "' is not a callable function");
        const std::string name = unqualified(expr);
        const auto found = functions.find(name);
        if (found != functions.end()) {
            auto& target = program.functions[static_cast<std::size_t>(found->second)];
            if (target.group > current_group || (target.group == current_group && !target.recursive))
                throw Error(expr.token.position, "function '" + name + "' requires an earlier definition or 'let rec'");
            if (target.tupled && expr.args.size() == 1 && expr.args.front()->kind == Kind::Tuple) {
                auto tuple = std::move(expr.args.front());
                expr.args = std::move(tuple->args);
            } else if (target.tupled && expr.args.size() > 1) {
                throw Error(expr.token.position, "extern functions take tupled arguments: f(a, b)");
            }
            if (expr.args.size() != target.parameters)
                throw Error(expr.token.position, "function '" + name + "' expects " +
                            std::to_string(target.parameters) + " argument(s)");
            visit_args(expr);
            for (std::size_t i = 0; i < expr.args.size(); ++i)
                types.unify(expr.args[i]->type, target.locals[i].type, expr.args[i]->token.position);
            types.unify(expr.type, target.result, expr.token.position);
            expr.index = found->second;
            return;
        }
        if ((name == "Array.map" || name == "Array.fold" || name == "Array.iter") && !globals.count(name)) {
            array_combinator(expr, name);
            return;
        }
        const auto builtin = builtins.find(name);
        if (builtin == builtins.end() || globals.count(name))
            throw Error(expr.token.position, "undefined function '" + expr.name + "'");
        const auto operation = builtin->second;
        const std::size_t arity = operation == Builtin::Min || operation == Builtin::Max ||
                                  operation == Builtin::ArrayCreate ? 2u : 1u;
        if (expr.args.size() != arity)
            throw Error(expr.token.position, "'" + name + "' expects " + std::to_string(arity) + " argument(s)");
        visit_args(expr);
        expr.index = -1 - static_cast<int>(operation);
        auto& arg = *expr.args[0];
        switch (operation) {
        case Builtin::Not:
            result(arg, mask(Type::Bool)); result(expr, mask(Type::Bool));
            if (arg.value) expr.value = Value{Type::Bool, arg.value->bits ? 0u : 1u};
            break;
        case Builtin::Ignore:
            result(expr, mask(Type::Unit));
            if (arg.value) expr.value = Value{};
            break;
        case Builtin::Int: case Builtin::Int64: case Builtin::Float:
            result(arg, numbers);
            result(expr, mask(operation == Builtin::Int ? Type::Int :
                              operation == Builtin::Int64 ? Type::Int64 : Type::Float));
            break;
        case Builtin::Abs:
            result(arg, numbers); same(expr, arg);
            break;
        case Builtin::Min: case Builtin::Max:
            result(arg, integers); same(arg, *expr.args[1]); same(expr, arg);
            if (arg.value && expr.args[1]->value) {
                const bool lower = signed_value(*arg.value) < signed_value(*expr.args[1]->value);
                expr.value = (operation == Builtin::Min ? lower : !lower) ? arg.value : expr.args[1]->value;
            }
            break;
        case Builtin::Sqrt: case Builtin::Floor: case Builtin::Ceil: case Builtin::Truncate:
            result(arg, mask(Type::Float)); same(expr, arg);
            break;
        case Builtin::ArrayZero:
            result(arg, mask(Type::Int));
            types.array_of(expr.type, types.add(expr.element_annotation), expr.token.position);
            break;
        case Builtin::ArrayCreate:
            result(arg, mask(Type::Int));
            types.array_of(expr.type, expr.args[1]->type, expr.token.position);
            break;
        case Builtin::ArrayLength: case Builtin::StringLength:
            result(arg, operation == Builtin::ArrayLength ? arrays : mask(Type::String));
            length(expr);
            break;
        }
    }

    void visit(Expr& expr) {
        expr.type = types.add(expr.kind == Kind::Let ? all_types : expr.annotation);
        switch (expr.kind) {
        case Kind::Literal:
            result(expr, mask(expr.value->type));
            break;
        case Kind::String:
            result(expr, mask(Type::String));
            expr.value = string(expr.name, expr.token.position);
            break;
        case Kind::Array: {
            const int element = types.add(elements);
            for (auto& arg : expr.args) {
                visit(*arg);
                types.unify(arg->type, element, arg->token.position);
            }
            types.array_of(expr.type, element, expr.token.position);
            break;
        }
        case Kind::Index:
            visit_args(expr);
            result(*expr.args[1], mask(Type::Int));
            types.array_of(expr.args[0]->type, expr.type, expr.token.position);
            break;
        case Kind::Length:
            visit_args(expr);
            length(expr);
            break;
        case Kind::Name: {
            const auto symbol = resolve(expr);
            expr.index = symbol.index;
            expr.global = symbol.global;
            expr.value = symbol.value;
            types.unify(expr.type, symbol.type, expr.token.position);
            break;
        }
        case Kind::Unary:
            visit_args(expr);
            result(*expr.args[0], expr.name == "~~~" ? integers : numbers);
            same(expr, *expr.args[0]);
            if (expr.args[0]->value) expr.value = unary(expr.name, *expr.args[0]->value);
            break;
        case Kind::Binary: {
            visit_args(expr);
            auto& a = *expr.args[0];
            auto& b = *expr.args[1];
            if (expr.name == "&&" || expr.name == "||") {
                result(a, mask(Type::Bool)); same(a, b); same(expr, a);
            } else if (expr.name == "=" || expr.name == "<>" || expr.name == "<" || expr.name == ">" ||
                       expr.name == "<=" || expr.name == ">=") {
                result(a, elements | mask(Type::String)); same(a, b); result(expr, mask(Type::Bool));
            } else if (expr.name == "<<<" || expr.name == ">>>") {
                result(a, integers); result(b, mask(Type::Int)); same(expr, a);
            } else {
                result(a, expr.name == "&&&" || expr.name == "|||" || expr.name == "^^^" || expr.name == "%" ?
                          integers : expr.name == "+" ? numbers | mask(Type::String) : numbers);
                same(a, b); same(expr, a);
            }
            if (a.value && b.value) {
                if (a.value->type == Type::String) {
                    const auto& x = program.strings[a.value->bits];
                    const auto& y = program.strings[b.value->bits];
                    if (expr.name == "+") expr.value = string(x + y, expr.token.position);
                    else {
                        std::size_t i = 0;
                        int comparison = 0;
                        for (; i < std::min(x.size(), y.size()); i += 2) {
                            const auto a_unit = static_cast<unsigned char>(x[i]) | (static_cast<unsigned char>(x[i + 1]) << 8);
                            const auto b_unit = static_cast<unsigned char>(y[i]) | (static_cast<unsigned char>(y[i + 1]) << 8);
                            if (a_unit != b_unit) { comparison = a_unit < b_unit ? -1 : 1; break; }
                        }
                        if (!comparison) comparison = x.size() < y.size() ? -1 : x.size() > y.size() ? 1 : 0;
                        expr.value = binary(expr.name, integer(Type::Int, static_cast<std::uint64_t>(comparison)), {Type::Int, 0});
                    }
                } else expr.value = binary(expr.name, *a.value, *b.value);
            }
            if (a.value && ((expr.name == "&&" && !a.value->bits) || (expr.name == "||" && a.value->bits)))
                expr.value = a.value;
            break;
        }
        case Kind::Call:
            call(expr);
            break;
        case Kind::If:
            visit_args(expr);
            result(*expr.args[0], mask(Type::Bool));
            same(*expr.args[1], *expr.args[2]); same(expr, *expr.args[1]);
            if (expr.args[0]->value) expr.value = expr.args[expr.args[0]->value->bits ? 1 : 2]->value;
            break;
        case Kind::Block: {
            scopes.emplace_back();
            bool constant = true;
            for (std::size_t i = 0; i < expr.args.size(); ++i) {
                visit(*expr.args[i]);
                constant = constant && expr.args[i]->value.has_value();
                if (i + 1 < expr.args.size()) result(*expr.args[i], mask(Type::Unit));
            }
            if (expr.args.back()->kind == Kind::Let && expr.args.back()->args.size() == 1)
                throw Error(expr.args.back()->token.position, "a local 'let' must be followed by an expression");
            same(expr, *expr.args.back());
            if (constant) expr.value = expr.args.back()->value;
            scopes.pop_back();
            break;
        }
        case Kind::Let: {
            visit(*expr.args[0]);
            types.constrain(expr.args[0]->type, expr.annotation, expr.token.position);
            expr.index = local(expr.name, expr.token, expr.args[0]->type, expr.flag);
            if (expr.args.size() == 2) scopes.emplace_back();
            bind(expr.name, {expr.index, expr.args[0]->type, false, expr.flag,
                            expr.flag ? std::optional<Value>{} : expr.args[0]->value});
            if (expr.args.size() == 2) {
                visit(*expr.args[1]);
                same(expr, *expr.args[1]);
                if (expr.args[0]->value) expr.value = expr.args[1]->value;
                scopes.pop_back();
            } else {
                // The annotation belongs to the bound value, not the unit-valued declaration.
                expr.type = types.add(mask(Type::Unit));
                if (expr.args[0]->value && !expr.flag) expr.value = Value{};
            }
            break;
        }
        case Kind::Assign: {
            auto& target = *expr.args[0];
            if (target.kind != Kind::Name && target.kind != Kind::Index)
                throw Error(target.token.position, "assignment requires a mutable variable or array element");
            if (target.kind == Kind::Name) {
                const auto symbol = resolve(target);
                if (!symbol.mutable_value) throw Error(target.token.position, "'" + target.name + "' is immutable");
            }
            visit_args(expr);
            same(target, *expr.args[1]);
            result(expr, mask(Type::Unit));
            break;
        }
        case Kind::While:
            visit_args(expr);
            result(*expr.args[0], mask(Type::Bool)); result(*expr.args[1], mask(Type::Unit));
            result(expr, mask(Type::Unit));
            break;
        case Kind::For: {
            visit(*expr.args[0]); visit(*expr.args[1]);
            result(*expr.args[0], mask(Type::Int)); result(*expr.args[1], mask(Type::Int));
            scopes.emplace_back();
            const int counter_type = types.add(mask(Type::Int));
            expr.index = local(expr.name, expr.token, counter_type);
            bind(expr.name, {expr.index, counter_type, false, false, {}});
            visit(*expr.args[2]);
            result(*expr.args[2], mask(Type::Unit)); result(expr, mask(Type::Unit));
            scopes.pop_back();
            break;
        }
        case Kind::Match: {
            visit(*expr.args[0]);
            expr.index = local("$match", expr.token, expr.args[0]->type);
            bool exhaustive = false, yes = false, no = false;
            for (auto& arm : expr.arms) {
                if (exhaustive) throw Error(arm.body->token.position, "unreachable match arm");
                scopes.emplace_back();
                for (auto& pattern : arm.patterns) {
                    if (pattern->kind == Kind::Name) {
                        if (arm.patterns.size() != 1)
                            throw Error(pattern->token.position, "binding patterns cannot be combined with '|'");
                        bind(pattern->name, {expr.index, expr.args[0]->type, false, false, expr.args[0]->value});
                        if (!arm.guard) exhaustive = true;
                    } else {
                        visit(*pattern);
                        same(*pattern, *expr.args[0]);
                        if (!arm.guard && pattern->value->type == Type::Bool) {
                            if (pattern->value->bits) yes = true; else no = true;
                        }
                    }
                }
                if (arm.guard) { visit(*arm.guard); result(*arm.guard, mask(Type::Bool)); }
                visit(*arm.body);
                same(expr, *arm.body);
                scopes.pop_back();
                exhaustive = exhaustive || (yes && no);
            }
            if (!exhaustive) throw Error(expr.token.position, "non-exhaustive match; add an unguarded '_' arm");
            break;
        }
        case Kind::Tuple:
            throw Error(expr.token.position, "tuples are currently supported only as extern call arguments");
        case Kind::Annotate:
            visit_args(expr); same(expr, *expr.args[0]); expr.value = expr.args[0]->value;
            break;
        case Kind::Do:
            visit_args(expr); result(expr, mask(Type::Unit));
            if (expr.args[0]->value) expr.value = Value{};
            break;
        case Kind::Lambda:
            throw Error(expr.token.position, "lambda values are supported in top-level function bindings and array callbacks");
        }
    }

    void finalize(Expr& expr) {
        types.get(expr.type, expr.token.position);
        for (auto& arg : expr.args) finalize(*arg);
        for (auto& arm : expr.arms) {
            for (auto& pattern : arm.patterns)
                if (pattern->type >= 0) types.get(pattern->type, pattern->token.position);
            if (arm.guard) finalize(*arm.guard);
            finalize(*arm.body);
        }
    }
public:
    explicit Checker(Program& program) : program(program), types(program.types) {}

    void run() {
        std::unordered_map<std::string, Position> names;
        for (std::size_t i = 0; i < program.globals.size(); ++i) {
            auto& global = program.globals[i];
            if (!names.emplace(global.name, global.token.position).second)
                throw Error(global.token.position, "duplicate declaration '" + global.name + "'");
            globals.emplace(global.name, static_cast<int>(i));
            global.type = types.add(global.annotation);
        }
        for (std::size_t i = 0; i < program.functions.size(); ++i) {
            auto& target = program.functions[i];
            if (!names.emplace(target.name, target.token.position).second)
                throw Error(target.token.position, "duplicate declaration '" + target.name + "'");
            functions.emplace(target.name, static_cast<int>(i));
            target.result = types.add(target.annotation);
            for (auto& parameter : target.locals) parameter.type = types.add(parameter.annotation);
        }
        std::size_t fi = 0, gi = 0;
        while (fi < program.functions.size() || gi < program.globals.size()) {
            if (gi < program.globals.size() &&
                (fi == program.functions.size() || program.globals[gi].group < program.functions[fi].group)) {
                auto& global = program.globals[gi++];
                function = nullptr;
                current_group = global.group;
                scopes.emplace_back();
                visit(*global.body);
                types.unify(global.type, global.body->type, global.token.position);
                scopes.pop_back();
                if (!global.body->value)
                    throw Error(global.token.position, "top-level values require constant initializers");
            } else {
                function = &program.functions[fi++];
                current_group = function->group;
                scopes.emplace_back();
                for (std::size_t i = 0; i < function->parameters; ++i) {
                    const auto& parameter = function->locals[i];
                    if (parameter.name != "_" && !parameter.name.empty() && scopes.back().count(parameter.name))
                        throw Error(parameter.token.position, "duplicate parameter '" + parameter.name + "'");
                    bind(parameter.name, {static_cast<int>(i), parameter.type, false, false, {}});
                }
                if (function->body) {
                    visit(*function->body);
                    types.unify(function->result, function->body->type, function->token.position);
                }
                scopes.pop_back();
            }
        }
        for (auto& target : program.functions) {
            types.get(target.result, target.token.position);
            for (auto& variable : target.locals) types.get(variable.type, variable.token.position);
            if (target.body) finalize(*target.body);
        }
        for (auto& global : program.globals) finalize(*global.body);
    }
};

} // namespace

void check(Program& program) { Checker(program).run(); }

} // namespace fsw

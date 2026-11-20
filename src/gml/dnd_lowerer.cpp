#include "gml/dnd_lowerer.h"

#include <cctype>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace gm {

namespace {

enum ActionKind : i32 {
    kActNormal = 0,
    kActBegin = 1,
    kActEnd = 2,
    kActElse = 3,
    kActExit = 4,
    kActRepeat = 5,
    kActVariable = 6,
    kActCode = 7,
};

enum ExecuteType : i32 {
    kExeNothing = 0,
    kExeFunction = 1,
    kExeCode = 2,
};

enum ArgType : i32 {
    kArgConstant = -1,
    kArgExpression = 0,
    kArgString = 1,
    kArgStringExp = 2,
};

std::string_view trim_sv(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Safely encodes an arbitrary raw string as a GML string expression (no escape sequences in GML).
std::string quote_gml_literal(std::string_view raw) {
    if (raw.find('"') == std::string_view::npos) {
        std::string out;
        out.reserve(raw.size() + 2);
        out.push_back('"');
        out.append(raw);
        out.push_back('"');
        return out;
    }
    if (raw.find('\'') == std::string_view::npos) {
        std::string out;
        out.reserve(raw.size() + 2);
        out.push_back('\'');
        out.append(raw);
        out.push_back('\'');
        return out;
    }
    // Contains both " and ': split on " and concatenate with '"'.
    std::string out;
    usize pos = 0;
    bool first = true;
    while (pos <= raw.size()) {
        usize q = raw.find('"', pos);
        if (q == std::string_view::npos) {
            if (!first) out.append(" + ");
            out.push_back('"');
            out.append(raw.substr(pos));
            out.push_back('"');
            break;
        }
        if (q > pos || first) {
            if (!first) out.append(" + ");
            out.push_back('"');
            out.append(raw.substr(pos, q - pos));
            out.push_back('"');
            first = false;
        }
        if (!first) out.append(" + ");
        out.append("'\"'");
        first = false;
        pos = q + 1;
    }
    return out;
}

// Checks whether a GML snippet ends inside an unclosed /* ... block comment.
bool has_unclosed_block_comment(std::string_view code) noexcept {
    bool in_block = false;
    char quote = 0;
    for (usize i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (in_block) {
            if (c == '*' && i + 1 < code.size() && code[i + 1] == '/') {
                in_block = false;
                ++i;
            }
            continue;
        }
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '/' && i + 1 < code.size()) {
            if (code[i + 1] == '/') {
                while (i < code.size() && code[i] != '\n') ++i;
            } else if (code[i + 1] == '*') {
                in_block = true;
                ++i;
            }
        }
    }
    return in_block;
}

class DndLowerer {
public:
    DndLowerer(const EventData& ev, const GameData& game) : ev_(ev), game_(game) {}

    std::string lower() {
        if (ev_.actions.empty()) {
            return {};
        }

        // Fast path: single ACT_CODE targeting self with no relative flag — return directly.
        if (ev_.actions.size() == 1) {
            const auto& a = ev_.actions[0];
            if (a.kind == kActCode && (!a.use_apply_to || a.who == -1) && !a.args.empty()) {
                return a.args[0];
            }
        }

        // Determine if relative flag toggles anywhere in this event.
        int rel_changes = 0;
        bool prev_rel = false;
        for (const auto& a : ev_.actions) {
            if (a.use_relative) {
                if (prev_rel != a.relative) ++rel_changes;
                prev_rel = a.relative;
            }
        }
        if (rel_changes > 0) {
            for (const auto& a : ev_.actions) {
                if (a.use_relative) {
                    rel_stack_.push_back(a.relative);
                    break;
                }
            }
        }

        out_.append("{\n");
        if (!rel_stack_.empty()) {
            out_.append(rel_stack_.back() ? "action_set_relative(1);\n" : "action_set_relative(0);\n");
        }

        usize idx = 0;
        while (idx < ev_.actions.size()) {
            idx = emit_action(idx);
        }

        if (!rel_stack_.empty()) {
            out_.append("action_set_relative(0);\n");
            rel_stack_.pop_back();
        }
        out_.append("}\n");
        return out_;
    }

private:
    bool push_relative(const ActionData& a) {
        if (a.use_relative && !rel_stack_.empty() && rel_stack_.back() != a.relative) {
            out_.append(a.relative ? "action_set_relative(1);\n" : "action_set_relative(0);\n");
            rel_stack_.push_back(a.relative);
            return true;
        }
        return false;
    }

    void pop_relative() {
        const bool popped = rel_stack_.back();
        rel_stack_.pop_back();
        if (!rel_stack_.empty() && rel_stack_.back() != popped) {
            out_.append(rel_stack_.back() ? "action_set_relative(1);\n" : "action_set_relative(0);\n");
        }
    }

    void emit_func_args(const ActionData& a) {
        const usize argc = static_cast<usize>(a.arg_count > 0 ? a.arg_count : 0);
        for (usize i = 0; i < argc; ++i) {
            if (i > 0) out_.append(", ");
            const std::string_view val = (i < a.args.size()) ? std::string_view(a.args[i]) : std::string_view{};
            const i32 type = (i < a.arg_types.size()) ? a.arg_types[i] : kArgExpression;

            if (type == kArgString) {
                out_.append(quote_gml_literal(val));
            } else if (type == kArgStringExp) {
                const std::string_view tv = trim_sv(val);
                if (!tv.empty() && (tv.front() == '"' || tv.front() == '\'')) {
                    out_.append(val);
                } else {
                    out_.append(quote_gml_literal(val));
                }
            } else {
                const std::string_view tv = trim_sv(val);
                out_.append(tv.empty() ? "0" : tv);
            }
        }
    }

    void emit_execute_script_call(const ActionData& a) {
        if (a.args.empty()) return;
        const i32 script_id = static_cast<i32>(std::strtol(a.args[0].c_str(), nullptr, 10));
        if (script_id < 0 || static_cast<usize>(script_id) >= game_.scripts.size() ||
            !game_.scripts[static_cast<usize>(script_id)].exists) {
            out_.append("0");
            return;
        }
        const std::string& script_name = game_.scripts[static_cast<usize>(script_id)].name;
        out_.append(script_name);
        out_.push_back('(');

        // Up to 5 arguments in args[1..5]; find last non-empty argument.
        usize last_arg = 0;
        const usize max_arg = std::min<usize>(a.args.size(), 6);
        for (usize i = 1; i < max_arg; ++i) {
            if (!trim_sv(a.args[i]).empty()) {
                last_arg = i;
            }
        }
        for (usize i = 1; i <= last_arg; ++i) {
            if (i > 1) out_.append(", ");
            const std::string_view tv = trim_sv(a.args[i]);
            out_.append(tv.empty() ? "0" : tv);
        }
        out_.push_back(')');
    }

    usize emit_block(usize n) {
        out_.append("{\n");
        ++n;
        while (n < ev_.actions.size()) {
            if (ev_.actions[n].kind == kActEnd) {
                ++n;
                break;
            }
            n = emit_action(n);
        }
        out_.append("}\n");
        return n;
    }

    usize emit_repeat(usize n) {
        const auto& a = ev_.actions[n];
        const std::string_view cnt = (!a.args.empty() && !trim_sv(a.args[0]).empty())
                                         ? trim_sv(a.args[0])
                                         : std::string_view("0");
        out_.append("repeat (");
        out_.append(cnt);
        out_.append(")\n");
        if (n + 1 < ev_.actions.size()) {
            return emit_action(n + 1);
        }
        out_.append("{}\n");
        return n + 1;
    }

    usize emit_condition(usize n) {
        const auto& a = ev_.actions[n];
        bool needs_block = false;
        bool existential_with = false;

        if (!cond_var_declared_) {
            out_.append("var __b__;\n");
            cond_var_declared_ = true;
        }
        out_.append("__b__ = 0;\n");

        if (a.use_apply_to && a.who != -1) {
            if (a.who == -2) {
                out_.append("with (other)\n");
            } else {
                out_.append("with (" + std::to_string(a.who) + ")\n");
                existential_with = true;
            }
            needs_block = true;
        }
        if (a.use_relative && !rel_stack_.empty() && rel_stack_.back() != a.relative) {
            needs_block = true;
        }

        if (needs_block) out_.append("{\n");
        const bool pushed_rel = push_relative(a);

        if (a.exe_type == kExeFunction) {
            out_.append("__b__ = ");
            if (a.name == "action_execute_script") {
                emit_execute_script_call(a);
                out_.append(";\n");
            } else {
                out_.append(a.name);
                out_.append("(");
                emit_func_args(a);
                out_.append(");\n");
            }
        } else if (a.exe_type == kExeCode) {
            // Custom GML question action: evaluate expression/code.
            out_.append("__b__ = (");
            out_.append(a.code.empty() ? "0" : a.code);
            out_.append(");\n");
        }

        if (existential_with) {
            out_.append(a.is_not ? "if (!__b__) break;\n" : "if (__b__) break;\n");
        }
        if (pushed_rel) pop_relative();
        if (needs_block) out_.append("}\n");

        out_.append(a.is_not ? "if (!__b__)\n{\n" : "if (__b__)\n{\n");
        n = emit_action(n + 1);
        out_.append("}\n");

        if (n < ev_.actions.size() && ev_.actions[n].kind == kActElse) {
            out_.append("else\n{\n");
            n = emit_action(n + 1);
            out_.append("}\n");
        }
        return n;
    }

    usize emit_normal(usize n) {
        const auto& a = ev_.actions[n];
        if (a.kind == kActNormal && a.exe_type == kExeNothing) {
            return n + 1;
        }

        bool needs_block = false;
        if (a.use_apply_to && a.who != -1) {
            if (a.who == -2) {
                out_.append("with (other)\n");
            } else {
                out_.append("with (" + std::to_string(a.who) + ")\n");
            }
            needs_block = true;
        }
        if (a.use_relative && !rel_stack_.empty() && rel_stack_.back() != a.relative) {
            needs_block = true;
        }

        if (needs_block) out_.append("{\n");
        const bool pushed_rel = push_relative(a);

        if (a.kind == kActVariable) {
            if (a.args.size() >= 2) {
                out_.append(a.args[0]);
                out_.append(a.relative ? " += (" : " = (");
                out_.append(trim_sv(a.args[1]).empty() ? "0" : a.args[1]);
                out_.append(");\n");
            }
        } else if (a.kind == kActCode) {
            const std::string_view code = (!a.args.empty()) ? std::string_view(a.args[0]) : std::string_view(a.code);
            out_.append("{\n");
            out_.append(code);
            if (has_unclosed_block_comment(code)) {
                out_.append("*/");
            }
            out_.append("\n}\n");
        } else if (a.exe_type == kExeFunction) {
            if (a.name == "action_execute_script") {
                emit_execute_script_call(a);
                out_.append(";\n");
            } else {
                out_.append(a.name);
                out_.append("(");
                emit_func_args(a);
                out_.append(");\n");
            }
        } else if (a.exe_type == kExeCode) {
            out_.append("{\n");
            out_.append(a.code);
            if (has_unclosed_block_comment(a.code)) {
                out_.append("*/");
            }
            out_.append("\n}\n");
        }

        if (pushed_rel) pop_relative();
        if (needs_block) out_.append("}\n");
        return n + 1;
    }

    usize emit_action(usize n) {
        if (n >= ev_.actions.size()) return n;
        const auto& a = ev_.actions[n];
        switch (a.kind) {
            case kActExit:
                if (!rel_stack_.empty()) {
                    out_.append("action_set_relative(0);\n");
                }
                out_.append("exit;\n");
                return n + 1;
            case kActBegin:
                return emit_block(n);
            case kActEnd:
            case kActElse:
                // Stray unmatched END or ELSE in malformed D&D list: skip safely.
                return n + 1;
            case kActRepeat:
                return emit_repeat(n);
            default:
                if (a.is_question) {
                    return emit_condition(n);
                }
                return emit_normal(n);
        }
    }

    const EventData& ev_;
    const GameData& game_;
    std::string out_;
    std::vector<bool> rel_stack_;
    bool cond_var_declared_ = false;
};

} // namespace

std::string lower_dnd_event_to_gml(const EventData& event, const GameData& game) {
    DndLowerer lowerer(event, game);
    return lowerer.lower();
}

} // namespace gm

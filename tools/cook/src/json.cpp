#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ve::cook::json {
namespace {

struct P {
    const std::string& t;
    std::size_t i = 0;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
    }
    bool fail(std::string* err, const std::string& msg) {
        if (err) *err = msg + " at offset " + std::to_string(i);
        return false;
    }

    bool value(Value& v, std::string* err) {
        ws();
        if (i >= t.size()) return fail(err, "unexpected end");
        const char c = t[i];
        if (c == '{') return object(v, err);
        if (c == '[') return array(v, err);
        if (c == '"') return string(v, err);
        if (c == 't' || c == 'f') return boolean(v, err);
        if (c == 'n') {
            if (t.compare(i, 4, "null") != 0) return fail(err, "bad literal");
            i += 4; v = Value{}; return true;
        }
        return number(v, err);
    }

    bool object(Value& v, std::string* err) {
        auto o = std::make_shared<Obj>();
        ++i; // '{'
        ws();
        if (i < t.size() && t[i] == '}') { ++i; v.kind = Value::Kind::Obj; v.obj = o; return true; }
        while (true) {
            ws();
            Value key;
            if (!string(key, err)) return false;
            ws();
            if (i >= t.size() || t[i] != ':') return fail(err, "expected ':'");
            ++i;
            Value val;
            if (!value(val, err)) return false;
            (*o)[key.str] = std::move(val);
            ws();
            if (i >= t.size()) return fail(err, "unterminated object");
            if (t[i] == ',') { ++i; continue; }
            if (t[i] == '}') { ++i; break; }
            return fail(err, "expected ',' or '}'");
        }
        v.kind = Value::Kind::Obj; v.obj = o;
        return true;
    }

    bool array(Value& v, std::string* err) {
        auto a = std::make_shared<Arr>();
        ++i; // '['
        ws();
        if (i < t.size() && t[i] == ']') { ++i; v.kind = Value::Kind::Arr; v.arr = a; return true; }
        while (true) {
            Value val;
            if (!value(val, err)) return false;
            a->push_back(std::move(val));
            ws();
            if (i >= t.size()) return fail(err, "unterminated array");
            if (t[i] == ',') { ++i; continue; }
            if (t[i] == ']') { ++i; break; }
            return fail(err, "expected ',' or ']'");
        }
        v.kind = Value::Kind::Arr; v.arr = a;
        return true;
    }

    bool string(Value& v, std::string* err) {
        if (i >= t.size() || t[i] != '"') return fail(err, "expected string");
        ++i;
        std::string s;
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') { v.kind = Value::Kind::Str; v.str = std::move(s); return true; }
            if (c == '\\') {
                if (i >= t.size()) break;
                const char e = t[i++];
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        // basic BMP unescape (hex4 -> UTF-8)
                        if (i + 4 > t.size()) return fail(err, "bad \\u escape");
                        const std::string hex = t.substr(i, 4);
                        i += 4;
                        const unsigned cp = std::strtoul(hex.c_str(), nullptr, 16);
                        if (cp < 0x80) s += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            s += static_cast<char>(0xC0 | (cp >> 6));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (cp >> 12));
                            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return fail(err, "bad escape");
                }
            } else {
                s += c;
            }
        }
        return fail(err, "unterminated string");
    }

    bool boolean(Value& v, std::string* err) {
        if (t.compare(i, 4, "true") == 0) { i += 4; v.kind = Value::Kind::Bool; v.b = true; return true; }
        if (t.compare(i, 5, "false") == 0) { i += 5; v.kind = Value::Kind::Bool; v.b = false; return true; }
        return fail(err, "bad literal");
    }

    bool number(Value& v, std::string* err) {
        char* end = nullptr;
        const double d = std::strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) return fail(err, "bad number");
        i = static_cast<std::size_t>(end - t.c_str());
        v.kind = Value::Kind::Num; v.num = d;
        return true;
    }
};

void write_value(const Value& v, std::string& out) {
    using K = Value::Kind;
    switch (v.kind) {
        case K::Null: out += "null"; break;
        case K::Bool: out += v.b ? "true" : "false"; break;
        case K::Num: {
            // shortest round-trip; integers print without '.0'
            char buf[64];
            const int n = std::snprintf(buf, sizeof buf, "%.17g", v.num);
            out.append(buf, static_cast<std::size_t>(n));
            break;
        }
        case K::Str: {
            out += '"';
            for (const char c : v.str) {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (static_cast<unsigned char>(c) < 0x20) out += '?';
                else out += c;
            }
            out += '"';
            break;
        }
        case K::Arr: {
            out += '[';
            bool first = true;
            for (const auto& e : v.a()) { if (!first) out += ','; write_value(e, out); first = false; }
            out += ']';
            break;
        }
        case K::Obj: {
            out += '{';
            bool first = true;
            for (const auto& [k, e] : v.o()) { // map order = deterministic
                if (!first) out += ',';
                Value key_val;
                key_val.kind = K::Str;
                key_val.str = k;
                write_value(key_val, out);
                out += ':';
                write_value(e, out);
                first = false;
            }
            out += '}';
            break;
        }
    }
}

} // namespace

bool parse(const std::string& text, Value& out, std::string* err) {
    P p{text};
    if (!p.value(out, err)) return false;
    p.ws();
    if (p.i != text.size()) return p.fail(err, "trailing content");
    return true;
}

std::string write(const Value& v) {
    std::string out;
    write_value(v, out);
    return out;
}

} // namespace ve::cook::json

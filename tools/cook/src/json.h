#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

// Minimal JSON reader for the cooklet (glTF JSON chunks). Tools-only:
// doctrine keeps *text* parsing out of runtime; cooked output is binary.
// Not a general-purpose parser — no \uXXXX escapes beyond basics, no NaN.
namespace ve::cook::json {

struct Value;
using Arr = std::vector<Value>;
using Obj = std::map<std::string, Value>; // ordered: deterministic iteration

struct Value {
    enum class Kind { Null, Bool, Num, Str, Arr, Obj } kind = Kind::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::shared_ptr<Arr> arr;
    std::shared_ptr<Obj> obj;

    bool is_obj() const { return kind == Kind::Obj; }
    bool is_arr() const { return kind == Kind::Arr; }
    bool is_num() const { return kind == Kind::Num; }
    bool is_str() const { return kind == Kind::Str; }
    const Obj& o() const { return *obj; }
    const Arr& a() const { return *arr; }
    const Value* find(const std::string& key) const {
        if (!is_obj()) return nullptr;
        auto it = obj->find(key);
        return it == obj->end() ? nullptr : &it->second;
    }
    double d() const { return num; }
    const std::string& s() const { return str; }
};

// Parse whole buffer. On failure returns false with position-tagged error.
bool parse(const std::string& text, Value& out, std::string* err);

// Re-serialize with keys in map order (already deterministic) — used to
// embed extras verbatim-but-stable into cooked units.
std::string write(const Value& v);

} // namespace ve::cook::json

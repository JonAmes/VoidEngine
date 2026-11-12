// Phase 8 task 8.2 — def-forms panel (see def_forms_panel.h).
//
// Two halves:
//  def_forms_doc   line-oriented TOML model. Parse maps every schema-field
//                  occurrence to its authored text; set_leaf re-emits only
//                  the touched line(s) -> minimal git-diff write-back.
//  def_forms_panel immediate-UI form over every leaf, 8.1 typed widgets,
//                  gate-before-dirty (#16: host writes the .toml, the
//                  cooklet re-cooks; the panel touches neither payloads).
#include "ve/game/def_forms_panel.h"

#include <charconv>
#include <cmath>
#include <cstring>

#include "ve/game/attr_keys.h"
#include "ve/game/units.h"
#include "ve/gfx/ui.h"

namespace ve::game {
namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parse_bare_double(std::string_view tok, double& out) {
    const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), out);
    return res.ec == std::errc{} && res.ptr == tok.data() + tok.size() && std::isfinite(out);
}

bool is_bare_number(std::string_view tok) {
    double v = 0.0;
    return parse_bare_double(trimmed(tok), v);
}

// "a.b" -> "quoted-literal" (forms re-emit authored strings quoted; the
// fixture style uses double quotes with no escapes in practice, but
// quote/escape anyway).
std::string quoted(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

// Strip surrounding quotes + unescape ("" -> raw text).
std::string unquoted(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) out += s[++i];
        else out += s[i];
    }
    return out;
}

// Split a TOML value at top-level commas (quote- and bracket-aware).
std::vector<std::string> split_top(std::string_view v) {
    std::vector<std::string> parts;
    int depth = 0;
    bool in_q = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (in_q) {
            if (c == '\\') ++i;
            else if (c == '"') in_q = false;
        } else if (c == '"') in_q = true;
        else if (c == '[') ++depth;
        else if (c == ']') --depth;
        else if (c == ',' && depth == 0) {
            parts.emplace_back(trimmed(v.substr(start, i - start)));
            start = i + 1;
        }
    }
    const auto last = trimmed(v.substr(start));
    if (!last.empty() || !parts.empty()) parts.emplace_back(last);
    return parts;
}

// Flatten a (possibly nested) array literal into scalar tokens.
std::vector<std::string> flatten(std::string_view v) {
    std::vector<std::string> out;
    for (const auto& p : split_top(v))
        if (!p.empty() && p.front() == '[')
            for (auto& q : flatten(p.substr(1, p.size() - 2))) out.push_back(std::move(q));
        else
            out.push_back(p);
    return out;
}

// Strip a trailing " #..." comment (quote-aware). Returns value + raw
// comment text (with its leading spacing preserved) for verbatim re-emit.
void split_comment(std::string_view v, std::string_view* value, std::string* comment) {
    bool in_q = false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (in_q) {
            if (c == '\\') ++i;
            else if (c == '"') in_q = false;
        } else if (c == '"') in_q = true;
        else if (c == '#') {
            *value = trimmed(v.substr(0, i));
            *comment = std::string(v.substr(i));
            return;
        }
    }
    *value = trimmed(v);
    comment->clear();
}

std::string header_name(std::string_view line) { // [[x]] / [x] -> x
    auto a = line.find('[');
    auto b = line.rfind(']');
    if (a == std::string_view::npos || b == std::string_view::npos || b <= a) return {};
    std::string_view n = line.substr(a + 1, b - a - 1);
    if (!n.empty() && n.front() == '[') n.remove_prefix(1);
    if (!n.empty() && n.back() == ']') n.remove_suffix(1);
    return std::string(trimmed(n));
}

const def_field* find_scalar(const def_schema& s, std::string_view full) {
    for (const auto& f : s.scalars)
        if (f.path == full) return &f;
    return nullptr;
}

const def_field* find_row_field(const def_schema& s, std::string_view row,
                                std::string_view key) {
    for (const auto& r : s.rows)
        if (r.row == row)
            for (const auto& f : r.fields)
                if (f.path == key) return &f;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- doc ----

bool def_forms_doc::load(const def_schema& schema, std::string source, std::string* err) {
    schema_ = &schema;
    auto fail = [&](int line, const std::string& msg) {
        if (err) *err = "line " + std::to_string(line + 1) + ": " + msg;
        return false;
    };

    raw_.clear();
    {
        std::size_t start = 0;
        while (true) {
            const auto nl = source.find('\n', start);
            if (nl == std::string::npos) { raw_.emplace_back(source.substr(start)); break; }
            raw_.emplace_back(source.substr(start, nl - start));
            start = nl + 1;
        }
    }
    src_.assign(raw_.size(), line_src{});
    leaves_.clear();
    idx_.clear();
    order_.clear();

    std::string tbl_prefix;              // "[grid]" -> "grid."
    std::string row_ctx;                 // innermost [[row]] full name
    std::string row_prefix;              // "component.0."
    std::map<std::string, int> row_count;
    int attrs_leaf = -1;                 // open attr-map block
    std::string attrs_value;

    auto close_block = [&](int end_line) {
        if (attrs_leaf < 0) return;
        // The body ends at the last authored entry: trailing blank lines
        // before the next header belong to the FILE's spacing, not the
        // table (reemit must not eat them — minimal-delta rule).
        while (end_line > 0 && trimmed(raw_[static_cast<std::size_t>(end_line)]).empty())
            --end_line;
        auto& l = leaves_[static_cast<std::size_t>(attrs_leaf)];
        l.value = attrs_value;
        auto& ls = src_[static_cast<std::size_t>(l.line)];
        ls.block_start = l.line;
        ls.block_end = end_line;
        attrs_leaf = -1;
    };

    for (std::size_t i = 0; i < raw_.size(); ++i) {
        const std::string_view line = trimmed(raw_[i]);
        if (line.empty() || line.front() == '#') continue;

        if (line.size() >= 2 && line[0] == '[' && line[1] == '[') {
            close_block(static_cast<int>(i) - 1);
            const std::string name = header_name(line);
            if (name.empty()) return fail(static_cast<int>(i), "malformed [[header]]");
            const auto dot = name.find('.');
            std::string base = name, tail;
            if (dot != std::string::npos) { // [[component.facet]] nests under last parent
                base = name.substr(0, dot);
                tail = name.substr(dot); // ".facet"
                if (!row_count.count(base) || row_count[base] == 0)
                    return fail(static_cast<int>(i), name + " has no parent [[" + base + "]]");
            }
            ++row_count[name];
            if (tail.empty()) {
                for (auto it = row_count.begin(); it != row_count.end();)
                    if (it->first.size() > name.size() + 1 &&
                        it->first.compare(0, name.size() + 1, name + ".") == 0)
                        it = row_count.erase(it);
                    else
                        ++it;
                row_ctx = name;
                row_prefix = name + "." + std::to_string(row_count[name] - 1) + ".";
                tbl_prefix.clear();
            } else {
                row_ctx = name;
                row_prefix = base + "." + std::to_string(row_count[base] - 1) + tail + "." +
                             std::to_string(row_count[name] - 1) + ".";
                tbl_prefix.clear();
            }
            continue;
        }
        if (line.front() == '[') {
            close_block(static_cast<int>(i) - 1);
            const std::string name = header_name(line);
            if (name.empty()) return fail(static_cast<int>(i), "malformed [header]");
            // [component.attrs] -> attr-map leaf on the current row.
            const auto dot = name.rfind('.');
            if (dot != std::string_view::npos && name.substr(dot + 1) == "attrs") {
                const std::string row = std::string(name.substr(0, dot));
                const def_field* f = find_row_field(schema, row, "attrs");
                if (f && row_ctx == row && !row_prefix.empty()) {
                    leaf l;
                    l.path = row_prefix + "attrs";
                    l.field = f;
                    l.line = static_cast<int>(i);
                    attrs_leaf = static_cast<int>(leaves_.size());
                    attrs_value.clear();
                    leaves_.push_back(std::move(l));
                    idx_[leaves_.back().path] = attrs_leaf;
                    order_.push_back(leaves_.back().path);
                    continue;
                }
            }
            tbl_prefix = name + ".";
            row_ctx.clear();
            row_prefix.clear();
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string_view::npos)
            return fail(static_cast<int>(i), "forms need line-oriented key = value");

        if (attrs_leaf >= 0) { // consumed by the open attr-map leaf
            std::string_view val;
            std::string comment;
            split_comment(line.substr(eq + 1), &val, &comment);
            const std::string key{trimmed(line.substr(0, eq))};
            if (!attrs_value.empty()) attrs_value += ", ";
            attrs_value += key + "=" + unquoted(trimmed(val));
            continue;
        }

        const std::string key{trimmed(line.substr(0, eq))};
        std::string_view val;
        std::string comment;
        split_comment(line.substr(eq + 1), &val, &comment);
        const std::string full = !row_prefix.empty() ? row_prefix + key : tbl_prefix + key;

        const def_field* f = nullptr;
        std::string leaf_path = full;
        if (!row_prefix.empty()) {
            const auto dot = row_ctx.rfind('.');
            f = find_row_field(schema, row_ctx, key);
            if (f) {
                // strip row indices from the path prefix? NO — leaf paths
                // keep the file position (component.2.socket) so refs and
                // the panel can address individual rows.
                leaf_path = row_prefix + key;
            }
            (void)dot;
        } else {
            f = find_scalar(schema, full);
        }
        if (!f) return fail(static_cast<int>(i), "key '" + full + "' is not in the def schema");

        auto add_leaf = [&](std::string path, std::string value, int slot) {
            leaf l;
            l.path = std::move(path);
            l.value = std::move(value);
            l.field = f;
            l.line = static_cast<int>(i);
            l.slot = slot;
            idx_[l.path] = static_cast<int>(leaves_.size());
            order_.push_back(l.path);
            leaves_.push_back(std::move(l));
        };

        line_src& ls = src_[i];
        ls.indent = raw_[i].substr(0, raw_[i].find_first_not_of(" \t"));
        ls.key = key;
        ls.comment = std::move(comment);

        const bool is_array = !val.empty() && val.front() == '[';
        auto add_scalar = [&](char form, std::string v) {
            ls.form = form;
            add_leaf(leaf_path, std::move(v), 0);
            ls.leaf_ids.push_back(idx_[leaf_path]);
        };
        switch (f->kind) {
        case field_kind::unit_str:
        case field_kind::number_si: {
            const int arity = f->arity ? f->arity : 1;
            if (arity == 1) {
                if (is_array) return fail(static_cast<int>(i), "scalar field has an array value");
                const bool num = f->kind == field_kind::number_si;
                add_scalar(num ? 'n' : 's',
                           num ? std::string(trimmed(val)) : unquoted(trimmed(val)));
            } else {
                if (!is_array) return fail(static_cast<int>(i), "array field needs [..]");
                const auto parts = split_top(val.substr(1, val.size() - 2));
                if (parts.size() != static_cast<std::size_t>(arity))
                    return fail(static_cast<int>(i), "array needs exactly " + std::to_string(arity));
                const bool num = f->kind == field_kind::number_si;
                ls.form = num ? 'N' : 'A';
                for (std::size_t k = 0; k < parts.size(); ++k) {
                    const std::string pth = leaf_path + "." + std::to_string(k);
                    add_leaf(pth, num ? std::string(trimmed(parts[k]))
                                      : unquoted(trimmed(parts[k])),
                             static_cast<int>(k));
                    ls.leaf_ids.push_back(idx_[pth]);
                }
            }
            break;
        }
        case field_kind::integer:
            if (is_array) return fail(static_cast<int>(i), "scalar field has an array value");
            add_scalar('n', std::string(trimmed(val)));
            break;
        case field_kind::boolean:
            if (is_array) return fail(static_cast<int>(i), "scalar field has an array value");
            add_scalar('b', std::string(trimmed(val)));
            break;
        case field_kind::text:
        case field_kind::id:
        case field_kind::tag:
        case field_kind::enumeration:
        case field_kind::endpoint:
        case field_kind::name_ref:
        case field_kind::def_path:
            if (is_array) return fail(static_cast<int>(i), "scalar field has an array value");
            add_scalar('s', unquoted(trimmed(val)));
            break;
        case field_kind::tag_list: {
            if (!is_array) return fail(static_cast<int>(i), "tag list needs [..]");
            std::string joined;
            for (const auto& t : split_top(val.substr(1, val.size() - 2))) {
                if (!joined.empty()) joined += ' ';
                joined += unquoted(t);
            }
            ls.form = 'T';
            add_leaf(leaf_path, std::move(joined), 0);
            ls.leaf_ids.push_back(idx_[leaf_path]);
            break;
        }
        case field_kind::path_list: {
            if (!is_array) return fail(static_cast<int>(i), "path list needs [..]");
            ls.form = 'A';
            const auto parts = split_top(val.substr(1, val.size() - 2));
            for (std::size_t k = 0; k < parts.size(); ++k) {
                const std::string pth = leaf_path + "." + std::to_string(k);
                add_leaf(pth, unquoted(parts[k]), static_cast<int>(k));
                ls.leaf_ids.push_back(idx_[pth]);
            }
            break;
        }
        case field_kind::int_boxes: {
            if (!is_array) return fail(static_cast<int>(i), "cells need [[..]]");
            std::string joined;
            for (const auto& t : flatten(val)) {
                if (!joined.empty()) joined += ',';
                joined += t;
            }
            ls.form = 'X';
            add_leaf(leaf_path, std::move(joined), 0);
            ls.leaf_ids.push_back(idx_[leaf_path]);
            break;
        }
        case field_kind::attr_map:
            return fail(static_cast<int>(i), "attr maps use a [name.attrs] table");
        }
    }
    close_block(static_cast<int>(raw_.size()) - 1);
    if (attrs_leaf >= 0) return fail(0, "unterminated attr map");
    rebuild_source();
    return true;
}

void def_forms_doc::rebuild_source() {
    std::string s;
    for (std::size_t i = 0; i < raw_.size(); ++i) {
        s += raw_[i];
        if (i + 1 < raw_.size()) s += '\n';
    }
    source_ = std::move(s);
}

bool def_forms_doc::reemit_line(int line_idx) {
    line_src& ls = src_[static_cast<std::size_t>(line_idx)];
    if (!ls.form) return false;
    std::string lit;
    auto lv = [&](std::size_t k) -> const std::string& {
        return leaves_[static_cast<std::size_t>(ls.leaf_ids[k])].value;
    };
    switch (ls.form) {
    case 's': lit = quoted(lv(0)); break;
    case 'n': case 'b': lit = lv(0); break;
    case 'A':
        lit = "[ ";
        for (std::size_t k = 0; k < ls.leaf_ids.size(); ++k) {
            if (k) lit += ", ";
            lit += quoted(lv(k));
        }
        lit += " ]";
        break;
    case 'N':
        lit = "[ ";
        for (std::size_t k = 0; k < ls.leaf_ids.size(); ++k) {
            if (k) lit += ", ";
            lit += lv(k);
        }
        lit += " ]";
        break;
    case 'T': {
        lit = "[ ";
        std::string_view v = lv(0);
        bool first = true;
        while (!v.empty()) {
            const auto sp = v.find(' ');
            const auto tok = trimmed(sp == std::string_view::npos ? v : v.substr(0, sp));
            if (!tok.empty()) {
                if (!first) lit += ", ";
                lit += quoted(tok);
                first = false;
            }
            if (sp == std::string_view::npos) break;
            v = v.substr(sp + 1);
        }
        lit += " ]";
        break;
    }
    case 'X': {
        // int_boxes: comma-joined scalars re-grouped 6 per box.
        std::vector<std::string> nums;
        for (const auto& t : split_top(lv(0))) nums.push_back(t);
        lit = "[ ";
        for (std::size_t b = 0; b * 6 < nums.size(); ++b) {
            if (b) lit += ", ";
            lit += "[";
            for (std::size_t k = 0; k < 6 && b * 6 + k < nums.size(); ++k) {
                if (k) lit += ", ";
                lit += nums[b * 6 + k];
            }
            lit += "]";
        }
        lit += " ]";
        break;
    }
    default: return false;
    }
    raw_[static_cast<std::size_t>(line_idx)] =
        ls.indent + ls.key + " = " + lit +
        (ls.comment.empty() ? std::string{} : "   " + ls.comment);
    return true;
}

bool def_forms_doc::reemit_block(int leaf_id) {
    const leaf& l = leaves_[static_cast<std::size_t>(leaf_id)];
    const int bs = src_[static_cast<std::size_t>(l.line)].block_start;
    const int be = src_[static_cast<std::size_t>(l.line)].block_end;
    if (bs < 0 || be < bs) return false;
    // Preserve the AUTHORED body indent (from the first entry line, not
    // the header) — re-emission stays byte-compatible with the file's own
    // style; hardcoding 4 spaces would reindent 0-indent tables.
    const std::string indent =
        raw_[static_cast<std::size_t>(bs + 1)].substr(0, raw_[static_cast<std::size_t>(bs + 1)].find_first_not_of(" \t"));
    std::vector<std::string> rebuilt; // replacement body lines (header stays at bs)
    // Authored trailing comments on old entry lines ride along when the
    // same key is re-emitted (#48: the delta should not silently delete
    // author notes).
    auto old_comment = [&](const std::string& key) {
        for (int li = bs + 1; li <= be; ++li) {
            const std::string& ol = raw_[static_cast<std::size_t>(li)];
            const std::string_view t = trimmed(ol);
            if (t.empty() || t.front() == '#' || t.front() == '[') continue;
            if (t.substr(0, key.size()) != key) continue;
            const auto h = ol.find('#');
            if (h != std::string::npos)
                return std::string{"   "} + std::string{trimmed(ol.substr(h))};
            break;
        }
        return std::string{};
    };
    for (const auto& tok : split_top(l.value)) {
        const auto eq = tok.find('=');
        if (eq == std::string::npos) return false;
        const std::string key{trimmed(tok.substr(0, eq))};
        const std::string val{trimmed(tok.substr(eq + 1))};
        rebuilt.push_back(indent + key + " = " +
                          (is_bare_number(val) ? val : quoted(val)) + old_comment(key));
    }
    const int old_n = be - bs;   // body lines after the header
    const int new_n = static_cast<int>(rebuilt.size());
    raw_.erase(raw_.begin() + bs + 1, raw_.begin() + be + 1);
    raw_.insert(raw_.begin() + bs + 1, rebuilt.begin(), rebuilt.end());
    src_.erase(src_.begin() + bs + 1, src_.begin() + be + 1);
    src_.insert(src_.begin() + bs + 1, new_n, line_src{});
    // Line indices shift for everything below the block (blocks are rare;
    // a rescan is cheaper than bookkeeping).
    const int delta = new_n - old_n;
    for (auto& o : leaves_) {
        if (o.line > be) o.line += delta;
        auto& ols = src_[static_cast<std::size_t>(o.line)];
        if (ols.block_start > be) ols.block_start += delta;
        // >= : the touched block's OWN end sits at be — when the rebuilt
        // body changed line count it must move too, or a later reemit
        // (e.g. a gate-rejection rollback) erases the wrong span and
        // leaves orphan lines in the source.
        if (ols.block_end >= be) ols.block_end += delta;
    }
    return true;
}

std::string def_forms_doc::value(std::string_view path) const {
    const auto it = idx_.find(std::string(path));
    return it == idx_.end() ? std::string{} : leaves_[static_cast<std::size_t>(it->second)].value;
}

const def_field* def_forms_doc::field(std::string_view path) const {
    const auto it = idx_.find(std::string(path));
    return it == idx_.end() ? nullptr : leaves_[static_cast<std::size_t>(it->second)].field;
}

namespace {
// Sane default literal for add_child: a REQUIRED field gets a value that
// passes its own entry grammar; whether it passes the GATE is the author's
// next edit (#48: easy to fix).
std::string default_literal(const def_field& f) {
    switch (f.kind) {
    case field_kind::enumeration: {
        const auto cm = f.enum_set.find(',');
        return "\"" + std::string(cm == std::string_view::npos ? f.enum_set
                                                              : f.enum_set.substr(0, cm)) + "\"";
    }
    case field_kind::integer:
    case field_kind::number_si:
        return "0";
    case field_kind::boolean:
        return "false";
    case field_kind::unit_str:
        return f.dim == unit_dim::time ? "\"0 s\""
                                       : f.dim == unit_dim::angle ? "\"0 deg\"" : "\"0 m\"";
    case field_kind::tag:
        return "\"new.tag\"";
    case field_kind::tag_list:
        return "[\"new.tag\"]";
    case field_kind::attr_map:
        return "";  // never emitted (not required in any ratified row)
    case field_kind::text:
    case field_kind::id:
    case field_kind::def_path:
    case field_kind::name_ref:
        return "\"new_part\"";
    case field_kind::endpoint:
        return "\"component.new_part\"";
    case field_kind::path_list:
        return "[]";
    case field_kind::int_boxes:
        return "[[0, 0, 0, 0, 0, 0]]";
    }
    return "\"\"";
}
}  // namespace

bool def_forms_doc::add_child(std::string_view row, std::string* err) {
    if (!schema_) { if (err) *err = "no schema (call load first)"; return false; }
    if (row.find('.') != std::string_view::npos) {
        if (err) *err = "add_child only supports top-level array rows";
        return false;
    }
    const def_row* hit = nullptr;
    for (const auto& r : schema_->rows) if (r.row == row) hit = &r;
    if (!hit) { if (err) *err = "unknown row '" + std::string(row) + "'"; return false; }
    std::string block = "\n[[" + std::string(row) + "]]\n";
    for (const auto& f : hit->fields)
        if (f.required && f.kind != field_kind::attr_map)
            block += std::string(f.path) + " = " + default_literal(f) + "\n";
    const std::string old = source_;
    const std::string joined = (!old.empty() && old.back() != '\n') ? old + "\n" : old;
    if (!load(*schema_, joined + block, err)) {
        load(*schema_, old, nullptr);  // keep the doc usable if the append broke it
        return false;
    }
    return true;
}

bool def_forms_doc::set_leaf(std::string_view path, std::string value) {
    const auto it = idx_.find(std::string(path));
    if (it == idx_.end()) return false;
    leaf& l = leaves_[static_cast<std::size_t>(it->second)];
    l.value = std::move(value);
    const line_src& ls = src_[static_cast<std::size_t>(l.line)];
    bool ok = false;
    if (ls.block_start >= 0) ok = reemit_block(it->second);
    else ok = reemit_line(l.line);
    if (ok) rebuild_source();
    return ok;
}

}  // namespace ve::game

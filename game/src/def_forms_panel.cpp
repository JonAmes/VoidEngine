// Phase 8 task 8.2 — def-forms panel half (doc half in def_forms_doc.cpp).
//
// Render walks the doc's leaves in file order and draws one 8.1 typed
// widget per leaf; a commit goes through composite grammar (for kinds the
// 8.1 widgets don't cover) and then the injected GATE before anything is
// marked dirty. The panel never writes files and never touches cooked
// bytes (#16): the host writes the .toml (take_write) and the watch-cook
// loop hot-swaps the payload.
#include "ve/game/def_forms_panel.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "ve/game/attr_keys.h"
#include "ve/gfx/ui.h"

namespace ve::game {
namespace {

bool parse_bare_double(std::string_view tok, double& out) {
    const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), out);
    return res.ec == std::errc{} && res.ptr == tok.data() + tok.size() && std::isfinite(out);
}

}  // namespace

// ------------------------------------------------------------ states ----

edit_field& def_forms_panel::edit_state(const std::string& path) {
    const auto it = edits_.find(path);
    if (it != edits_.end()) return it->second;
    auto& st = edits_[path];
    prime_field(st, doc_.value(path));
    return st;
}

enum_field& def_forms_panel::enum_state(const std::string& path) {
    const auto it = enums_.find(path);
    if (it != enums_.end()) return it->second;
    auto& st = enums_[path];
    const def_field* f = doc_.field(path);
    prime_enum_field(st, f && !f->enum_set.empty() ? f->enum_set : "", doc_.value(path));
    return st;
}

picker_field& def_forms_panel::picker_state(const std::string& path) {
    const auto it = picks_.find(path);
    if (it != picks_.end()) return it->second;
    auto& st = picks_[path];
    st.committed = doc_.value(path);
    return st;
}

bool def_forms_panel::bool_state(const std::string& path) {
    const auto it = bools_.find(path);
    if (it != bools_.end()) return it->second;
    return bools_[path] = (doc_.value(path) == "true");
}

std::vector<std::string> def_forms_panel::candidates_for(const std::string& path,
                                                         const def_field& f) const {
    const auto it = cands_.find(path);
    if (it != cands_.end()) return it->second;
    // Derived defaults: name_ref ".target" fields address wires, and wire
    // ids live in the def itself. Everything else (socket refs, partner
    // paths) needs host knowledge -> empty until set_candidates().
    std::vector<std::string> out;
    if (f.kind == field_kind::name_ref && path.size() >= 7 &&
        path.compare(path.size() - 7, 7, ".target") == 0) {
        const auto wire_id_prefix = [](const std::string& s) {
            return s.size() > 5 && s.compare(0, 5, "wire.") == 0 &&
                   s.compare(s.size() - 3, 3, ".id") == 0;
        };
        for (const auto& p : doc_.paths())
            if (wire_id_prefix(p)) out.push_back(doc_.value(p));
    } else if (f.kind == field_kind::endpoint) {
        for (const auto& p : doc_.paths()) {
            if (p.size() > 8 && p.compare(0, 10, "component.") == 0 &&
                p.compare(p.size() - 3, 3, ".id") == 0)
                out.push_back("component." + doc_.value(p));
        }
    }
    return out;
}

bool def_forms_panel::composite_ok(const def_field& f, std::string_view text,
                                   std::string& msg) {
    switch (f.kind) {
    case field_kind::tag_list: {
        std::string_view v = text;
        int n = 0;
        while (!v.empty()) {
            const auto sp = v.find(' ');
            const auto tok = sp == std::string_view::npos ? v : v.substr(0, sp);
            if (!tok.empty()) {
                if (!is_valid_attr_key(tok)) {  // same dotted grammar as tags (#5.4)
                    msg = "tag '" + std::string(tok) + "' must be a.b_c (dotted, lowercase)";
                    return false;
                }
                ++n;
            }
            if (sp == std::string_view::npos) break;
            v = v.substr(sp + 1);
        }
        if (n == 0) { msg = "tag list needs at least one tag"; return false; }
        return true;
    }
    case field_kind::attr_map: {
        std::string_view v = text;
        int n = 0;
        while (!v.empty()) {
            const auto cm = v.find(',');
            const auto tok = cm == std::string_view::npos ? v : v.substr(0, cm);
            const auto eq = tok.find('=');
            if (eq == std::string_view::npos) { msg = "attr needs key=value"; return false; }
            const auto raw_key = tok.substr(0, eq);
            const auto val = tok.substr(eq + 1);
            // trim BOTH sides: comma-splitting leaves a leading space on every
            // pair after the first ("power.draw" -> " power.draw" fails grammar).
            const auto ks = raw_key.find_first_not_of(" \t");
            const auto key = ks == std::string_view::npos
                ? std::string_view{}
                : raw_key.substr(ks, raw_key.find_last_not_of(" \t") - ks + 1);
            if (!is_valid_attr_key(key)) {
                msg = "attr key '" + std::string(key) + "' not in registry grammar";
                return false;
            }
            double d = 0.0;
            const auto vs = val.find_first_not_of(" \t");
            const auto vt2 = vs == std::string_view::npos
                ? std::string_view{}
                : val.substr(vs, val.find_last_not_of(" \t") - vs + 1);
            bool ok_val = parse_bare_double(vt2, d) ||  // plain SI number
                          (vt2.size() >= 3 && vt2.front() == '"' && vt2.back() == '"');
            if (!ok_val) {  // NUMBER SUFFIX (gimbal_rad = "15 deg" class):
                const auto sp = vt2.rfind(' ');      // re-emit quotes it as needed
                if (sp != std::string_view::npos) {
                    double n2 = 0.0;
                    if (parse_bare_double(vt2.substr(0, sp), n2))
                        for (const auto& u : kUnits)
                            if (u.suffix == vt2.substr(sp + 1)) { ok_val = true; break; }
                }
            }
            if (!ok_val) { msg = "attr value '" + std::string(vt2) + "' must be a number or NUMBER SUFFIX"; return false; }
            ++n;
            if (cm == std::string_view::npos) break;
            v = v.substr(cm + 1);
        }
        if (n == 0) { msg = "attr map is empty"; return false; }
        return true;
    }
    case field_kind::int_boxes: {
        std::string_view v = text;
        int n = 0;
        while (!v.empty()) {
            const auto cm = v.find(',');
            const auto tok = cm == std::string_view::npos ? v : v.substr(0, cm);
            const auto t = tok.substr(tok.find_first_not_of(" \t"));
            const auto t2 = t.substr(0, t.find_last_not_of(" \t") + 1);
            long long b = 0;
            const auto res = std::from_chars(t2.data(), t2.data() + t2.size(), b);
            if (res.ec != std::errc{} || res.ptr != t2.data() + t2.size()) {
                msg = "cell bound '" + std::string(t2) + "' is not an integer";
                return false;
            }
            ++n;
            if (cm == std::string_view::npos) break;
            v = v.substr(cm + 1);
        }
        if (n == 0 || n % 6 != 0) { msg = "cells need whole boxes (6 bounds each)"; return false; }
        return true;
    }
    default:
        return true;
    }
}

// ------------------------------------------------------------ commit ----

bool def_forms_panel::try_commit(const std::string& path, const std::string& text) {
    blocked_ = false;
    const std::string old = doc_.value(path);
    if (text == old) {
        if (error_path_ == path) { gate_error_.clear(); error_path_.clear(); }
        return true;
    }
    if (!doc_.set_leaf(path, text)) return false;
    if (gate) {
        if (std::string msg = gate(doc_.source()); !msg.empty()) {
            doc_.set_leaf(path, old);  // #16: nothing half-baked reaches source
            gate_error_ = std::move(msg);
            error_path_ = path;
            blocked_ = true;
            return false;
        }
    }
    dirty_ = true;
    if (error_path_ == path) { gate_error_.clear(); error_path_.clear(); }
    return true;
}

std::string def_forms_panel::take_write() {
    dirty_ = false;
    return doc_.source();
}

// ------------------------------------------------------------ render ----

std::optional<gfx::rect> def_forms_panel::field_rect(std::string_view path) const {
    const auto it = rects_.find(std::string(path));
    if (it == rects_.end()) return std::nullopt;
    return it->second;
}

int def_forms_panel::leaf_index(std::string_view path) const {
    const auto& ps = doc_.paths();
    for (int i = 0; i < static_cast<int>(ps.size()); ++i)
        if (ps[i] == path) return i;
    return -1;
}

std::optional<gfx::rect> def_forms_panel::candidate_rect(std::string_view path,
                                                         std::size_t i) const {
    const auto it = cand_rects_.find(std::string(path));
    if (it == cand_rects_.end() || i >= it->second.size()) return std::nullopt;
    return it->second[i];
}

void def_forms_panel::render(gfx::ui_context& ui, const char* title,
                             std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                             int first_leaf, int leaf_count) {
    if (!ui.begin_window(title, x, y, w, h)) { ui.end_window(); return; }
    const auto& all_paths = doc_.paths();
    const int n = static_cast<int>(all_paths.size());
    const int last = leaf_count < 0 ? n : std::min(n, first_leaf + leaf_count);
    for (int li = first_leaf; li < last; ++li) {
        const std::string& path = all_paths[li];
        const def_field* f = doc_.field(path);
        if (!f) continue;
        const char* label = path.c_str();
        gfx::rect r{};
        bool committed = false;
        std::string new_text;

        switch (f->kind) {
        case field_kind::unit_str: {
            auto& st = edit_state(path);
            // unit_str fields always carry a schema dim; length is only a
            // compile-time fallback (every ratified schema sets dim).
            committed = draw_unit_field(ui, label, st, f->dim.value_or(unit_dim::length),
                                        &r) == field_edit::committed;
            new_text = st.committed;
            break;
        }
        case field_kind::number_si: {
            auto& st = edit_state(path);
            committed = draw_number_field(ui, label, st, &r) == field_edit::committed;
            new_text = st.committed;
            break;
        }
        case field_kind::integer: {
            auto& st = edit_state(path);
            committed = draw_integer_field(ui, label, st, &r) == field_edit::committed;
            new_text = st.committed;
            break;
        }
        case field_kind::tag: {
            auto& st = edit_state(path);
            committed = draw_tag_field(ui, label, st, &r) == field_edit::committed;
            new_text = st.committed;
            break;
        }
        case field_kind::text:
        case field_kind::id: {
            auto& st = edit_state(path);
            committed = draw_text_field(ui, label, st, &r) == field_edit::committed;
            new_text = st.committed;
            break;
        }
        case field_kind::tag_list:
        case field_kind::attr_map:
        case field_kind::int_boxes: {
            // Free-text with panel-side composite grammar (draw_text_field
            // only enforces non-empty).
            auto& st = edit_state(path);
            const auto e = draw_text_field(ui, label, st, &r);
            if (e == field_edit::committed) {
                std::string msg;
                if (!composite_ok(*f, st.committed, msg)) {
                    st.error = true;
                    st.message = std::move(msg);
                    ui.refocus_text(label);  // rejected draft stays editable
                } else {
                    committed = true;
                    new_text = st.committed;
                }
            }
            break;
        }
        case field_kind::enumeration: {
            auto& st = enum_state(path);
            if (draw_enum_field(ui, label, st, &r) == field_edit::committed) {
                if (try_commit(path, st.value)) {
                    // keep state in sync if the gate reverts
                } else {
                    st.value = doc_.value(path);
                }
            }
            break;
        }
        case field_kind::boolean: {
            if (!bools_.count(path)) bools_[path] = doc_.value(path) == "true";
            bool& v = bools_[path];
            if (draw_bool_field(ui, label, v, &r) == field_edit::committed) {
                if (!try_commit(path, v ? "true" : "false"))
                    v = doc_.value(path) == "true";  // revert on gate block
            }
            break;
        }
        case field_kind::endpoint:
        case field_kind::name_ref:
        case field_kind::def_path:
        case field_kind::path_list: {
            auto& st = picker_state(path);
            const auto cands = candidates_for(path, *f);
            std::vector<gfx::rect> cr;
            const auto pk = draw_picker_field(ui, label, st, cands, &r, &cr);
            if (!cr.empty()) cand_rects_[path] = std::move(cr);  // stale once closed
            if (pk == field_edit::committed) {
                if (!try_commit(path, st.committed))
                    st.committed = doc_.value(path);  // revert on gate block
            }
        } break;
        }
        if (r.w > 0) rects_[path] = r;

        // Text-widget commits share one tail: gate-blocked -> the doc (and
        // thus the write buffer) never saw the value; the field keeps the
        // rejected draft editable (#48) with the gate reason shown.
        if (committed && !try_commit(path, new_text)) {
            const auto it = edits_.find(path);
            if (it != edits_.end()) {
                it->second.committed = doc_.value(path);
                it->second.error = true;
                it->second.message = gate_error_;
                ui.refocus_text(label);
            }
        }

        // Gate rejections surface under the offending field (#48: easy to
        // fix — the value stays put, the reason shows in place).
        if (error_path_ == path && !gate_error_.empty()) {
            std::string line = "BLOCKED: " + gate_error_;
            ui.label(line.c_str());
        }
    }
    ui.end_window();
}

}  // namespace ve::game

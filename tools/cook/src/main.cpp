// ve_cook — the asset cooklet CLI.
//   ve_cook <assets_dir> <cooked_dir> [--watch] [--interval <ms>]
#include <atomic>
#include <cstdlib>
#include <string>

#include <ve/core/log.h>
#include <ve/cook/cook.h>

int main(int argc, char** argv) {
    ve::core::log_init(ve::core::LogLevel::info, nullptr);

    std::string src, out;
    bool watch = false;
    int interval_ms = 200;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--watch") {
            watch = true;
        } else if (a == "--interval" && i + 1 < argc) {
            interval_ms = std::atoi(argv[++i]);
        } else if (positional == 0) {
            src = a;
            ++positional;
        } else if (positional == 1) {
            out = a;
            ++positional;
        } else {
            VE_LOG_ERROR("usage: ve_cook <assets_dir> <cooked_dir> [--watch] [--interval ms]");
            return 2;
        }
    }
    if (positional < 2) {
        VE_LOG_ERROR("usage: ve_cook <assets_dir> <cooked_dir> [--watch] [--interval ms]");
        return 2;
    }

    if (!watch) {
        auto r = ve::cook::cook_incremental(src, out);
        VE_LOG_INFO("cook: %d cooked, %d skipped, %d failed (units: %zu)",
                    r.counts.cooked, r.counts.skipped, r.counts.failures,
                    r.unit_ids.size());
        if (!r.ok) {
            VE_LOG_ERROR("cook failed: %s", r.error.c_str());
            return 1;
        }
        return 0;
    }

    VE_LOG_INFO("cook: watching '%s' -> '%s' every %d ms", src.c_str(), out.c_str(), interval_ms);
    std::atomic<bool> stop{false};
    ve::cook::CookCounts totals;
    ve::cook::watch_loop(src, out, interval_ms, stop, &totals);
    return 0;
}

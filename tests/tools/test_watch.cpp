// test: watch_mode_fires — the watch cooklet re-cooks new/changed sources
// without being restarted (decision #16 hot-swap loop).
#include <doctest/doctest.h>

#include <unistd.h> // getpid

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <ve/cook/cook.h>

TEST_CASE("watch_mode_fires — new file picked up while watching") {
    static std::atomic<int> n{0};
    auto base = std::filesystem::temp_directory_path() /
                ("ve_watch_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    std::filesystem::remove_all(base);
    auto src = (base / "assets").string();
    auto out = (base / "cooked").string();
    std::filesystem::create_directories(src);

    auto write = [&](const std::string& rel, const std::string& body) {
        std::filesystem::path p(std::filesystem::path(src) / rel);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << body;
    };
    write("a.bin", "seed");

    // initial cook so the manifest exists
    REQUIRE(ve::cook::cook_incremental(src, out).ok);

    std::atomic<bool> stop{false};
    ve::cook::CookCounts totals;
    std::thread watcher([&] { ve::cook::watch_loop(src, out, 30, stop, &totals); });

    // mutate sources while the watcher runs
    write("b.bin", "added-while-watching");
    const std::string new_body = "changed-while-watching";
    write("a.bin", new_body);
    ve::cook::Bytes new_bytes(new_body.begin(), new_body.end());
    const auto new_sha = ve::cook::sha256(new_bytes);

    // poll the manifest until it carries the new content ID
    bool seen = false;
    for (int i = 0; i < 100 && !seen; ++i) {
        ve::cook::Manifest man;
        std::string err;
        if (ve::cook::read_manifest(out + "/manifest.bin", man, &err)) {
            seen = man.count("b.bin") == 1 && man.count("a.bin") == 1 &&
                   man["a.bin"].source_sha == new_sha;
        }
        if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    stop = true;
    watcher.join();

    CHECK(seen);
    CHECK(totals.cooked >= 2); // at least the add + the change

    std::filesystem::remove_all(base);
}

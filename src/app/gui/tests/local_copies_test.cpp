// local_copies -- what is opened from a network volume is worked on as a
// local mirror (LocalCopies.h): copied on open, written beside, sent back,
// remembered across sessions. A temp folder stands in for the volume.

#include "app/gui/LocalCopies.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <random>

namespace fs = std::filesystem;
using gui::LocalCopies;

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

LocalCopies::Result finish(LocalCopies& lc) {
    while (lc.copying()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto r = lc.take_result();
    return r ? *r : LocalCopies::Result{};
}

}  // namespace

int main() {
    const fs::path tmp = fs::weakly_canonical(fs::temp_directory_path()) /
                         ("local_copies_test_" + std::to_string(std::random_device{}()));
    fs::remove_all(tmp);
    const fs::path vol = tmp / "vol", disk = tmp / "disk";
    fs::create_directories(disk);
    for (int i = 0; i < 30; i++) put(vol / "proj/photos" / ("p" + std::to_string(i) + ".jpg"), "photo" + std::to_string(i));
    put(vol / "proj/photos/outputs/ckpt.bin", "checkpoint");
    put(vol / "proj/other/untouched.txt", "x");
    const std::string vs = vol.generic_string();
    auto network = [vs](const std::string& p) { return p.compare(0, vs.size(), vs) == 0; };

    LocalCopies lc;
    lc.on_network = network;
    lc.configure(true, disk.string());
    const std::string photos = (vol / "proj/photos").string();
    expect(lc.enabled() && lc.root() == (disk / "spirula-local").generic_string(), "the mirror's root");
    expect(lc.wants(photos) && !lc.wants((disk / "x").string()), "a volume path is wanted, a local one not");
    const std::string local = lc.local_for(photos);
    expect(local == lc.root() + vol.generic_string() + "/proj/photos", "mapped under the root");
    expect(lc.real_for(local + "/p1.jpg") == vs + "/proj/photos/p1.jpg", "and back");

    lc.start({photos}, /*skip_outputs=*/true);
    LocalCopies::Result r = finish(lc);
    expect(r.ok && r.paths.size() == 1 && r.paths[0] == local, "copied, to the mirror");
    expect(r.copied == 30 && get(fs::u8path(local) / "p7.jpg") == "photo7", "every photo, intact");
    expect(!fs::exists(fs::u8path(local) / "outputs"), "outputs/ left on the volume");
    expect(!lc.wants(local), "the copy itself is not copied again");

    // Work done locally: a photo changed, one deleted, a dataset made beside.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    put(fs::u8path(local) / "p3.jpg", "edited");
    fs::remove(fs::u8path(local) / "p4.jpg");
    const fs::path beside = fs::u8path(lc.local_for((vol / "proj/photos_dataset").string()));
    put(beside / "sparse/0/cameras.bin", "model");
    put(fs::u8path(lc.root()) / "stray/elsewhere.txt", "not ours");
    lc.sync(/*wait=*/true);
    const LocalCopies::SyncReport s = lc.last_sync();
    expect(s.error.empty() && s.copied == 2 && s.removed == 1, "sync: two copied, one removed");
    expect(get(vol / "proj/photos/p3.jpg") == "edited", "the edit reached the volume");
    expect(!fs::exists(vol / "proj/photos/p4.jpg"), "and the deletion");
    expect(get(vol / "proj/photos_dataset/sparse/0/cameras.bin") == "model",
           "a dataset made beside the photos went beside them on the volume");
    expect(!fs::exists("/stray/elsewhere.txt"), "nothing outside the copied places goes anywhere");
    expect(get(vol / "proj/photos/outputs/ckpt.bin") == "checkpoint", "outputs/ untouched");
    lc.sync(true);
    expect(lc.last_sync().copied == 0 && lc.last_sync().removed == 0, "a second sync has nothing to do");

    // Opened again: unsent work goes first, then nothing new is copied.
    put(fs::u8path(local) / "p5.jpg", "edited again");
    put(vol / "proj/photos/p_new.jpg", "added on the volume");
    lc.start({photos}, true);
    r = finish(lc);
    expect(r.ok && get(vol / "proj/photos/p5.jpg") == "edited again", "pending work sent before re-copying");
    expect(get(fs::u8path(local) / "p_new.jpg") == "added on the volume", "and the volume's news arrives");
    expect(r.copied == 1, "only that one file copied");

    // A big file goes in byte ranges, whole.
    {
        std::ofstream f(vol / "proj/big.ply", std::ios::binary);
        std::string block(1 << 20, '\0');
        for (int i = 0; i < 300; i++) {
            for (int j = 0; j < 4096; j++) block[(size_t)j * 256] = (char)(i + j);
            f.write(block.data(), (std::streamsize)block.size());
        }
    }
    lc.start({(vol / "proj/big.ply").string()}, true);
    r = finish(lc);
    const std::string big_local = lc.local_for((vol / "proj/big.ply").string());
    expect(r.ok && r.paths[0] == big_local && fs::file_size(big_local) == 300ull << 20 &&
               get(big_local) == get(vol / "proj/big.ply"),
           "a 300 MB file copied whole, through the range path");
    expect(!fs::exists(fs::u8path(lc.local_for((vol / "proj/other").string()))),
           "a file's siblings are not copied with it");

    // A second session finds the mirror and its record.
    LocalCopies again;
    again.on_network = network;
    again.configure(true, disk.string());
    expect(again.has_copies(), "the record survives the session");
    again.sync(true);
    expect(again.last_sync().copied == 0 && again.last_sync().removed == 0,
           "and knows nothing is pending");

    // Cancelled: an error, nothing opened.
    for (int i = 0; i < 200; i++) put(vol / "proj/many" / (std::to_string(i) + ".jpg"), "x");
    lc.start({(vol / "proj/many").string()}, true);
    lc.cancel();
    r = finish(lc);
    expect(!r.ok || r.copied == 200, "a cancel stops the copy (or it had already finished)");

    LocalCopies off;
    off.on_network = network;
    off.configure(false, disk.string());
    expect(!off.wants(photos), "off: nothing is copied");

    fs::remove_all(tmp);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}

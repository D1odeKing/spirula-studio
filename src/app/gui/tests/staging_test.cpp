// staging -- copying a dataset to a local disk and back (Staging.h): which
// mounts count as network ones, the parallel copy and its skips, a run's
// deletions carried back and nothing else, and paths in records rewritten.

#include "app/gui/Staging.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <random>

namespace fs = std::filesystem;
using namespace gui::staging;

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

void test_mounts() {
    const std::string mounts =
        "overlay / overlay rw 0 0\n"
        "/dev/sda1 /data ext4 rw 0 0\n"
        "geesefs /workspace fuse.geesefs rw 0 0\n"
        "server:/x /mnt/nfs nfs4 rw 0 0\n"
        "tmpfs /dev/shm tmpfs rw 0 0\n"
        "/dev/sdb1 /mnt/my\\040disk ext4 rw 0 0\n";
    expect(fs_type_in(mounts, "/workspace/datasets/a") == "fuse.geesefs", "the volume's type");
    expect(fs_type_in(mounts, "/workspaces/x") == "overlay", "a prefix is not a parent");
    expect(fs_type_in(mounts, "/data/x") == "ext4", "the local disk");
    expect(fs_type_in(mounts, "/mnt/my disk/a") == "ext4", "an escaped space in a mount");
    expect(is_network_fs("fuse.geesefs") && is_network_fs("nfs4") && is_network_fs("cifs") &&
               is_network_fs("fuse"),
           "network kinds");
    expect(!is_network_fs("ext4") && !is_network_fs("overlay") && !is_network_fs("tmpfs") &&
               !is_network_fs("fuse.lxcfs"),
           "local kinds");
}

void test_copy(const fs::path& tmp) {
    const fs::path vol = tmp / "vol", fast = tmp / "fast";
    for (int i = 0; i < 40; i++) put(vol / "images" / ("f" + std::to_string(i) + ".jpg"), "img" + std::to_string(i));
    put(vol / "record.json", "{\"dir\": \"" + vol.string() + "/images\"}");
    put(vol / "videos/a.insv", "video");
    put(vol / "outputs/model.ply", "trained");
    auto skip = [](const std::string& rel) {
        return rel == "videos" || rel.rfind("videos/", 0) == 0 || rel == "outputs" ||
               rel.rfind("outputs/", 0) == 0;
    };
    CopyResult in;
    std::string err;
    int calls = 0;
    expect(copy_tree(vol.string(), fast.string(), 8, skip, nullptr, true,
                     [&](const Progress& p) { calls++; (void)p; }, nullptr, in, err),
           "copy in");
    expect(in.copied == 41 && in.skipped == 0 && calls == 41, "every file once, progress for each");
    expect(!fs::exists(fast / "videos") && !fs::exists(fast / "outputs"), "skipped trees stay");
    expect(get(fast / "images/f7.jpg") == "img7", "contents");

    CopyResult again;
    copy_tree(vol.string(), fast.string(), 8, skip, nullptr, true, nullptr, nullptr, again, err);
    expect(again.copied == 0 && again.skipped == 41, "a second copy finds everything there");

    // A file the user deleted on the volume since goes from the copy too.
    fs::remove(vol / "images/f3.jpg");
    put(fast / "stray.tmp", "x");
    CopyResult pruned;
    copy_tree(vol.string(), fast.string(), 8, skip, nullptr, true, nullptr, nullptr, pruned, err);
    expect(!fs::exists(fast / "images/f3.jpg") && !fs::exists(fast / "stray.tmp") &&
               pruned.removed == 2,
           "prune makes the copy the volume's");

    // The run: rewrites one file, deletes one it staged in, adds a folder.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    put(fast / "images/f5.jpg", "redone");
    fs::remove(fast / "images/f6.jpg");
    put(fast / "sparse/0/cameras.bin", "model");
    put(fast / "record.json", "{\"dir\": \"" + fast.string() + "/images\"}");
    // Meanwhile something else wrote to the volume: not ours to delete.
    put(vol / "images/new_from_elsewhere.jpg", "keep");
    expect(rewrite_paths(fast.string(), fast.string(), vol.string()) == 1, "one record rewritten");
    CopyResult out;
    expect(copy_tree(fast.string(), vol.string(), 8, skip, &pruned.files, false, nullptr,
                     nullptr, out, err),
           "copy back");
    expect(get(vol / "images/f5.jpg") == "redone", "a changed file goes back");
    expect(get(vol / "sparse/0/cameras.bin") == "model", "a new folder goes back");
    expect(!fs::exists(vol / "images/f6.jpg") && out.removed == 1,
           "what the run deleted is deleted");
    expect(fs::exists(vol / "images/new_from_elsewhere.jpg"), "what it never saw stays");
    expect(get(vol / "record.json").find(fast.string()) == std::string::npos &&
               get(vol / "record.json").find(vol.string() + "/images") != std::string::npos,
           "the record names the volume");
    expect(get(vol / "videos/a.insv") == "video" && get(vol / "outputs/model.ply") == "trained",
           "inputs and outputs untouched");
    expect(out.copied == 3, "only what changed went back (" + std::to_string(out.copied) + ")");

    // Cancelled before it starts: nothing copied, false.
    std::atomic<bool> cancel{true};
    CopyResult c;
    expect(!copy_tree(vol.string(), (tmp / "other").string(), 4, skip, nullptr, true, nullptr,
                      &cancel, c, err) && c.copied == 0,
           "a cancelled copy stops");
    expect(tree_size(vol.string(), skip) > 0, "size of what would be staged");
}

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("staging_test_" + std::to_string(std::random_device{}()));
    fs::remove_all(tmp);
    test_mounts();
    test_copy(tmp);
    std::string why;
    expect(pick_fast_root((tmp / "missing").string(), 0, why).empty() && !why.empty(),
           "a missing local disk is refused, with why");
    fs::remove_all(tmp);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}

// dense_edit_test -- an edited dense cloud saved in place is re-signed, so the
// trainer's checksum accepts it, for both the published-generation layout and
// the older single-file one. The first edit keeps the original.

#include "dense/Generation.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace spirula::dense;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

void write_text(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A cloud and the manifest a dense run would have written for it.
void make_artifact(const fs::path& cloud, const fs::path& manifest, const std::string& body) {
    write_text(cloud, body);
    JsonWriter w;
    w.object().field("version", 2).field("complete", true).field("cloud", "roma.ply")
        .field("cloud_sha256", spirula::sha256_file(cloud.string()))
        .field("cloud_bytes", (long long)fs::file_size(cloud)).field("settings_kept", "yes");
    w.key("statistics").object().field("exported", 5).field("views", 7).end();
    w.end();
    write_text(manifest, w.str());
}

void edit(const std::string& dataset, const fs::path& saved_to, const std::string& body, int64_t points) {
    std::string found;
    ArtifactFiles files;
    check(edited_artifact(saved_to, found, files), "recognized as a dense cloud: " + saved_to.filename().string());
    const fs::path part = files.cloud.parent_path() / "roma-edit.part";
    write_text(part, body);
    commit_edit(found, files, part, points);
    (void)dataset;
}

void generation_layout(const fs::path& root) {
    const std::string dataset = (root / "gen").string();
    fs::create_directories(fs::path(dataset) / "work");
    make_artifact(fs::path(dataset) / "work" / "roma.ply", fs::path(dataset) / "work" / "manifest.json", "original cloud");
    const ArtifactFiles published = publish_generation(dataset, "run-1", fs::path(dataset) / "work" / "roma.ply",
                                                       fs::path(dataset) / "work" / "manifest.json");
    check(artifact_checksum_valid(published), "published generation is valid");

    edit(dataset, published.cloud, "cropped cloud", 3);
    ArtifactFiles now;
    try { now = artifact_files(dataset); } catch (const std::exception&) {}
    check(now.generation == "run-1", "the current record still verifies after the edit");
    check(artifact_checksum_valid(now), "the edited generation passes the checksum");
    check(read_text(now.cloud) == "cropped cloud", "the edit is the cloud");
    check(read_text(now.cloud.parent_path() / "roma.original.ply") == "original cloud", "the original is kept");
    check(read_text(fs::path(dataset) / "dense" / "roma.ply") == "cropped cloud", "the compatibility copy follows");
    check(artifact_checksum_valid({fs::path(dataset) / "dense" / "roma.ply", fs::path(dataset) / "dense" / "manifest.json", {}}),
          "the compatibility copy passes the checksum");
    bool seed_ok = true;
    try { verified_seed_path(dataset, now.cloud.string()); } catch (const std::exception&) { seed_ok = false; }
    check(seed_ok, "the trainer accepts it as a seed");
    const JsonValue m = json_parse_file(now.manifest.string());
    check(m.find("statistics")->get_double("exported", 0) == 3 && m.find("statistics")->get_double("views", 0) == 7,
          "statistics carry the new count and keep the rest");
    check(m.find("settings_kept") && m.find("edited")->find("original_sha256"), "other fields and the edit record kept");

    // A second edit, saved over the dense folder's own copy, lands in the generation.
    const std::string first_original = m.find("edited")->find("original_sha256")->as_string();
    edit(dataset, fs::path(dataset) / "dense" / "roma.ply", "cropped again", 2);
    now = artifact_files(dataset);
    check(artifact_checksum_valid(now) && read_text(now.cloud) == "cropped again", "a second edit through the copy");
    check(json_parse_file(now.manifest.string()).find("edited")->find("original_sha256")->as_string() == first_original,
          "the original's checksum is remembered across edits");
    check(read_text(now.cloud.parent_path() / "roma.original.ply") == "original cloud", "the original is not replaced");
}

// The state an older build left: the generation's cloud overwritten unsigned,
// the dense folder's copy still the original. Saving again must keep that copy.
void unsigned_generation_edit(const fs::path& root) {
    const std::string dataset = (root / "unsigned").string();
    fs::create_directories(fs::path(dataset) / "work");
    make_artifact(fs::path(dataset) / "work" / "roma.ply", fs::path(dataset) / "work" / "manifest.json", "first original");
    const ArtifactFiles published = publish_generation(dataset, "run-2", fs::path(dataset) / "work" / "roma.ply",
                                                       fs::path(dataset) / "work" / "manifest.json");
    write_text(published.cloud, "cropped by an old build");
    bool refused = false;
    try { verified_seed_path(dataset, published.cloud.string()); } catch (const std::exception&) { refused = true; }
    check(refused, "the unsigned edit is refused, as reported");
    edit(dataset, published.cloud, "cropped by an old build", 6);
    const ArtifactFiles now = artifact_files(dataset);
    check(artifact_checksum_valid(now) && read_text(now.cloud) == "cropped by an old build", "saving again repairs it");
    check(read_text(now.cloud.parent_path() / "roma.original.ply") == "first original",
          "the original is rescued from the dense folder's copy");
    check(read_text(fs::path(dataset) / "dense" / "roma.ply") == "cropped by an old build", "the copy then follows the edit");
}

void legacy_layout(const fs::path& root) {
    const std::string dataset = (root / "legacy").string();
    const fs::path dense = fs::path(dataset) / "dense";
    fs::create_directories(dense);
    make_artifact(dense / "roma.ply", dense / "manifest.json", "legacy original");
    // Overwritten outside this path first, as an older build would have: no original to keep.
    write_text(dense / "roma.ply", "edited before the fix");
    check(!artifact_checksum_valid(artifact_files(dataset)), "an unsigned edit fails the checksum");
    edit(dataset, dense / "roma.ply", "edited before the fix", 4);
    check(artifact_checksum_valid(artifact_files(dataset)), "saving it again repairs the checksum");
    check(!fs::exists(dense / "roma.original.ply"), "no stale original is invented");
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "spirula-dense-edit-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    try {
        generation_layout(root);
        unsigned_generation_edit(root);
        legacy_layout(root);
    } catch (const std::exception& e) {
        check(false, std::string("threw: ") + e.what());
    }
    std::string found;
    ArtifactFiles files;
    check(!edited_artifact(root / "elsewhere" / "cloud.ply", found, files), "other files are left alone");
    const fs::path ds = root / "set";
    check(fs::equivalent(dense_dataset_of(ds / "dense" / "roma.ply"), ds, ec) || dense_dataset_of(ds / "dense" / "roma.ply") ==
              fs::absolute(ds).lexically_normal().string(), "the dense folder's cloud names its dataset");
    check(dense_dataset_of(ds / "dense" / "generations" / "run-1" / "roma.ply") == fs::absolute(ds).lexically_normal().string(),
          "a generation's cloud names its dataset");
    check(dense_dataset_of(ds / "dense" / "generations" / "run-1" / "other.ply").empty() &&
              dense_dataset_of(ds / "roma.ply").empty() && dense_dataset_of(ds / "dense" / "generations" / ".." / "roma.ply") ==
              fs::absolute(ds).lexically_normal().string(), "other files name no dataset");
    fs::remove_all(root, ec);
    std::printf("%s\n", g_failures ? "dense_edit_test: FAILED" : "dense_edit_test: OK");
    return g_failures ? 1 : 0;
}

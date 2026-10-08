#include "app/gui/ModelCache.h"
#include "core/Sha256.h"
#include "roma/model/Fetch.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Fixture {
    fs::path root = fs::temp_directory_path() /
        ("spirula-download-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root,error); }
};

void wait(gui::FileDownload& download) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (download.state() == gui::FileDownload::State::Running) {
        require(std::chrono::steady_clock::now() < deadline,"download fixture timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::string file_url(const fs::path& path) {
    const auto source = fs::absolute(path).generic_string();
    std::string encoded;
    for (const unsigned char c : source) {
        if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.') encoded += (char)c;
        else { char escape[4]; std::snprintf(escape,sizeof escape,"%%%02X",c); encoded += escape; }
    }
    return "file://" + std::string(source.front() == '/' ? "" : "/") + encoded;
}

gui::PendingDownload verified(std::string url, std::string dest, uint64_t bytes, std::string sha256) {
    gui::PendingDownload download{std::move(url),std::move(dest),bytes};
    download.sha256 = std::move(sha256);
    return download;
}
}

int main(int argc, char** argv) {
    try {
        Fixture fixture;
        const auto source = fixture.root / "source.bin", destination = fixture.root / "verified.bin";
        { std::ofstream output(source,std::ios::binary); output << "verified fixture bytes"; }
        const auto digest = spirula::sha256_file(source.string());
        const auto& entry = gui::dense_model_entry();
        require(std::string(entry.id) == "romav2.0.1" && std::string(entry.family) == "roma" &&
                entry.bytes == spirula::roma::kOfficialCheckpoint.bytes &&
                std::string(entry.sha256) == spirula::roma::kOfficialCheckpoint.sha256 &&
                std::string(entry.url) == spirula::roma::kOfficialCheckpoint.url,"official download identity changed");
        gui::FileDownload download;
        download.start(verified(file_url(source),destination.string(),fs::file_size(source),digest));
        wait(download);
        require(download.state() == gui::FileDownload::State::Done && download.path() == destination.string() &&
                spirula::sha256_file(destination.string()) == digest,"valid download was not verified and published");
        download.start(verified(file_url(source),destination.string(),fs::file_size(source),std::string(64,'0')));
        wait(download);
        require(download.state() == gui::FileDownload::State::Failed &&
                spirula::sha256_file(destination.string()) == digest && !fs::exists(destination.string() + ".part"),
                "checksum failure replaced a verified file or retained corrupt partial bytes");
        download.start(verified(file_url(source),destination.string(),fs::file_size(source),digest));
        download.cancel(); wait(download);
        require(download.state() == gui::FileDownload::State::Cancelled &&
                spirula::sha256_file(destination.string()) == digest,"cancelled download replaced the previous verified file");
        if (argc > 1) {
            const fs::path checkpoint(argv[1]); const auto output = fixture.root / entry.file;
            require(fs::file_size(checkpoint) == entry.bytes && spirula::sha256_file(checkpoint.string()) == entry.sha256,
                    "local official checkpoint does not match pinned bytes");
            download.start(verified(file_url(checkpoint),output.string(),entry.bytes,entry.sha256)); wait(download);
            require(download.state() == gui::FileDownload::State::Done && spirula::sha256_file(output.string()) == entry.sha256,
                    "official checkpoint bytes did not pass the shared download verifier");
        }
        std::printf("PASS official download identity, checksum success/failure, cancellation and previous-file preservation%s\n",
                    argc > 1 ? ", including complete official checkpoint bytes" : "");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}

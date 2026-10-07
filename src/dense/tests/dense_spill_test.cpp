#include "dense/ReferenceSampling.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace spirula::dense;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    fs::path root = fs::temp_directory_path() /
        ("spirula-spill-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root,error); }
};
}

int main() {
    try {
        Fixture fixture;
        const auto spill = fixture.root / "array.bin";
        {
            DiskArray<uint64_t> array(spill,3 * sizeof(uint64_t));
            for (uint64_t i = 0; i < 71; ++i) array.push_back(i * i);
            require(fs::exists(spill),"array did not spill under its memory budget");
            uint64_t i = 0;
            for (auto value : array) { require(value == i * i,"spilled array changed record order"); ++i; }
            require(array[11] == 121 && array[70] == 4900,"spilled random access was incorrect");
            array.set(11,999); require(array[11] == 999,"spilled update was incorrect");
            array.resize(13); require(array.size() == 13 && array[12] == 144,"spilled truncation changed retained records");
            array.push_back(7); require(array[13] == 7,"append after spilled truncation used the old file size");
            array.sort(std::less<uint64_t>{},16);
            for (uint64_t j = 1; j < array.size(); ++j) require(array[j - 1] <= array[j],"in-place external array sort changed ordering");
            array.clear(); require(array.empty() && !fs::exists(spill),"clear retained a spill file");
            array.push_back(9); require(array.front() == 9,"cleared spill array could not be reused");
        }
        require(!fs::exists(spill),"array destructor retained a temporary spill file");
        {
            DiskArray<uint64_t> array(spill,sizeof(uint64_t));
            array.push_back(2); array.push_back(1);
            bool stopped = false;
            try { array.sort(std::less<uint64_t>{},16,[] { throw std::runtime_error("cancelled"); }); }
            catch (const std::exception&) { stopped = true; }
            require(stopped && fs::exists(spill),"cancelled spill sort did not reach its closed-file path");
        }
        require(!fs::exists(spill),"cancelled spill sort retained its closed temporary file");
        struct Item { int key, ordinal; };
        const auto source = fixture.root / "unsorted.bin", ordered = fixture.root / "ordered.bin";
        {
            std::ofstream output(source,std::ios::binary);
            for (int i = 0; i < 191; ++i) write_disk_record(output,Item{i % 7,i});
        }
        external_sort<Item>(source,ordered,8 * sizeof(Item),[](auto a,auto b) { return a.key < b.key; });
        {
            std::ifstream input(ordered,std::ios::binary); Item item; int key = -1, ordinal = -1, count = 0;
            while (read_disk_record(input,item)) {
                require(item.key >= key && (item.key != key || item.ordinal > ordinal),"external sort was not stable across runs");
                key = item.key; ordinal = item.ordinal; ++count;
            }
            require(count == 191,"external sort lost records");
        }
        for (uint64_t count : {1ull,17ull,200ull,1200ull}) {
            ReferenceSampling memory(32,24,count,921);
            ExternalReferenceSampling disk(fixture.root / ("sample-" + std::to_string(count)),32,24,count,921,2048);
            for (uint32_t pixel = 0; pixel < 32 * 24; ++pixel) {
                const float confidence = pixel % 19 ? (float)((pixel % 11) + 1) / 11 : 0;
                memory.add(pixel,confidence); disk.add(pixel,confidence);
            }
            const auto expected = memory.selected();
            std::ifstream input(disk.selected(),std::ios::binary); std::set<uint32_t> actual; uint32_t pixel;
            while (read_disk_record(input,pixel)) actual.insert(pixel);
            require(actual == expected,"external coverage/weighted sampling changed the selected pixels");
        }
        bool cancelled = false;
        try { external_sort<Item>(source,fixture.root / "cancelled.bin",1024,[](auto a,auto b) { return a.key < b.key; },
                                  [] { throw std::runtime_error("cancelled"); }); }
        catch (const std::exception&) { cancelled = true; }
        require(cancelled,"external sort ignored cancellation");
        std::printf("PASS bounded spill arrays, stable external merging, deterministic coverage/weighted sampling and cancellation\n");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}

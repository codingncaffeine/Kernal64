// Makes a ready CMD HD image by running the drive's own programs (hdprep.h).
#include "hdprep.h"

#include "c64.h"
#include "hdprep/programs.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace k64 {

namespace {

// The programs the set-up needs, by the SHA-1 of their file (load address included). The
// names they are loaded by are the disk's own (the programs load each other by name).
enum Need { kInstall, kRewrite, kHeader, kDos, kGeos, kNeedCount };
constexpr const char* kInstallSha1 = "ed0a3b1ac48c6dc222274608efc24db7314015b2";   // the install program
constexpr const char* kRewriteSha1 = "39728ec453fe82f69589bc63b4bb9f95dda55654";   // the DOS writer it chains to
constexpr const char* kGeosSha1 = "20382b521e1089fa01c910d73ca5baa1b473c185";      // the GEOS driver 2.00
// A system header and the DOS it checks (the writer refuses any other pair), newest first.
struct DosPair { const char* header; const char* dos; const char* version; };
constexpr DosPair kDosPairs[] = {
    {"8c5674f40f8fe41b1c004eae62eab5006e170007", "7956f48c2b3ce02e2caf1407917c556a018d3418", "1.92"},
    {"3ec54d19ec8134cb4765958d76ed679dbb80b1fc", "45ffefc0d668893640194bb3c0ccb9c05bee830e", "1.82"},
};

struct DiskFile { std::string name; std::vector<uint8_t> data; };

// The program files of a disk image: a D64 (1541: 35 or 40 tracks, with or without error
// bytes; directory from 18/1) or a D81 (1581: 80 tracks of 40 sectors; directory from 40/3).
bool readImage(const std::vector<uint8_t>& d, std::vector<DiskFile>& out) {
    const bool d81 = d.size() == 819200 || d.size() == 822400;
    const bool d64 = d.size() == 174848 || d.size() == 175531 || d.size() == 196608 || d.size() == 197376;
    if (!d81 && !d64) return false;
    const unsigned tracks = d81 ? 80 : (d.size() >= 196608 ? 40 : 35);
    auto spt = [d81](unsigned t) -> unsigned { return d81 ? 40 : t <= 17 ? 21 : t <= 24 ? 19 : t <= 30 ? 18 : 17; };
    auto sec = [&](unsigned t, unsigned s) -> const uint8_t* {
        if (t < 1 || t > tracks || s >= spt(t)) return nullptr;
        size_t off = 0;
        for (unsigned x = 1; x < t; ++x) off += spt(x);
        return d.data() + (off + s) * 256;
    };
    unsigned t = d81 ? 40 : 18, s = d81 ? 3 : 1, guard = 0;
    while (t && ++guard < 400) {
        const uint8_t* b = sec(t, s);
        if (!b) return false;
        for (int i = 0; i < 8; ++i) {
            const uint8_t* e = b + i * 32;
            if ((e[2] & 7) != 2) continue;   // programs only
            DiskFile f;
            for (int k = 5; k < 21 && e[k] != 0xA0; ++k) f.name += static_cast<char>(e[k]);
            unsigned ft = e[3], fs = e[4], g = 0;
            while (ft && ++g < 3200) {
                const uint8_t* x = sec(ft, fs);
                if (!x) return false;
                const unsigned n = x[0] ? 254 : (x[1] >= 1 ? x[1] - 1u : 0u);
                f.data.insert(f.data.end(), x + 2, x + 2 + n);
                ft = x[0];
                fs = x[1];
            }
            out.push_back(std::move(f));
        }
        t = b[0];
        s = b[1];
    }
    return true;
}

// Finds the five programs on one image; the DOS is the newest whose header is there too.
bool pickNeeded(const std::vector<uint8_t>& image, DiskFile (&need)[kNeedCount], std::string* version = nullptr) {
    std::vector<DiskFile> files;
    if (!readImage(image, files)) return false;
    std::map<std::string, const DiskFile*> byHash;
    for (const auto& f : files) byHash.emplace(sha1Hex(f.data.data(), f.data.size()), &f);
    auto has = [&](const char* h) { return byHash.count(h) ? byHash[h] : nullptr; };
    if (!has(kInstallSha1) || !has(kRewriteSha1) || !has(kGeosSha1)) return false;
    for (const DosPair& p : kDosPairs) {
        if (!has(p.header) || !has(p.dos)) continue;
        need[kInstall] = *has(kInstallSha1);
        need[kRewrite] = *has(kRewriteSha1);
        need[kGeos] = *has(kGeosSha1);
        need[kHeader] = *has(p.header);
        need[kDos] = *has(p.dos);
        if (version) *version = p.version;
        return true;
    }
    return false;
}

// The folder drive's disk: the programs, in memory.
class MemStore : public FolderStore {
public:
    void add(const std::string& name, std::vector<uint8_t> data) { files_[name] = std::move(data); }
    std::vector<Entry> list() override {
        std::vector<Entry> out;
        for (auto& [n, d] : files_) out.push_back({n, false, d.size()});
        return out;
    }
    bool read(const std::string& name, bool seq, std::vector<uint8_t>& out) override {
        auto it = files_.find(name);
        if (seq || it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool write(const std::string& name, bool seq, const std::vector<uint8_t>& data) override {
        if (seq) return false;
        files_[name] = data;
        return true;
    }
    bool remove(const std::string& name) override { return files_.erase(name) > 0; }
    bool rename(const std::string&, const std::string&) override { return false; }
    std::string label() override { return "K64 SETUP"; }
private:
    std::map<std::string, std::vector<uint8_t>> files_;
};

// Runs the machine a frame at a time and reads its text screen.
class Driver {
public:
    explicit Driver(C64& m) : m_(m) {}
    std::string screen() const {
        std::string s;
        for (int i = 0; i < 1000; ++i) {
            uint8_t c = m_.ram()[0x0400 + i] & 0x7F;
            s += c < 0x20 ? static_cast<char>(c + 0x40) : c < 0x40 ? static_cast<char>(c) : '?';
            if (i % 40 == 39) s += '\n';
        }
        return s;
    }
    // The screen line holding TEXT (trimmed), for messages.
    std::string lineWith(const std::string& text) const {
        const std::string s = screen();
        const size_t p = s.find(text);
        if (p == std::string::npos) return {};
        const size_t a = s.rfind('\n', p), b = s.find('\n', p);
        std::string l = s.substr(a == std::string::npos ? 0 : a + 1, (b == std::string::npos ? s.size() : b) - (a == std::string::npos ? 0 : a + 1));
        while (!l.empty() && l.back() == ' ') l.pop_back();
        return l;
    }
    int count(const std::string& text) const {
        const std::string s = screen();
        int n = 0;
        for (size_t p = s.find(text); p != std::string::npos; p = s.find(text, p + 1)) ++n;
        return n;
    }
    // Runs until pred() or `seconds` of machine time pass; false on time-out or cancel.
    template <class Pred>
    bool runUntil(Pred pred, double seconds) {
        const int frames = static_cast<int>(seconds * 50.0);
        for (int f = 0; f < frames; ++f) {
            if (pred()) return true;
            m_.runFrame();
            if (cancel_ || (f % 50 == 0 && !tick_())) { cancel_ = true; return false; }
        }
        return pred();
    }
    bool runFor(double seconds) { return runUntil([] { return false; }, seconds) || !cancel_; }
    // Types PETSCII text through the keyboard buffer, ten keys at a time as it empties.
    bool type(const std::string& text) {
        for (size_t at = 0; at < text.size();) {
            if (!runUntil([this] { return m_.ram()[0x00C6] == 0; }, 10)) return false;
            const size_t n = std::min<size_t>(10, text.size() - at);
            for (size_t k = 0; k < n; ++k) m_.pokeRam(static_cast<uint16_t>(0x0277 + k), static_cast<uint8_t>(text[at + k]));
            m_.pokeRam(0x00C6, static_cast<uint8_t>(n));
            at += n;
            m_.runFrame();
        }
        return true;
    }
    std::function<bool()> tick_ = [] { return true; };
    bool cancel_ = false;
private:
    C64& m_;
};

} // namespace

bool findUtilitiesDisk(const std::string& dir, std::vector<uint8_t>& image, std::string& from) {
    namespace fs = std::filesystem;
    int best = -1;   // index into kDosPairs of the best image so far (lower = newer DOS)
    for (const fs::path& d : {fs::path(dir), fs::path(dir) / "kernal64"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const auto size = e.file_size(ec);
            if (size < 174848 || size > 822400) continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
            DiskFile need[kNeedCount];
            std::string version;
            if (!pickNeeded(bytes, need, &version)) continue;
            int rank = 0;
            while (kDosPairs[rank].version != version) ++rank;
            if (best >= 0 && rank >= best) continue;
            best = rank;
            image = std::move(bytes);
            from = e.path().string();
        }
    }
    return best >= 0;
}

bool makeHardDrive(const std::string& path, uint64_t bytes, const RomSet& roms, const std::vector<uint8_t>& bootRom,
                   const std::vector<uint8_t>& d81, const std::function<bool(int)>& progress, std::string& error,
                   int* partitionsMade) {
    namespace fs = std::filesystem;
    error.clear();
    if (bytes < kHardDriveMinBytes || bytes > kHardDriveMaxBytes) { error = "the size must be from 20 MB to 1 GB"; return false; }
    bytes &= ~uint64_t(511);
    DiskFile need[kNeedCount];
    if (!pickNeeded(d81, need)) { error = "the utilities disk does not hold the programs that set a drive up"; return false; }
    if (!roms.has(RomRole::Basic) || !roms.has(RomRole::Kernal) || !roms.has(RomRole::Chargen)) {
        error = "the C64's BASIC, KERNAL and character ROMs are needed";
        return false;
    }
    std::error_code ec;
    if (fs::exists(path, ec)) { error = "the file exists already"; return false; }
    {   // a blank, sparse image of the size asked for
        std::ofstream f(path, std::ios::binary);
        if (!f) { error = "cannot create the file"; return false; }
    }
    fs::resize_file(path, bytes, ec);
    if (ec) { fs::remove(path, ec); error = "cannot size the file"; return false; }
    auto fail = [&](const std::string& why) {
        error = why;
        std::error_code e2;
        fs::remove(path, e2);
        return false;
    };

    BlockFile disk;
    if (!disk.open(path, 0)) return fail("cannot open the file");
    auto m = std::make_unique<C64>();
    m->setRoms(roms);
    m->setDriveWanted(false);
    MemStore store;
    for (auto& f : need) store.add(f.name, f.data);
    store.add("K64PART", std::vector<uint8_t>(std::begin(kPartitionPrg), std::end(kPartitionPrg)));
    store.add("K64FORMAT", std::vector<uint8_t>(std::begin(kFormatPrg), std::end(kFormatPrg)));
    m->setFolderDrive(&store, 10);
    CmdHd& hd = m->hardDrive();
    if (!hd.setRom(bootRom.data(), bootRom.size())) return fail("not the CMD HD boot ROM");
    hd.attachDisk(&disk);
    hd.scsi().trace = std::getenv("K64_SCSI_TRACE") != nullptr;
    hd.setTime({26, 1, 1, 4, 12, 0, 0});
    m->setHardDriveOn(true);
    m->reset();

    Driver d(*m);
    int pct = 0;
    auto at = [&](int p) {
        pct = p;
        if (progress && !progress(p)) d.cancel_ = true;
        return !d.cancel_;
    };
    d.tick_ = [&] { return !progress || progress(pct); };
    auto stopped = [&](const char* what) { return fail(d.cancel_ ? "cancelled" : what); };

    // 1. Power on: the drive searches the blank disk and waits in install mode ($CED7).
    hd.countPc = 0xCED7;
    if (!at(1) || !d.runUntil([&] { return hd.countHits > 0; }, 7200)) return stopped("the drive did not start");
    hd.countPc = 0;
    if (!at(20) || !d.runUntil([&] { return d.count("READY.") > 0; }, 10)) return stopped("the computer did not start");
    // 2. The install program, from the folder drive (device 10).
    int ready = d.count("READY.");
    if (!d.type("LOAD\"" + need[kInstall].name + "\",10\r") ||
        !d.runUntil([&] { return d.count("READY.") > ready; }, 120)) return stopped("the install program did not load");
    if (!d.type("RUN\r") || !d.runUntil([&] { return d.count("PRESS <RETURN> TO CREATE") > 0; }, 60))
        return stopped("the install program did not start");
    if (!d.type("\r") || !d.runUntil([&] { return d.count("RETURN TO ACCEPT") > 0; }, 120))
        return stopped("the install program did not read the drive's size");
    if (!d.type("\r") || !d.runUntil([&] { return d.count("CLEAR AREA BELOW SYSTEM?") > 0; }, 60))
        return stopped("the install program stopped");
    if (!at(30) || !d.type("N")) return stopped("cancelled");
    if (!d.runUntil([&] { return d.count("REBOOT DOS") > 0 || d.count("ERROR") > 0; }, 1800))
        return stopped("the install did not finish");
    if (d.count("ERROR") > 0) return fail("the install program reported: " + d.lineWith("ERROR"));
    if (!at(60) || !d.runUntil([&] { return d.count("READY.") > 0; }, 10)) return stopped("the install did not end");
    // 3. The partition table: the size in blocks at 828-830 for the program.
    // The DOS reads and writes with the 6-byte SCSI commands: a 21-bit block number, so the first
    // 2^21 blocks (1 GB) are all it can reach on one disk (a higher block's top bit lands in the
    // LUN field: measured on a 2 GB image, partition 66 on). Partitions end there.
    const uint32_t blocks = std::min<uint32_t>(disk.blocks(), 1u << 21);
    m->pokeRam(828, static_cast<uint8_t>(blocks));
    m->pokeRam(829, static_cast<uint8_t>(blocks >> 8));
    m->pokeRam(830, static_cast<uint8_t>(blocks >> 16));
    ready = d.count("READY.");
    if (!d.type("LOAD\"K64PART\",10\r") || !d.runUntil([&] { return d.count("READY.") > ready; }, 60))
        return stopped("the partition program did not load");
    if (!d.type("RUN\r") || !d.runUntil([&] { return d.count("PARTITIONS") > 0; }, 600))
        return stopped("the partition table was not written");
    const int partitions = m->ram()[831];
    if (partitions < 1) return fail("no partition fits on the drive");
    // 4. Reset: the DOS boots from the disk as device 12; every partition is formatted.
    if (!at(70)) return stopped("cancelled");
    hd.pressReset();
    if (!d.runFor(8)) return stopped("cancelled");
    ready = d.count("READY.");
    if (!d.type("LOAD\"K64FORMAT\",10\r") || !d.runUntil([&] { return d.count("READY.") > ready; }, 60))
        return stopped("the format program did not load");
    if (!d.type("RUN\r") || !d.runUntil([&] { return d.count("FORMATTED") > 0 || d.count("ERROR") > 0; }, 60.0 + 30.0 * partitions))
        return stopped("the partitions were not formatted");
    if (d.count("ERROR") > 0) return fail("formatting a partition failed: " + d.lineWith("ERROR"));
    if (!at(100)) return stopped("cancelled");
    if (partitionsMade) *partitionsMade = partitions;
    return true;
}

} // namespace k64

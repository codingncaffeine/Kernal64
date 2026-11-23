// cputest: runs the per-opcode single-instruction suite (SingleStepTests 65x02,
// 6502/v1) against the CPU core, comparing every bus cycle and the final state.
//
//   cputest DIR [OPCODE_HEX...] [--max-fail N] [--quiet]
//
// Memory is 64 KB of plain RAM; an access to an address the test does not list is
// a failure (the suite's own rule). Exit code 0 only if every case passed.
#include "cpu6502.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- a small JSON reader, enough for this suite --------------------------------
struct Json {
    enum T { Null, Num, Str, Arr, Obj } t = Null;
    double num = 0;
    std::string str;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json& operator[](const char* k) const {
        for (auto& f : fields) if (f.first == k) return f.second;
        static const Json none;
        return none;
    }
    const Json& at(size_t i) const { return items[i]; }
    int i() const { return static_cast<int>(num); }
};

struct Parser {
    const char* p;
    const char* end;
    void ws() { while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p; }
    [[noreturn]] void bad(const char* what) {
        std::fprintf(stderr, "json: %s near '%.20s'\n", what, p);
        std::exit(3);
    }
    Json value() {
        ws();
        if (p >= end) bad("unexpected end");
        Json j;
        if (*p == '{') {
            j.t = Json::Obj; ++p; ws();
            if (*p == '}') { ++p; return j; }
            for (;;) {
                ws(); std::string k = string(); ws();
                if (*p++ != ':') bad("expected :");
                j.fields.emplace_back(std::move(k), value());
                ws();
                if (*p == ',') { ++p; continue; }
                if (*p++ == '}') return j;
                bad("expected , or }");
            }
        }
        if (*p == '[') {
            j.t = Json::Arr; ++p; ws();
            if (*p == ']') { ++p; return j; }
            for (;;) {
                j.items.push_back(value());
                ws();
                if (*p == ',') { ++p; continue; }
                if (*p++ == ']') return j;
                bad("expected , or ]");
            }
        }
        if (*p == '"') { j.t = Json::Str; j.str = string(); return j; }
        if (*p == '-' || std::isdigit(static_cast<unsigned char>(*p))) {
            char* e = nullptr;
            j.t = Json::Num; j.num = std::strtod(p, &e); p = e; return j;
        }
        if (!std::strncmp(p, "null", 4)) { p += 4; return j; }
        bad("unexpected character");
    }
    std::string string() {
        if (*p++ != '"') bad("expected string");
        std::string s;
        while (p < end && *p != '"') { if (*p == '\\') ++p; s += *p++; }
        ++p;
        return s;
    }
};

// ---- the bus ----------------------------------------------------------------
struct Access { uint16_t addr; uint8_t value; bool write; };

struct TestBus {
    uint8_t mem[65536];
    bool listed[65536];
    std::vector<Access> log;
    bool strayAccess = false;
    uint16_t stray = 0;
    uint8_t read(uint16_t a) {
        if (!listed[a] && !strayAccess) { strayAccess = true; stray = a; }
        log.push_back({a, mem[a], false});
        return mem[a];
    }
    void write(uint16_t a, uint8_t v) {
        if (!listed[a] && !strayAccess) { strayAccess = true; stray = a; }
        mem[a] = v;
        log.push_back({a, v, true});
    }
};

std::string flags(uint8_t p) {
    std::string s = "NV-BDIZC";
    for (int b = 0; b < 8; ++b) if (!(p & (0x80 >> b))) s[b] = static_cast<char>(std::tolower(s[b]));
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cputest DIR [OPCODE_HEX...] [--max-fail N] [--quiet]\n"); return 2; }
    std::string dir = argv[1];
    std::vector<int> ops;
    int maxFail = 5;
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--max-fail") && i + 1 < argc) maxFail = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else ops.push_back(static_cast<int>(std::strtol(argv[i], nullptr, 16)));
    }
    if (ops.empty()) for (int o = 0; o < 256; ++o) ops.push_back(o);

    static TestBus bus;
    k64::Cpu6502<TestBus> cpu(bus);
    long total = 0, failed = 0;
    int opsFailed = 0;
    for (int op : ops) {
        char name[16];
        std::snprintf(name, sizeof name, "%02x.json", op);
        std::ifstream f(dir + "/" + name, std::ios::binary);
        if (!f) { std::fprintf(stderr, "missing %s/%s\n", dir.c_str(), name); return 3; }
        std::stringstream ss; ss << f.rdbuf();
        const std::string text = ss.str();
        Parser parser{text.data(), text.data() + text.size()};
        const Json tests = parser.value();

        long opFail = 0;
        for (const Json& t : tests.items) {
            ++total;
            const Json& in = t["initial"];
            const Json& out = t["final"];
            const Json& cycles = t["cycles"];
            std::memset(bus.mem, 0, sizeof bus.mem);
            std::memset(bus.listed, 0, sizeof bus.listed);
            for (const Json& kv : in["ram"].items) { bus.mem[kv.at(0).i()] = static_cast<uint8_t>(kv.at(1).i()); bus.listed[kv.at(0).i()] = true; }
            for (const Json& kv : out["ram"].items) bus.listed[kv.at(0).i()] = true;
            bus.log.clear();
            bus.strayAccess = false;

            auto& st = cpu.state();
            st = {};
            st.r.pc = static_cast<uint16_t>(in["pc"].i());
            st.r.s = static_cast<uint8_t>(in["s"].i());
            st.r.a = static_cast<uint8_t>(in["a"].i());
            st.r.x = static_cast<uint8_t>(in["x"].i());
            st.r.y = static_cast<uint8_t>(in["y"].i());
            st.r.p = static_cast<uint8_t>(in["p"].i());

            for (size_t c = 0; c < cycles.items.size(); ++c) cpu.tick();

            std::string why;
            char buf[256];
            const bool jam = cpu.jammed();
            if (!jam && !cpu.atInstructionStart()) why += "instruction not complete after the listed cycles; ";
            if (bus.log.size() != cycles.items.size()) {
                std::snprintf(buf, sizeof buf, "%zu bus cycles, expected %zu; ", bus.log.size(), cycles.items.size());
                why += buf;
            }
            for (size_t c = 0; c < cycles.items.size() && c < bus.log.size(); ++c) {
                const Json& e = cycles.at(c);
                const Access& a = bus.log[c];
                const bool w = e.at(2).str == "write";
                if (a.addr != e.at(0).i() || a.value != e.at(1).i() || a.write != w) {
                    std::snprintf(buf, sizeof buf, "cycle %zu: %s %04x=%02x, expected %s %04x=%02x; ", c + 1,
                                  a.write ? "write" : "read", a.addr, a.value, w ? "write" : "read", e.at(0).i(), e.at(1).i());
                    why += buf;
                    break;
                }
            }
            const auto& r = cpu.regs();
            if (r.pc != out["pc"].i() || r.s != out["s"].i() || r.a != out["a"].i() || r.x != out["x"].i() ||
                r.y != out["y"].i() || r.p != out["p"].i()) {
                std::snprintf(buf, sizeof buf, "regs pc=%04x s=%02x a=%02x x=%02x y=%02x p=%s, expected pc=%04x s=%02x a=%02x x=%02x y=%02x p=%s; ",
                              r.pc, r.s, r.a, r.x, r.y, flags(r.p).c_str(), out["pc"].i(), out["s"].i(), out["a"].i(),
                              out["x"].i(), out["y"].i(), flags(static_cast<uint8_t>(out["p"].i())).c_str());
                why += buf;
            }
            for (const Json& kv : out["ram"].items) {
                if (bus.mem[kv.at(0).i()] != kv.at(1).i()) {
                    std::snprintf(buf, sizeof buf, "ram %04x=%02x, expected %02x; ", kv.at(0).i(), bus.mem[kv.at(0).i()], kv.at(1).i());
                    why += buf;
                    break;
                }
            }
            if (bus.strayAccess) { std::snprintf(buf, sizeof buf, "accessed unlisted %04x; ", bus.stray); why += buf; }

            if (!why.empty()) {
                ++failed;
                if (opFail++ < maxFail && !quiet) std::printf("FAIL %s: %s\n", t["name"].str.c_str(), why.c_str());
            }
        }
        if (opFail) {
            ++opsFailed;
            std::printf("opcode %02x: %ld of %zu failed\n", op, opFail, tests.items.size());
        }
    }
    std::printf("cputest: %ld cases, %ld failed, %d opcode(s) with failures\n", total, failed, opsFailed);
    return failed ? 1 : 0;
}

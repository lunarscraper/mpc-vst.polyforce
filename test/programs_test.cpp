// MPC's preset list: the preset library as VST2 programs (names, choosing one, and a project's
// state not being overwritten by a program number the host restores right after it).
#include "host.h"
#include "../plugin/library.h"
#include "factory_presets.h"

#include <filesystem>
#include <fstream>
#include <thread>

namespace pft {
namespace {
namespace fs = std::filesystem;

std::string progName(Host& h, int i) {
    char b[256] = {};
    return h.e->dispatcher(h.e, vst::effGetProgramNameIndexed, i, 0, b, 0.0f) ? std::string(b) : std::string("<none>");
}
std::string curName(Host& h) {
    char b[256] = {};
    h.e->dispatcher(h.e, vst::effGetProgramName, 0, 0, b, 0.0f);
    return b;
}
int cur(Host& h) { return static_cast<int>(h.e->dispatcher(h.e, vst::effGetProgram, 0, 0, nullptr, 0.0f)); }
void choose(Host& h, int i) { h.e->dispatcher(h.e, vst::effSetProgram, 0, i, nullptr, 0.0f); }

int find(Host& h, const std::string& name) {
    for (int i = 0; i < h.e->numPrograms; ++i)
        if (progName(h, i) == name) return i;
    return -1;
}

} // namespace

void programTests() {
    // A user preset on disk before the plugin loads: listed after the factory set.
    fs::create_directories(fixtureDir() + "/presets/User");
    std::ofstream(fixtureDir() + "/presets/User/My Reese.pfp") << "polyforce 4\nvolume=-9\nf1_cut=432\n";
    pf::presetLibrary().rescan();

    Host h;
    // (earlier tests leave User 001/002 behind: the list is the whole library, factory set first)
    CHECK(h.e->numPrograms == static_cast<int>(pf::presetLibrary().listing()->items.size()));
    CHECK(h.e->numPrograms > pf::kNumFactoryPresets);
    CHECK(progName(h, 0) == "INIT Init");
    const int user = find(h, "USER My Reese");
    CHECK(user >= pf::kNumFactoryPresets);
    CHECK(progName(h, h.e->numPrograms) == "<none>" && progName(h, -1) == "<none>");
    bool fits = true;
    for (int i = 0; i < h.e->numPrograms; ++i) fits = fits && progName(h, i).size() <= 23;
    CHECK(fits);   // 24 bytes with the terminator

    // A fresh sound comes from no preset: the plugin's own name.
    CHECK(curName(h) == "PolyForce");

    // Choosing one loads it, and the plugin reports it as current.
    const int acid = find(h, "BASS Acid Bass");
    CHECK(acid > 0);
    choose(h, acid);
    CHECK(cur(h) == acid && curName(h) == "BASS Acid Bass");
    CHECK(h.value(pf::P_ENGINE) == pf::EN_DIRTY && h.display(pf::P_PRESET) == "PRESET  Bass / Acid Bass");
    choose(h, user);
    CHECK(cur(h) == user && h.display(pf::P_F1_CUT) == "432 Hz");

    // The same program again keeps the edits (MPC re-sends the current one).
    choose(h, acid);
    h.set(pf::P_F1_CUT, 900.0f);
    choose(h, acid);
    CHECK(h.display(pf::P_F1_CUT) == "900 Hz");

    // A preset picked on the plugin's own pages shows in MPC's list too.
    h.setN(pf::P_PRESET_NEXT, 1.0f);
    h.run(2);
    CHECK(cur(h) == acid + 1);

    // A project's state, then a host restoring program 0 on top of it: the project's sound stays.
    Host a;
    choose(a, find(a, "LEAD Hoover"));
    a.set(pf::P_F1_CUT, 1234.0f);
    const std::string project = a.chunk();
    Host b;
    CHECK(b.load(project) == 1);
    choose(b, 0);
    CHECK(b.display(pf::P_F1_CUT) == "1.23 kHz" && curName(b) == "LEAD Hoover");
    // Later, a real choice in the list works again.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    choose(b, 0);
    CHECK(curName(b) == "INIT Init");

    fs::remove_all(fixtureDir() + "/presets/User");
    pf::presetLibrary().rescan();
}

} // namespace pft

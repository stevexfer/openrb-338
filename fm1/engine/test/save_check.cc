// Engine::save_blob against the song blobs: a loaded song saves back to the same bytes (the play
// flag cleared), an edited one reloads with the edits, a new song saves to a blob that loads.
//   save_check TABLES.bin SONG.bin...
#include <stdio.h>
#include <string.h>

#include <memory>
#include <vector>

#include "../frame.h"

using namespace orb;

static bool read_file(const char* path, std::vector<uint32_t>& words, uint32_t& len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return perror(path), false;
    fseek(f, 0, SEEK_END);
    len = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    words.assign((len + 3) / 4, 0);
    bool ok = fread(words.data(), 1, len, f) == len;
    fclose(f);
    return ok;
}

static std::vector<uint8_t> save(Engine& e, const uint8_t* old, uint32_t old_len)
{
    uint32_t n = e.save_blob(old, old_len, 0, 0, 0);
    std::vector<uint8_t> out(n, 0xEE);
    // in sector-sized pieces, as the firmware writes it
    for (uint32_t off = 0; off < n; off += 4096) e.save_blob(old, old_len, off, out.data() + off, 4096);
    return out;
}

static Tables tables;
static SampleSet samples;

static std::unique_ptr<Engine> fresh() { return std::unique_ptr<Engine>(new Engine(&tables, &samples, &process_globals())); }

int main(int argc, char** argv)
{
    std::vector<uint32_t> tw;
    uint32_t tl;
    if (argc < 3 || !read_file(argv[1], tw, tl) || !tables_from_blob(tables, (const uint8_t*)tw.data(), tl)) {
        fprintf(stderr, "usage: save_check TABLES.bin SONG.bin...\n");
        return 2;
    }
    int fail = 0;
    for (int a = 2; a < argc; a++) {
        std::vector<uint32_t> sw;
        uint32_t sl;
        if (!read_file(argv[a], sw, sl)) return 1;
        const uint8_t* blob = (const uint8_t*)sw.data();
        auto e = fresh();
        if (!e->load_blob(blob, sl)) {
            printf("%s: does not load\n", argv[a]);
            fail = 1;
            continue;
        }
        std::vector<uint8_t> out = save(*e, blob, sl), want(blob, blob + sl);
        want[13] = 0;
        size_t diff = 0, first = 0;
        for (size_t i = 0; i < sl && i < out.size(); i++)
            if (out[i] != want[i] && !diff++) first = i;
        bool same = out.size() == sl && diff == 0;

        // edits: pattern mode, a knob, a mute, a 303 step, an 808 step, a 909 knob, tempo
        e->set_song_mode(0);
        e->set_param(DEV_TB303_1, Tb303::CUTOFF, 99);
        e->frame.mixer().set_enabled(2, 0);
        Tb303& b = *static_cast<Tb303*>(e->frame.devices.channels[0]);
        b.patterns[5 * Tb303::PATTERN_LEN + 2 + 6 * 3] = 11;
        static_cast<Tr808*>(e->frame.devices.channels[2])->patterns[7 * Tr808::PATTERN_LEN + 2 + 12 * 4 + 1] ^= 1;
        e->set_param(DEV_TR909, 20, 77);
        e->frame.seq.set_tempo(133000);
        std::vector<uint8_t> ed = save(*e, blob, sl);
        auto r = fresh();
        bool reload = r->load_blob(ed.data(), (uint32_t)ed.size());
        bool kept = reload && r->frame.seq.globals.song_mode == 0 && r->frame.seq.globals.tempo == 133000 &&
                    static_cast<Tb303*>(r->frame.devices.channels[0])->live_params[Tb303::CUTOFF] == 99 &&
                    r->frame.mixer().live_params.channels[2].enabled == 0 &&
                    static_cast<Tb303*>(r->frame.devices.channels[0])->patterns[5 * Tb303::PATTERN_LEN + 2 + 18] == 11 &&
                    !memcmp(static_cast<Tr808*>(r->frame.devices.channels[2])->patterns,
                            static_cast<Tr808*>(e->frame.devices.channels[2])->patterns, sizeof Tr808::patterns) &&
                    static_cast<Tr909*>(r->frame.devices.channels[3])->live_params[Tr909::internal_index(20)] == 77;
        printf("%-60s %s%s\n", argv[a], same ? "same" : "DIFFERS", kept ? ", edits kept" : ", EDITS LOST");
        if (!same) printf("  %zu bytes differ, first at %zu (%zu vs %u bytes)\n", diff, first, out.size(), sl);
        fail |= !same || !kept;
    }
    auto e = fresh();
    std::vector<uint8_t> nb = save(*e, 0, 0);
    auto r = fresh();
    bool ok = r->load_blob(nb.data(), (uint32_t)nb.size()) && save(*r, nb.data(), (uint32_t)nb.size()) == nb &&
              !strcmp(Engine::blob_title(nb.data(), (uint32_t)nb.size()), "NEW SONG");
    printf("new song: %zu bytes, %s\n", nb.size(), ok ? "loads and saves back" : "BAD");
    return fail || !ok;
}

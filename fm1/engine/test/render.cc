// The export of a song blob, as the Rust `export_song` (and fm1pack's --ref WAV) makes it:
//   render TABLES.bin SONG.bin OUT.wav KIT_0.bin [KIT_1.bin ...] [--ref REF.wav] [--exact]
//          [--idle N]
// Loads the tables, the kit regions and the song, renders, writes a 16-bit stereo 44.1 kHz WAV
// and with --ref compares it with the reference sample by sample. Exit status 1 with --exact
// when they differ.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../export.h"

using namespace orb;

static bool read_file(const char* path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return perror(path), false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// The int16 samples of a 16-bit stereo WAV (the data chunk).
static bool read_wav(const char* path, std::vector<int16_t>& out)
{
    std::vector<uint8_t> b;
    if (!read_file(path, b) || b.size() < 12 || memcmp(b.data(), "RIFF", 4) != 0) return false;
    size_t p = 12;
    while (p + 8 <= b.size()) {
        uint32_t n = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (uint32_t)b[p + 7] << 24;
        if (memcmp(b.data() + p, "data", 4) == 0) {
            n = (uint32_t)std::min<size_t>(n, b.size() - p - 8);
            out.resize(n / 2);
            memcpy(out.data(), b.data() + p + 8, out.size() * 2);
            return true;
        }
        p += 8 + n + (n & 1);
    }
    return false;
}

static void sink(void* ctx, const int16_t* frames, uint32_t n)
{
    std::vector<int16_t>* out = (std::vector<int16_t>*)ctx;
    out->insert(out->end(), frames, frames + 2 * n);
}

int main(int argc, char** argv)
{
    std::vector<const char*> pos;
    const char* ref = 0;
    bool exact = false;
    uint32_t idle = 9088;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ref") && i + 1 < argc) ref = argv[++i];
        else if (!strcmp(argv[i], "--idle") && i + 1 < argc) idle = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--exact")) exact = true;
#ifdef ORB_TEST_SOLO
        else if (!strcmp(argv[i], "--solo") && i + 1 < argc) test_solo = atoi(argv[++i]);
#endif
        else pos.push_back(argv[i]);
    }
    if (pos.size() < 4) {
        fprintf(stderr, "usage: render TABLES.bin SONG.bin OUT.wav KIT_0.bin [KIT_1.bin ...] [--ref REF.wav] [--exact] "
                        "[--idle N]\n");
        return 2;
    }
    std::vector<uint8_t> tables_blob, song;
    std::vector<std::vector<uint8_t>> kit(pos.size() - 3);
    if (!read_file(pos[0], tables_blob) || !read_file(pos[1], song)) return 1;
    for (size_t r = 0; r < kit.size(); r++)
        if (!read_file(pos[3 + r], kit[r])) return 1;
    // the blobs as the FM-1 has them in flash: 4-byte aligned
    std::vector<uint32_t> tables_words((tables_blob.size() + 3) / 4), song_words((song.size() + 3) / 4);
    memcpy(tables_words.data(), tables_blob.data(), tables_blob.size());
    memcpy(song_words.data(), song.data(), song.size());

    static Tables tables;
    if (!tables_from_blob(tables, (const uint8_t*)tables_words.data(), (uint32_t)tables_blob.size())) {
        fprintf(stderr, "%s: not a tables blob\n", pos[0]);
        return 1;
    }
    static SampleSet samples;
    std::vector<const uint8_t*> regions;
    std::vector<uint32_t> lens;
    for (auto& k : kit) {
        regions.push_back(k.data());
        lens.push_back((uint32_t)k.size());
    }
    int missing = kit_from_regions(samples, regions.data(), lens.data(), (int)regions.size());
    if (missing < 0) {
        fprintf(stderr, "%s: not a kit\n", pos[3]);
        return 1;
    }
    const uint8_t* blob = (const uint8_t*)song_words.data();
    uint32_t blob_len = (uint32_t)song.size();
    const char* title = Engine::blob_title(blob, blob_len);
    printf("song \"%s\", tables %s, kit missing-region mask 0x%x\n", title ? title : "?",
           tables.tb303_filter ? "full" : "computed", missing);

    std::unique_ptr<Engine> engine(new Engine(&tables, &samples, &process_globals()));
    std::vector<int16_t> out;
    std::vector<int16_t> buf(2 * CHUNK_FRAMES);
    auto t0 = std::chrono::steady_clock::now();
    if (!export_song(*engine, blob, blob_len, idle, buf.data(), sink, &out)) {
        fprintf(stderr, "%s: not a song blob\n", pos[1]);
        return 1;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint32_t frames = (uint32_t)(out.size() / 2);
    // the idle blocks render too
    double rendered = (double)frames + (double)idle * BLOCK;
    printf("rendered %u frames (+%u idle blocks) in %.3f s: %.1fx realtime\n", frames, idle, secs,
           rendered / 44100.0 / secs);
    const Pool& pool = engine->frame.seq.pool;
    printf("pool: %u of %u nodes used, %u external lists copied, %u copies did not fit\n", pool.used(), POOL_NODES,
           pool.copies(), pool.copy_failures());
    if (const FaultRecord* f = engine->fault())
        printf("fault: PCF wave %u, block %llu, %llu calls\n", f->first.wave, (unsigned long long)f->block,
               (unsigned long long)f->calls);

    FILE* w = fopen(pos[2], "wb");
    if (!w) return perror(pos[2]), 1;
    uint8_t h[44];
    wav_header(frames, h);
    fwrite(h, 1, 44, w);
    fwrite(out.data(), 2, out.size(), w);
    fclose(w);

    if (!ref) return 0;
    std::vector<int16_t> want;
    if (!read_wav(ref, want)) {
        fprintf(stderr, "%s: not a WAV\n", ref);
        return 1;
    }
    size_t n = std::min(out.size(), want.size());
    size_t same = 0, first = (size_t)-1;
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        double a = want[i], d = (double)out[i] - a;
        sig += a * a;
        err += d * d;
        if (out[i] == want[i]) same++;
        else if (first == (size_t)-1) first = i;
    }
    bool identical = same == n && out.size() == want.size();
    double snr = err > 0 ? 10 * log10(sig / err) : HUGE_VAL;
    printf("ref: %zu of %zu samples identical (%zu ref, %zu out)", same, n, want.size(), out.size());
    if (first != (size_t)-1) printf(", first difference at frame %zu", first / 2);
    if (identical) printf(", BIT-EXACT\n");
    else printf(", SNR %.2f dB\n", snr);
    return exact && !identical ? 1 : 0;
}

// The FM-1 kit's sample decoders against the PCM kit:
//   kit_check PCM_0.bin KIT_0.bin [KIT_1.bin ...]
// Decodes every sample of the kit through SampleCursor/sample_at, as the voices read them (in
// order from 0, and a pitched voice's overlapping pairs at a step of 0.75), and compares it with
// the PCM kit's. A FLAC sample must be the PCM exactly, or (fm1pack's near-lossless samples) the
// PCM rounded per FLAC frame to a power-of-two step 2^k, exactly; an ADPCM one reports its SNR.
// Exit status 1 when a sample's length differs, a FLAC frame is neither, or the pitched reads
// differ from the in-order ones.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "../orb_core.h"

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

static int load(SampleSet& set, std::vector<std::vector<uint8_t>>& files, const char* name)
{
    std::vector<const uint8_t*> regions;
    std::vector<uint32_t> lens;
    for (auto& f : files) {
        regions.push_back(f.data());
        lens.push_back((uint32_t)f.size());
    }
    int m = kit_from_regions(set, regions.data(), lens.data(), (int)regions.size());
    if (m < 0) fprintf(stderr, "%s: not a kit\n", name);
    else printf("%s: missing-region mask 0x%x\n", name, m);
    return m;
}

// fm1pack's near_lossless rounding of x to a step of 2^k.
static int32_t rounded(int32_t x, int k)
{
    if (k == 0) return x;
    int32_t hi = (32767 >> k) << k, q = ((x + (1 << (k - 1))) >> k) << k;
    return q < -32768 ? -32768 : q > hi ? hi : q;
}

int main(int argc, char** argv)
{
    if (argc < 3) return fprintf(stderr, "usage: kit_check PCM_0.bin KIT_0.bin [KIT_1.bin ...]\n"), 2;
    std::vector<std::vector<uint8_t>> pcm_files(1), kit_files(argc - 2);
    if (!read_file(argv[1], pcm_files[0])) return 1;
    for (int i = 2; i < argc; i++)
        if (!read_file(argv[i], kit_files[i - 2])) return 1;
    static SampleSet pcm, kit;
    if (load(pcm, pcm_files, argv[1]) != 0 || load(kit, kit_files, argv[2]) != 0) return 1;

    static const char* const CODEC[3] = {"pcm", "flac", "adpcm"};
    bool bad = false;
    double sig_all = 0, err_all = 0;
    uint32_t exact = 0, rounded_n = 0, adpcm = 0;
    static SampleCursor a, b;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        const Sample* sp = &pcm.s[i];
        const Sample* sk = &kit.s[i];
        uint16_t id = (uint16_t)(SAMPLE_FIRST_ID + i);
        printf("  %u %-5s %7u frames: ", id, CODEC[sk->codec], sk->frames);
        if (sp->frames != sk->frames) {
            printf("the PCM kit's has %u frames\n", sp->frames);
            bad = true;
            continue;
        }
        a.reset(sp);
        b.reset(sk);
        std::vector<int32_t> x(sp->frames + 600), y(sp->frames + 600);
        double sig = 0, err = 0;
        for (uint32_t k = 0; k < x.size(); k++) {
            x[k] = sample_at(a, k);
            y[k] = sample_at(b, k);
            sig += (double)x[k] * x[k];
            err += (double)(y[k] - x[k]) * (y[k] - x[k]);
        }
        // a pitched voice's pairs (i, i+1) at a step of 0.75: the reads go back by one
        b.reset(sk);
        uint32_t pdiff = 0;
        for (uint32_t ph = 0; (ph >> 8) < sp->frames + 2; ph += 192)
            for (uint32_t k = ph >> 8; k < (ph >> 8) + 2; k++) pdiff += sample_at(b, k) != y[k];
        // the steps of the FLAC frames
        int kmax = 0;
        bool consistent = true;
        if (sk->codec == CODEC_FLAC) {
            for (uint32_t f = 0; f < sk->frames && consistent; f += FLAC_BLOCK) {
                uint32_t end = f + FLAC_BLOCK < sk->frames ? f + FLAC_BLOCK : sk->frames;
                int k = 0;
                for (; k <= 12; k++) {
                    uint32_t j = f;
                    while (j < end && rounded(x[j], k) == y[j]) j++;
                    if (j == end) break;
                }
                if (k > 12) consistent = false;
                else if (k > kmax) kmax = k;
            }
        }
        sig_all += sig;
        err_all += err;
        double snr = err > 0 ? 10 * log10(sig / err) : HUGE_VAL;
        if (pdiff) {
            printf("pitched reads differ (%u)\n", pdiff);
            bad = true;
        } else if (sk->codec == CODEC_ADPCM) {
            printf("SNR %.1f dB\n", snr);
            adpcm++;
        } else if (!consistent) {
            printf("WRONG: not the PCM rounded per frame, SNR %.1f dB\n", snr);
            bad = true;
        } else if (err == 0) {
            printf("identical\n");
            exact++;
        } else {
            printf("rounded exactly (steps up to 2^%d), SNR %.1f dB\n", kmax, snr);
            rounded_n++;
        }
    }
    printf("kit: %u identical, %u near-lossless FLAC decoded exactly, %u ADPCM; SNR over all samples %.1f dB; %s\n",
           exact, rounded_n, adpcm, err_all > 0 ? 10 * log10(sig_all / err_all) : HUGE_VAL, bad ? "FAIL" : "ok");
    return bad ? 1 : 0;
}

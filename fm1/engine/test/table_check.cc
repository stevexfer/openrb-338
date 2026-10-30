// The computed filter and shaper entries (orb_tables.cc) against the Rust engine's tables:
//   table_check TABLES_FULL.bin
// Prints the largest relative error of each coefficient, relative to the coefficient's own
// range so that entries near zero do not dominate.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "../orb_core.h"

using namespace orb;

struct Err {
    double worst = 0;
    unsigned at_row = 0, at_idx = 0;
    void add(double a, double b, double scale, unsigned r, unsigned i)
    {
        double e = fabs(a - b) / scale;
        if (e > worst) {
            worst = e;
            at_row = r;
            at_idx = i;
        }
    }
};

int main(int argc, char** argv)
{
    if (argc != 2) return fprintf(stderr, "usage: table_check TABLES_FULL.bin\n"), 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return perror(argv[1]), 1;
    std::vector<uint32_t> buf(1 << 20);
    size_t n = fread(buf.data(), 1, buf.size() * 4, f);
    fclose(f);
    Tables t;
    if (!tables_from_blob(t, (const uint8_t*)buf.data(), (uint32_t)n) || !t.tb303_filter)
        return fprintf(stderr, "not a full tables blob\n"), 1;
    bool bad = false;
    auto report = [&](const char* name, int k, Err& e, double limit) {
        printf("%-14s c%d  max rel err %.3g  (row %u, idx %u)\n", name, k, e.worst, e.at_row, e.at_idx);
        if (e.worst > limit) bad = true;
    };
    {
        Err e[4];
        double scale[4] = {0, 0, 0, 0};
        for (unsigned r = 0; r < 8; r++)
            for (unsigned i = 0; i < 1024; i++)
                for (int k = 0; k < 4; k++) scale[k] = fmax(scale[k], fabs(t.tb303_filter[r][i][k]));
        for (unsigned r = 0; r < 8; r++)
            for (unsigned i = 0; i < 1024; i++) {
                float c[4];
                tb303_filter_entry(r, i, c);
                for (int k = 0; k < 4; k++) e[k].add(c[k], t.tb303_filter[r][i][k], scale[k], r, i);
            }
        for (int k = 0; k < 4; k++) report("tb303_filter", k, e[k], 1e-5);
    }
    for (int bp = 0; bp < 2; bp++) {
        const float(*tab)[512][3] = bp ? t.pcf_bandpass : t.pcf_lowpass;
        Err e[3];
        double scale[3] = {0, 0, 0};
        for (unsigned r = 0; r < 8; r++)
            for (unsigned i = 0; i < 512; i++)
                for (int k = 0; k < 3; k++) scale[k] = fmax(scale[k], fabs(tab[r][i][k]));
        for (unsigned r = 0; r < 8; r++)
            for (unsigned i = 0; i < 512; i++) {
                float c[3];
                pcf_filter_entry(bp, r, i, c);
                for (int k = 0; k < 3; k++) e[k].add(c[k], tab[r][i][k], scale[k], r, i);
            }
        for (int k = 0; k < 3; k++) report(bp ? "pcf_bandpass" : "pcf_lowpass", k, e[k], 1e-5);
    }
    {
        Err e;
        for (unsigned r = 0; r < 32; r++) {
            float w[256];
            dist_shaper_row(r, w);
            for (unsigned i = 0; i < 256; i++) e.add(w[i], t.dist_shaper[r][i], 256.0 * 0.7, r, i);
        }
        report("dist_shaper", 0, e, 1e-5);
    }
    // the filters' pole terms: the gain `c1 + (1 - c0)` the coefficients imply, relative to
    // the table's, at every entry (what the low cutoffs' cancellation would spoil)
    {
        Err e;
        for (unsigned r = 0; r < 8; r++)
            for (unsigned i = 0; i < 1024; i++) {
                float c[4];
                tb303_filter_entry(r, i, c);
                const float* x = t.tb303_filter[r][i];
                e.add(c[2], x[2], fabs(x[2]) > 0 ? fabs(x[2]) : 1, r, i);
            }
        report("tb303 c2 (own)", 2, e, 1e-4);
    }
    return bad ? 1 : 0;
}

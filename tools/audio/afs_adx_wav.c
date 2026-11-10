// Every ADX stream of an AFS archive -> 16-bit WAV (<prefix>_NN.wav). CRI ADX v4 with or without type-8 encryption
// (Clover's key, as src/kamite/launcher_bark.cpp). Build: cc -O2 afs_adx_wav.c -o afs_adx_wav -lm
// Usage: afs_adx_wav se/ev/ev04.afs out/ev04   (the cutscene streams: voice, music and SFX in one stereo track)
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void wav(const char *fn, const int16_t *s, long frames, int ch, int rate)
{
    FILE *f = fopen(fn, "wb");
    uint32_t db = (uint32_t)(frames * ch * 2), v;
    uint16_t w;
    fwrite("RIFF", 1, 4, f); v = 36 + db; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    w = 1; fwrite(&w, 2, 1, f); w = (uint16_t)ch; fwrite(&w, 2, 1, f); v = (uint32_t)rate; fwrite(&v, 4, 1, f);
    v = (uint32_t)(rate * ch * 2); fwrite(&v, 4, 1, f); w = (uint16_t)(ch * 2); fwrite(&w, 2, 1, f); w = 16; fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&db, 4, 1, f); fwrite(s, 2, (size_t)(frames * ch), f); fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: afs_adx_wav <file.afs> <out prefix>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    int count = (int)le32(d + 4);
    for (int e = 0; e < count; e++)
    {
        uint32_t o = le32(d + 8 + 8 * e), sz = le32(d + 12 + 8 * e);
        if (!sz || (long)o + sz > n || d[o] != 0x80) continue;
        const uint8_t *b = d + o;
        int off = be16(b + 2), bs = b[5], bits = b[6], ch = b[7], rate = (int)be32(b + 8), hp = be16(b + 16), enc = b[0x13] == 8;
        long ns = (long)be32(b + 12);
        double a = sqrt(2.0) - cos(2 * M_PI * hp / rate), c0 = sqrt(2.0) - 1, cc = (a - sqrt((a + c0) * (a - c0))) / c0;
        int c1 = (int)(cc * 8192), c2 = (int)(-cc * cc * 4096), spb = (bs - 2) * 8 / bits;
        int16_t *out = calloc((size_t)(ns + spb) * ch, 2);
        int p1[8] = {0}, p2[8] = {0}, key[8];
        for (int k = 0; k < ch; k++) { key[k] = 0x49E1; for (int j = 0; j < k; j++) key[k] = (key[k] * 0x4A57 + 0x553D) & 0x7FFF; }
        long pos = off + 4, got = 0;
        int stop = 0;
        while (got < ns && pos + bs * ch <= (long)sz && !stop)
        {
            for (int k = 0; k < ch; k++)
            {
                const uint8_t *bl = b + pos; pos += bs;
                int sc = be16(bl);
                if (sc == 0x8001) { stop = 1; break; }
                if (enc) { sc = ((sc ^ key[k]) & 0x1FFF) + 1; for (int j = 0; j < ch; j++) key[k] = (key[k] * 0x4A57 + 0x553D) & 0x7FFF; }
                for (int i = 0; i < spb && got + i < ns; i++)
                {
                    int by = bl[2 + i / 2], nb = (i & 1) ? (by & 15) : (by >> 4);
                    if (nb >= 8) nb -= 16;
                    int s = nb * sc + ((c1 * p1[k] + c2 * p2[k]) >> 12);
                    s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
                    out[(got + i) * ch + k] = (int16_t)s; p2[k] = p1[k]; p1[k] = s;
                }
            }
            if (!stop) got += spb;
        }
        if (got > ns) got = ns;
        char fn[1024];
        snprintf(fn, sizeof(fn), "%s_%02d.wav", argv[2], e);
        wav(fn, out, got, ch, rate);
        printf("%s %d ch %d Hz %.2f s\n", fn, ch, rate, (double)got / rate);
        free(out);
    }
    return 0;
}

/* Native MPC drum program (.xpm) from a loaded Maschine group.
 * Template and rules follow Lucky Dip / force-kit-builder: SliceEnd must be the
 * real frame count (0 plays silence on a Force), samples sit next to the .xpm. */
#include "play.h"
#include "mxgrp.h"
#include "xpm_template.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { XPM_PADS = 128, SR = 44100, CAP_FRAMES = SR * 2 };

static const unsigned TINT_RGB[6] = {
    16766720u, /* other / yellow */
    16711680u, /* kick */
    16744192u, /* snare */
    13144319u, /* hat / lilac */
    255u,      /* percussion */
    2263842u   /* tom */
};

static int mkdir_p(const char *path) {
    char cur[768];
    size_t n = strlen(path);
    if (n >= sizeof cur) return 0;
    cur[0] = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || path[i] == '/') {
            if (cur[0] && mkdir(cur, 0777) != 0 && errno != EEXIST) return 0;
        }
        if (i < n) {
            size_t L = strlen(cur);
            cur[L] = path[i];
            cur[L + 1] = 0;
        }
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int replace_first(char *s, const char *from, const char *to) {
    char *p = strstr(s, from);
    if (!p) return 0;
    size_t fl = strlen(from), tl = strlen(to), rest = strlen(p + fl);
    memmove(p + tl, p + fl, rest + 1);
    memcpy(p, to, tl);
    return 1;
}

static void xml_esc(const char *s, char *out, int n) {
    int w = 0;
    for (; s && *s && w < n - 8; s++) {
        if (*s == '&') w += snprintf(out + w, (size_t)(n - w), "&amp;");
        else if (*s == '<') w += snprintf(out + w, (size_t)(n - w), "&lt;");
        else if (*s == '>') w += snprintf(out + w, (size_t)(n - w), "&gt;");
        else out[w++] = *s;
    }
    out[w] = 0;
}

static void safe_name(const char *in, char *out, int n) {
    int w = 0, ws = 0;
    for (const char *p = in; p && *p && w < n - 1; p++) {
        unsigned char c = (unsigned char)*p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == ' ' || c == '(' || c == ')' || c == '_' || c == '-';
        if (!ok) {
            if (w && out[w - 1] != '_') out[w++] = '_';
            ws = 0;
            continue;
        }
        if (c == ' ') {
            if (ws) continue;
            ws = 1;
        } else
            ws = 0;
        out[w++] = (char)c;
    }
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '_' || out[w - 1] == '-')) w--;
    out[w] = 0;
    if (!out[0]) snprintf(out, (size_t)n, "kit");
}

static void crlf_write(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '\n') fputc('\r', f);
        fputc(*s, f);
    }
}

static int write_wav(const char *path, const int16_t *pcm, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int data = n * 2, rate = SR;
    unsigned char hdr[44];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "RIFF", 4);
    int chunk = 36 + data;
    hdr[4] = chunk; hdr[5] = chunk >> 8; hdr[6] = chunk >> 16; hdr[7] = chunk >> 24;
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16;
    hdr[20] = 1;
    hdr[22] = 1;
    hdr[24] = rate; hdr[25] = rate >> 8; hdr[26] = rate >> 16; hdr[27] = rate >> 24;
    int br = rate * 2;
    hdr[28] = br; hdr[29] = br >> 8; hdr[30] = br >> 16; hdr[31] = br >> 24;
    hdr[32] = 2;
    hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    hdr[40] = data; hdr[41] = data >> 8; hdr[42] = data >> 16; hdr[43] = data >> 24;
    int ok = fwrite(hdr, 1, 44, f) == 44 && fwrite(pcm, 2, (size_t)n, f) == (size_t)n;
    return fclose(f) == 0 && ok;
}

static int capture_pad(PlayKit *k, int pad, int16_t *mono, int max) {
    play_all_off(k);
    play_note(k, pad, 110);
    int have = 0, last = 0;
    int16_t buf[256];
    while (have + 128 <= max) {
        play_render(k, buf, 128, 1.f, 0);
        int any = 0;
        for (int i = 0; i < 128; i++) {
            int32_t s = ((int32_t)buf[2 * i] + buf[2 * i + 1]) / 2;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            mono[have + i] = (int16_t)s;
            if (s > 80 || s < -80) {
                any = 1;
                last = have + i;
            }
        }
        have += 128;
        if (!any && have > SR / 4 && !play_pad_active(k, pad)) break;
    }
    play_all_off(k);
    last += SR / 50;
    if (last > have) last = have;
    if (last < 64) last = have > 64 ? 64 : have;
    return last;
}

static void colour_pads(char *pads_json, const PlayKit *k) {
    char *from = strstr(pads_json, "\"pads\"");
    if (!from) return;
    for (int i = 0; i < 16; i++) {
        char key[32];
        snprintf(key, sizeof key, "\"value%d\": ", i);
        char *p = strstr(from, key);
        if (!p) continue;
        p += strlen(key);
        char *e = p;
        while (*e >= '0' && *e <= '9') e++;
        int tint = play_pad_tint(play_pad_name(k, i));
        if (tint < 0 || tint > 5) tint = 0;
        if (!play_pad_name(k, i)[0]) continue;
        char num[16];
        snprintf(num, sizeof num, "%u", TINT_RGB[tint]);
        size_t old = (size_t)(e - p), nw = strlen(num);
        memmove(p + nw, e, strlen(e) + 1);
        memcpy(p, num, nw);
        (void)old;
    }
}

static void mute_groups(const PlayKit *k, int grp[MX_PADS]) {
    for (int i = 0; i < MX_PADS; i++) grp[i] = 0;
    int next = 1;
    for (int i = 0; i < MX_PADS; i++) {
        int ch[4], n = play_pad_chokes(k, i, ch, 4);
        if (!n) continue;
        int g = grp[i];
        if (!g) g = next++;
        grp[i] = g;
        for (int j = 0; j < n; j++)
            if (ch[j] >= 0 && ch[j] < MX_PADS) grp[ch[j]] = g;
    }
}

int play_export_xpm(PlayKit *k, const char *dir, const char *name_in, char *err, int err_n) {
    if (err && err_n) err[0] = 0;
    if (!k || !dir || !name_in) {
        if (err) snprintf(err, (size_t)err_n, "no group loaded");
        return 0;
    }
    char name[80];
    safe_name(name_in, name, sizeof name);
    char folder[768];
    snprintf(folder, sizeof folder, "%s/%s", dir, name);
    if (!mkdir_p(folder)) {
        if (err) snprintf(err, (size_t)err_n, "cannot create %s", folder);
        return 0;
    }

    int mute[MX_PADS];
    mute_groups(k, mute);
    char used[16][48];
    int frames[16];
    int have[16];
    memset(used, 0, sizeof used);
    memset(have, 0, sizeof have);
    memset(frames, 0, sizeof frames);

    int16_t *cap = malloc((size_t)CAP_FRAMES * 2);
    if (!cap) {
        if (err) snprintf(err, (size_t)err_n, "out of memory");
        return 0;
    }

    int nwav = 0;
    for (int p = 0; p < 16; p++) {
        const char *nm = play_pad_name(k, p);
        if (!nm[0] || play_pad_src(k, p) == MX_EMPTY) continue;
        char sn[48];
        safe_name(nm[0] ? nm : "pad", sn, sizeof sn);
        for (int q = 0; q < p; q++)
            if (have[q] && !strcmp(used[q], sn)) {
                snprintf(sn + strlen(sn), sizeof sn - strlen(sn), " %d", p + 1);
                break;
            }
        if (strlen(sn) > 42) sn[42] = 0;
        snprintf(used[p], sizeof used[p], "%s", sn);

        char wav[800];
        snprintf(wav, sizeof wav, "%s/%s.wav", folder, sn);
        int st = 0, en = 0, fr = 0;
        const int16_t *pcm = play_pad_pcm(k, p, &st, &en, &fr);
        if (pcm && en > st) {
            if (!write_wav(wav, pcm + st, en - st)) {
                free(cap);
                if (err) snprintf(err, (size_t)err_n, "wav write failed");
                return 0;
            }
            frames[p] = en - st;
            have[p] = 1;
            nwav++;
        } else if (play_pad_src(k, p) != MX_SAMPLE) {
            int n = capture_pad(k, p, cap, CAP_FRAMES);
            if (n > 32 && write_wav(wav, cap, n)) {
                frames[p] = n;
                have[p] = 1;
                nwav++;
            }
        }
    }
    free(cap);
    if (!nwav) {
        if (err) snprintf(err, (size_t)err_n, "no playable pads");
        return 0;
    }

    char xpm[800];
    snprintf(xpm, sizeof xpm, "%s/%s.xpm", folder, name);
    FILE *f = fopen(xpm, "wb");
    if (!f) {
        if (err) snprintf(err, (size_t)err_n, "cannot write %s", xpm);
        return 0;
    }
    crlf_write(f, XPM_HEAD);
    char esc[160];
    xml_esc(name, esc, sizeof esc);
    fprintf(f, "    <ProgramName>%s</ProgramName>\r\n", esc);

    char *pads = malloc(strlen(XPM_PROGPADS) + 512);
    if (!pads) {
        fclose(f);
        if (err) snprintf(err, (size_t)err_n, "out of memory");
        return 0;
    }
    memcpy(pads, XPM_PROGPADS, strlen(XPM_PROGPADS) + 1);
    colour_pads(pads, k);
    crlf_write(f, pads);
    free(pads);

    crlf_write(f, XPM_PROG_PARAMS);

    size_t ilen = strlen(XPM_INSTRUMENT);
    char *inst = malloc(ilen + 256);
    if (!inst) {
        fclose(f);
        if (err) snprintf(err, (size_t)err_n, "out of memory");
        return 0;
    }
    for (int n = 1; n <= XPM_PADS; n++) {
        memcpy(inst, XPM_INSTRUMENT, ilen + 1);
        char num[32];
        snprintf(num, sizeof num, "<Instrument number=\"%d\">", n);
        replace_first(inst, "<Instrument number=\"1\">", num);
        int p = n - 1;
        if (n <= 16 && have[p]) {
            char tag[80];
            xml_esc(used[p], esc, sizeof esc);
            snprintf(tag, sizeof tag, "<SampleName>%s</SampleName>", esc);
            replace_first(inst, "<SampleName>PRGKIT26BD1</SampleName>", tag);
            snprintf(tag, sizeof tag, "<SliceEnd>%d</SliceEnd>", frames[p]);
            replace_first(inst, "<SliceEnd>33688</SliceEnd>", tag);
            if (mute[p]) {
                snprintf(tag, sizeof tag, "<MuteGroup>%d</MuteGroup>", mute[p]);
                replace_first(inst, "<MuteGroup>0</MuteGroup>", tag);
            }
        } else {
            replace_first(inst, "<SampleName>PRGKIT26BD1</SampleName>", "<SampleName></SampleName>");
            replace_first(inst, "<SliceEnd>33688</SliceEnd>", "<SliceEnd>0</SliceEnd>");
            replace_first(inst, "<WarpTempo>20.000000</WarpTempo>", "<WarpTempo>120.000000</WarpTempo>");
        }
        crlf_write(f, inst);
    }
    free(inst);
    crlf_write(f, "    </Instruments>\n");
    crlf_write(f, "    <PadNoteMap>\n");
    for (int i = 1; i <= XPM_PADS; i++)
        fprintf(f, "      <PadNote>\r\n        <Number>%d</Number>\r\n      </PadNote>\r\n", (35 + i) % 128);
    crlf_write(f, "    </PadNoteMap>\n    <PadGroupMap>\n");
    for (int i = 1; i <= XPM_PADS; i++)
        fprintf(f, "      <PadGroup>\r\n        <Number>0</Number>\r\n      </PadGroup>\r\n");
    crlf_write(f, "    </PadGroupMap>\n");
    crlf_write(f, XPM_TAIL);
    if (fclose(f) != 0) {
        if (err) snprintf(err, (size_t)err_n, "xpm write failed");
        return 0;
    }
    return 1;
}

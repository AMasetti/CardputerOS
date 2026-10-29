/*
 * Minimal ELF loader for CardputerOS apps (Xtensa, 32-bit little endian).
 *
 * Apps are linked as fixed-address executables with two PT_LOAD segments: code
 * (R+X, link address 0x00000000..) and data (R+W, link address 0x10000000..) and
 * keep their relocations (--emit-relocs). Apps never import symbols: every OS
 * service is reached through the cpos_api_t table passed to the entry point, so
 * the only relocation that matters is R_XTENSA_32 (an absolute address stored in
 * a literal pool or a data word). The stored value already holds the link-time
 * address, so relocating means adding the load offset of whichever segment it
 * points into.
 *
 * When the heap has no single block for the code, it is loaded in parts. The build lists in
 * .cpos_splits the offsets where that is safe (between functions, crossed by no PC-relative
 * jump, call or constant load), so only the absolute words above need to know about the parts.
 */
#include "loader.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "loader";

#define EI_NIDENT 16
typedef struct {
    uint8_t e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr_t;
typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr_t;
typedef struct {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr_t;
typedef struct { uint32_t r_offset, r_info; int32_t r_addend; } Elf32_Rela_t;

#define PT_LOAD 1
#define PF_X 1
#define SHT_RELA 4
#define EM_XTENSA 94
#define R_XTENSA_32 1

static int rd(FILE *f, long off, void *dst, size_t n) {
    if (fseek(f, off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, n, f) == n ? 0 : -1;
}

static int read_ehdr(FILE *f, Elf32_Ehdr_t *eh, const char **err) {
    if (rd(f, 0, eh, sizeof(*eh))) { *err = "cannot read file"; return -1; }
    if (memcmp(eh->e_ident, "\177ELF", 4) || eh->e_ident[4] != 1 || eh->e_ident[5] != 1) { *err = "not a 32-bit LE ELF"; return -1; }
    if (eh->e_machine != EM_XTENSA) { *err = "not an Xtensa ELF"; return -1; }
    if (eh->e_phentsize != sizeof(Elf32_Phdr_t) || eh->e_shentsize != sizeof(Elf32_Shdr_t)) { *err = "bad ELF tables"; return -1; }
    return 0;
}

/* find section by name; returns 0 and fills sh */
static int find_section(FILE *f, const Elf32_Ehdr_t *eh, const char *name, Elf32_Shdr_t *out) {
    Elf32_Shdr_t str;
    if (rd(f, eh->e_shoff + eh->e_shstrndx * sizeof(str), &str, sizeof(str))) return -1;
    size_t nlen = strlen(name);
    char buf[32];
    for (int i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr_t sh;
        if (rd(f, eh->e_shoff + i * sizeof(sh), &sh, sizeof(sh))) return -1;
        if (rd(f, str.sh_offset + sh.sh_name, buf, nlen + 1)) continue;
        if (memcmp(buf, name, nlen + 1) == 0) { *out = sh; return 0; }
    }
    return -1;
}

int cpos_read_header(const char *path, cpos_app_header_t *out, const char **err) {
    static const char *e0;
    if (!err) err = &e0;
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "cannot open file"; return -1; }
    Elf32_Ehdr_t eh;
    Elf32_Shdr_t sh;
    int rc = -1;
    if (read_ehdr(f, &eh, err) == 0) {
        if (find_section(f, &eh, ".cpos_app", &sh) || sh.sh_size < sizeof(*out)) *err = "missing .cpos_app header";
        else if (rd(f, sh.sh_offset, out, sizeof(*out))) *err = "cannot read header";
        else if (out->magic != CPOS_APP_MAGIC) *err = "bad app magic";
        else rc = 0;
    }
    fclose(f);
    return rc;
}

/*
 * Code lives in executable RAM that we reach through the instruction-bus alias. Only aligned
 * 32-bit accesses are used there (it is what the IDF heap itself does for EXEC allocations), so
 * the same code path works on the chip and in the emulator.
 */
/* where code offset off (0..code_size) lives in memory */
static uint8_t *code_at(const cpos_image_t *img, size_t off) {
    int i = 0;
    while (i + 1 < img->nparts && off >= img->part_off[i + 1]) i++;
    return (uint8_t *)img->part[i] + (off - img->part_off[i]);
}

static int part_of(const cpos_image_t *img, uint32_t addr) {
    for (int i = 0; i < img->nparts; i++) {
        uint32_t p = (uint32_t)img->part[i];
        if (addr >= p && addr < p + (img->part_off[i + 1] - img->part_off[i])) return i;
    }
    return -1;
}

static void write_code(cpos_image_t *img, size_t off, const void *src, size_t n) {
    const uint8_t *s = src;
    while (n) {
        int i = 0;
        while (i + 1 < img->nparts && off >= img->part_off[i + 1]) i++;
        size_t m = img->part_off[i + 1] - off < n ? img->part_off[i + 1] - off : n;
        volatile uint32_t *d = (volatile uint32_t *)((uint8_t *)img->part[i] + (off - img->part_off[i]));
        for (size_t k = 0; k < m; k += 4) {
            uint32_t w = 0;
            memcpy(&w, s + k, m - k < 4 ? m - k : 4);
            *d++ = w;
        }
        off += m;
        s += m;
        n -= m;
    }
}

/* sequential reader over the cut-point list */
typedef struct { FILE *f; uint32_t off, n, i, base, cnt; uint32_t buf[32]; } splits_t;

static int split_get(splits_t *sp, uint32_t i, uint32_t *v) {
    if (i >= sp->n) return -1;
    if (i < sp->base || i >= sp->base + sp->cnt) {
        sp->base = i;
        sp->cnt = sp->n - i < 32 ? sp->n - i : 32;
        if (rd(sp->f, sp->off + i * 4, sp->buf, sp->cnt * 4)) return -1;
    }
    *v = sp->buf[i - sp->base];
    return 0;
}

/* one block if possible, else the largest free blocks in turn, each filled up to the last
 * cut point that fits */
static int alloc_code(FILE *f, const Elf32_Ehdr_t *eh, cpos_image_t *img) {
    size_t size = img->code_size;
    img->part[0] = heap_caps_malloc(size + 4, MALLOC_CAP_EXEC);
    if (img->part[0]) {
        img->nparts = 1;
        img->part_off[1] = size;
        return 0;
    }
    Elf32_Shdr_t sh;
    if (find_section(f, eh, ".cpos_splits", &sh) || sh.sh_size < 4) return -1;
    splits_t sp = {.f = f, .off = sh.sh_offset, .n = sh.sh_size / 4};
    size_t pos = 0;
    while (pos < size) {
        if (img->nparts == CPOS_MAX_CODE_PARTS) return -1;
        size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
        if (room < 64) return -1;
        room -= 16; /* the 4 spare bytes and allocator rounding */
        size_t end = 0;
        if (size - pos + 4 <= room) end = size;
        else {
            uint32_t s;
            while (split_get(&sp, sp.i, &s) == 0 && s - pos + 4 <= room) {
                if (s > pos) end = s;
                sp.i++;
            }
        }
        if (end <= pos) return -1; /* even the next piece does not fit */
        void *p = heap_caps_malloc(end - pos + 4, MALLOC_CAP_EXEC);
        if (!p) return -1;
        img->part[img->nparts] = p;
        img->part_off[img->nparts] = pos;
        img->nparts++;
        img->part_off[img->nparts] = end;
        pos = end;
    }
    return 0;
}

void cpos_unload(cpos_image_t *img) {
    for (int i = 0; i < img->nparts; i++) heap_caps_free(img->part[i]);
    if (img->data) heap_caps_free(img->data);
    memset(img, 0, sizeof(*img));
}

int cpos_load(const char *path, cpos_image_t *img, const char **err) {
    static const char *e0;
    if (!err) err = &e0;
    memset(img, 0, sizeof(*img));
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "cannot open file"; return -1; }
    Elf32_Ehdr_t eh;
    Elf32_Phdr_t ph[4], code_ph = {0}, data_ph = {0};
    int have_code = 0, have_data = 0;
    uint8_t *tmp = NULL;
    int rc = -1;

    if (read_ehdr(f, &eh, err)) goto out;
    if (eh.e_phnum == 0 || eh.e_phnum > 4) { *err = "unexpected program headers"; goto out; }
    if (rd(f, eh.e_phoff, ph, eh.e_phnum * sizeof(ph[0]))) { *err = "cannot read program headers"; goto out; }
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_flags & PF_X) { code_ph = ph[i]; have_code = 1; }
        else { data_ph = ph[i]; have_data = 1; }
    }
    if (!have_code) { *err = "no code segment"; goto out; }

    /* manifest */
    Elf32_Shdr_t sh;
    if (find_section(f, &eh, ".cpos_app", &sh) || rd(f, sh.sh_offset, &img->header, sizeof(img->header)) || img->header.magic != CPOS_APP_MAGIC) {
        *err = "missing .cpos_app header";
        goto out;
    }
    if (img->header.abi_major != CPOS_ABI_MAJOR) { *err = "app needs a different ABI major"; goto out; }
    if (img->header.abi_minor > CPOS_ABI_MINOR) { *err = "app needs a newer OS"; goto out; }

    img->code_size = (code_ph.p_memsz + 3) & ~3u;
    if (alloc_code(f, &eh, img)) { *err = "not enough executable RAM"; goto out; }
    img->code = img->part[0];
    if (have_data && data_ph.p_memsz) {
        img->data_size = (data_ph.p_memsz + 3) & ~3u;
        img->data = heap_caps_calloc(1, img->data_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!img->data) { *err = "not enough RAM for app data"; goto out; }
    }

    tmp = malloc(1024);
    if (!tmp) { *err = "out of memory"; goto out; }

    /* code: file bytes, then zero the rest */
    for (int i = 0; i < img->nparts; i++)
        for (size_t o = 0; o < img->part_off[i + 1] - img->part_off[i]; o += 4) ((volatile uint32_t *)img->part[i])[o / 4] = 0;
    for (size_t o = 0; o < code_ph.p_filesz; o += 1024) {
        size_t n = code_ph.p_filesz - o < 1024 ? code_ph.p_filesz - o : 1024;
        if (rd(f, code_ph.p_offset + o, tmp, n)) { *err = "short read (code)"; goto out; }
        write_code(img, o, tmp, n);
    }
    if (img->data) {
        for (size_t o = 0; o < data_ph.p_filesz; o += 1024) {
            size_t n = data_ph.p_filesz - o < 1024 ? data_ph.p_filesz - o : 1024;
            if (rd(f, data_ph.p_offset + o, tmp, n)) { *err = "short read (data)"; goto out; }
            memcpy((uint8_t *)img->data + o, tmp, n);
        }
    }

    /* relocations: only absolute 32-bit words matter */
    uint32_t data_off = img->data ? (uint32_t)img->data - data_ph.p_vaddr : 0;
    uint32_t nreloc = 0;
    uint32_t rbuf[255 * 3]; /* 255 entries of 12 bytes */
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf32_Shdr_t s;
        if (rd(f, eh.e_shoff + i * sizeof(s), &s, sizeof(s))) { *err = "cannot read sections"; goto out; }
        if (s.sh_type != SHT_RELA) continue;
        /* relocation tables are read in blocks: one seek per entry would crawl on an SD card */
        for (uint32_t base = 0; base < s.sh_size; base += sizeof(rbuf)) {
            uint32_t n = s.sh_size - base < sizeof(rbuf) ? s.sh_size - base : sizeof(rbuf);
            if (rd(f, s.sh_offset + base, rbuf, n)) { *err = "short read (reloc)"; goto out; }
            for (uint32_t o = 0; o + sizeof(Elf32_Rela_t) <= n; o += sizeof(Elf32_Rela_t)) {
                Elf32_Rela_t r;
                memcpy(&r, (uint8_t *)rbuf + o, sizeof(r));
                if ((r.r_info & 0xFF) != R_XTENSA_32) continue;
                uint32_t where = r.r_offset;
                uint32_t *p;
                if (where >= code_ph.p_vaddr && where + 4 <= code_ph.p_vaddr + code_ph.p_memsz)
                    p = (uint32_t *)code_at(img, where - code_ph.p_vaddr);
                else if (img->data && where >= data_ph.p_vaddr && where + 4 <= data_ph.p_vaddr + data_ph.p_memsz)
                    p = (uint32_t *)((uint8_t *)img->data + (where - data_ph.p_vaddr));
                else continue; /* debug sections etc. */
                uint32_t v = *(volatile uint32_t *)p;
                if (v >= code_ph.p_vaddr && v < code_ph.p_vaddr + code_ph.p_memsz + 4) v = (uint32_t)code_at(img, v - code_ph.p_vaddr);
                else if (img->data && v >= data_ph.p_vaddr && v < data_ph.p_vaddr + data_ph.p_memsz + 4) v += data_off;
                else continue; /* not an address into the image */
                *(volatile uint32_t *)p = v;
                nreloc++;
            }
        }
    }
    __asm__ volatile("memw\n\tisync\n\t" ::: "memory");

    /* manifest again, now relocated (entry pointer) */
    uint32_t hdr_link = sh.sh_addr;
    const cpos_app_header_t *hp = (const cpos_app_header_t *)((uint8_t *)img->data + (hdr_link - data_ph.p_vaddr));
    if (!img->data || hdr_link < data_ph.p_vaddr || hdr_link + sizeof(*hp) > data_ph.p_vaddr + data_ph.p_memsz) {
        *err = ".cpos_app must live in the data segment";
        goto out;
    }
    img->header = *hp;
    img->entry = hp->entry;
    if (part_of(img, (uint32_t)img->entry) < 0) {
        *err = "entry point outside code";
        goto out;
    }
    ESP_LOGI(TAG, "%s v%s: code %uB in %d part(s) data %uB, %u relocs, entry %p", img->header.name, img->header.version,
             (unsigned)img->code_size, img->nparts, (unsigned)img->data_size, (unsigned)nreloc, img->entry);
    rc = 0;
out:
    free(tmp);
    fclose(f);
    if (rc) cpos_unload(img);
    return rc;
}

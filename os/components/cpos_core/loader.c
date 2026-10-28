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
static void write_code(void *code, size_t off, const void *src, size_t n) {
    volatile uint32_t *d = (volatile uint32_t *)((uint8_t *)code + off);
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        size_t m = n - i < 4 ? n - i : 4;
        memcpy(&w, s + i, m);
        *d++ = w;
    }
}

void cpos_unload(cpos_image_t *img) {
    if (img->code) heap_caps_free(img->code);
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
    img->code = heap_caps_malloc(img->code_size + 4, MALLOC_CAP_EXEC);
    if (!img->code) { *err = "not enough executable RAM"; goto out; }
    if (have_data && data_ph.p_memsz) {
        img->data_size = (data_ph.p_memsz + 3) & ~3u;
        img->data = heap_caps_calloc(1, img->data_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!img->data) { *err = "not enough RAM for app data"; goto out; }
    }

    tmp = malloc(1024);
    if (!tmp) { *err = "out of memory"; goto out; }

    /* code: file bytes, then zero the rest */
    for (size_t o = 0; o < img->code_size; o += 4) ((volatile uint32_t *)img->code)[o / 4] = 0;
    for (size_t o = 0; o < code_ph.p_filesz; o += 1024) {
        size_t n = code_ph.p_filesz - o < 1024 ? code_ph.p_filesz - o : 1024;
        if (rd(f, code_ph.p_offset + o, tmp, n)) { *err = "short read (code)"; goto out; }
        write_code(img->code, o, tmp, n);
    }
    if (img->data) {
        for (size_t o = 0; o < data_ph.p_filesz; o += 1024) {
            size_t n = data_ph.p_filesz - o < 1024 ? data_ph.p_filesz - o : 1024;
            if (rd(f, data_ph.p_offset + o, tmp, n)) { *err = "short read (data)"; goto out; }
            memcpy((uint8_t *)img->data + o, tmp, n);
        }
    }

    /* relocations: only absolute 32-bit words matter */
    uint32_t code_off = (uint32_t)img->code - code_ph.p_vaddr;
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
                    p = (uint32_t *)((uint8_t *)img->code + (where - code_ph.p_vaddr));
                else if (img->data && where >= data_ph.p_vaddr && where + 4 <= data_ph.p_vaddr + data_ph.p_memsz)
                    p = (uint32_t *)((uint8_t *)img->data + (where - data_ph.p_vaddr));
                else continue; /* debug sections etc. */
                uint32_t v = *(volatile uint32_t *)p;
                if (v >= code_ph.p_vaddr && v < code_ph.p_vaddr + code_ph.p_memsz + 4) v += code_off;
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
    if ((uint32_t)img->entry < (uint32_t)img->code || (uint32_t)img->entry >= (uint32_t)img->code + img->code_size) {
        *err = "entry point outside code";
        goto out;
    }
    ESP_LOGI(TAG, "%s v%s: code %uB data %uB, %u relocs, entry %p", img->header.name, img->header.version,
             (unsigned)img->code_size, (unsigned)img->data_size, (unsigned)nreloc, img->entry);
    rc = 0;
out:
    free(tmp);
    fclose(f);
    if (rc) cpos_unload(img);
    return rc;
}

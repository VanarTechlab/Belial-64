/*
 * sdl_n64.c - capa SDL 1.2 minima para DevilutionX en Nintendo 64 (libdragon)
 * Proyecto Belial 64 (antes Diablo N64) - Vanar / VanarTechlab. Modelo: sdl_nds.c de Hellspawn.
 *
 * Fase 1: video 8 bpp convertido a 16 bpp por la CPU, mando N64 como teclado
 * (igual que el RG99), archivos con fopen, hilos cooperativos del kernel de
 * libdragon y registro de mensajes en pantalla para depurar.
 */
#define _GNU_SOURCE
#include <libdragon.h>
#include <fmv.h>
#include <yuv.h>
#include <video.h>
#include <mpeg1.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>
#include <backtrace.h>

#ifndef N64_DEBUG
#define N64_DEBUG 1
#endif

/* ---------------- registro de mensajes (stdout/stderr) ---------------- */
#define LOG_LINES 24
#define LOG_COLS 78
static char log_buf[LOG_LINES][LOG_COLS + 1];
static int log_head, log_col;
static int log_overlay = 0; /* Z+L+R lo muestra */ /* antes N64_DEBUG */ /* lineas de registro sobre la imagen */

/* copia del registro en sd:/devilutionx/n64_log.txt: las lineas se guardan en un bufer
   y se escriben a la SD como mucho una vez por segundo (n64_log_flush, desde SDL_Flip y
   antes de cerrar). Se cuentan los fallos de escritura para verlos en pantalla. */
static int log_sd;
static char sd_buf[8192];
static int sd_used;
static unsigned sd_err, sd_lost;
static int sd_errno;
#define LOG_SD_PATH "sd:/devilutionx/n64_log.txt"
void n64_log_flush(void)
{
    static int busy;
    if (!log_sd || sd_used == 0 || busy)
        return;
    busy = 1;
    FILE *f = fopen(LOG_SD_PATH, "a");
    if (f) {
        if (fwrite(sd_buf, 1, (size_t)sd_used, f) != (size_t)sd_used) {
            sd_err++;
            sd_errno = errno;
        }
        if (fclose(f) != 0) {
            sd_err++;
            sd_errno = errno;
        }
    } else {
        sd_err++;
        sd_errno = errno;
    }
    sd_used = 0;
    busy = 0;
}
void n64_log(const char *fmt, ...);
/* antes de un abort() o un assert, volcar el registro a la SD (se enlaza con --wrap) */
void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void)
{
    n64_log_flush();
    __real_abort();
}
void __real___assert_func(const char *f, int l, const char *fn, const char *e) __attribute__((noreturn));
void __wrap___assert_func(const char *f, int l, const char *fn, const char *e)
{
    n64_log("ASSERT %s:%d %s: %s", f ? f : "?", l, fn ? fn : "?", e ? e : "?");
    n64_log_flush();
    __real___assert_func(f, l, fn, e);
}
/* f2v68: assertf() de libdragon (no pasa por __assert_func): anotar el mensaje y volcarlo a la SD */
void __real_debug_assert_func_f(const char *file, int line, const char *func, const char *failedexpr, const char *msg, ...) __attribute__((noreturn));
void __wrap_debug_assert_func_f(const char *file, int line, const char *func, const char *failedexpr, const char *msg, ...)
{
    char m[160] = "";
    if (msg) {
        va_list ap;
        va_start(ap, msg);
        vsnprintf(m, sizeof m, msg, ap);
        va_end(ap);
    }
    n64_log("ASSERTF %s:%d %s: %s | %s", file ? file : "?", line, func ? func : "?", failedexpr ? failedexpr : "?", m);
    n64_log_flush();
    __real_debug_assert_func_f(file, line, func, failedexpr, "%s", m);
}
static void log_putc(char c)
{
    if (log_sd) {
        if (sd_used < (int)sizeof sd_buf)
            sd_buf[sd_used++] = c;
        else
            sd_lost++;
    }
    if (c == '\r')
        return;
    if (c == '\n' || log_col >= LOG_COLS) {
        log_head = (log_head + 1) % LOG_LINES;
        log_buf[log_head][0] = 0;
        log_col = 0;
        if (c == '\n')
            return;
    }
    log_buf[log_head][log_col++] = (c >= 32 && c < 127) ? c : '?';
    log_buf[log_head][log_col] = 0;
}

static ssize_t log_write(void *cookie, const char *buf, size_t n)
{
    (void)cookie;
    for (size_t i = 0; i < n; i++)
        log_putc(buf[i]);
    return (ssize_t)n;
}

void n64_log(const char *fmt, ...)
{
    char tmp[800]; /* f2v41: 800; f2v35: antes 256 y la linea de estado salia cortada */
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(tmp, sizeof tmp, "[%lu] ", (unsigned long)get_ticks_ms());
    vsnprintf(tmp + n, sizeof tmp - n, fmt, ap);
    va_end(ap);
    for (char *p = tmp; *p; p++)
        log_putc(*p);
    log_putc('\n');
}

static void log_draw(surface_t *d, int first_line, int max_lines, int y0)
{
    int shown = 0;
    graphics_set_color(graphics_make_color(255, 255, 0, 255), graphics_make_color(0, 0, 0, 160));
    for (int i = LOG_LINES - 1 - first_line; i >= 0 && shown < max_lines; i--) {
        int idx = (log_head - i + LOG_LINES) % LOG_LINES;
        if (!log_buf[idx][0])
            continue;
        graphics_draw_text(d, 8, y0 + shown * 9, log_buf[idx]);
        shown++;
    }
}

/* pantalla de texto completa (antes del video del juego o tras un error) */
void n64_log_flush(void);
static void log_screen(const char *title)
{
    n64_log_flush();
    surface_t *d = display_get();
    graphics_fill_screen(d, graphics_make_color(0, 0, 50, 255));
    graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
    graphics_draw_text(d, 8, 8, title);
    log_draw(d, 0, LOG_LINES, 24);
    display_show(d);
}

static void redirect_std(void);
#define ENGINE_STACK (384 * 1024) /* pila del hilo del motor */
static unsigned stack_used_kib(void);

/* ---------------- errores ---------------- */
static char err_buf[256];
void SDL_SetError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, sizeof err_buf, fmt, ap);
    va_end(ap);
    n64_log("SDL: %s", err_buf);
}
char *SDL_GetError(void) { return err_buf; }
void SDL_ClearError(void) { err_buf[0] = 0; }
void SDL_Error(SDL_errorcode code) { SDL_SetError("SDL_Error %d", (int)code); }
char *SDL_getenv(const char *name) { return getenv(name); }

/* libdragon no trae access(): se emula con stat() */
int access(const char *path, int mode)
{
    struct stat st;
    (void)mode;
    if (stat(path, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

/* ---------------- memoria: avisar cuando malloc falla ----------------
   (se enlaza con -Wl,--wrap=malloc; operator new tambien pasa por aqui) */
void *__real_malloc(size_t n);
static size_t mem_low = (size_t)-1; /* minimo de memoria libre visto */
static unsigned mem_fail_n;
static int big_alloc_log; /* se activa al entrar en partida (ver SDL_Flip) */
void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    if (n >= 65536 && big_alloc_log) {
        static int in_log;
        if (!in_log) {
            in_log = 1;
            void *bt[8];
            int k = backtrace(bt, 8);
            heap_stats_t hs;
            sys_get_heap_stats(&hs);
            n64_log("malloc %u -> %s, libres %d KiB, pila %08lx %08lx %08lx %08lx %08lx %08lx", (unsigned)n, p ? "ok" : "FALLO",
                (hs.total - hs.used) / 1024,
                (unsigned long)(k > 2 ? (uintptr_t)bt[2] : 0), (unsigned long)(k > 3 ? (uintptr_t)bt[3] : 0),
                (unsigned long)(k > 4 ? (uintptr_t)bt[4] : 0), (unsigned long)(k > 5 ? (uintptr_t)bt[5] : 0),
                (unsigned long)(k > 6 ? (uintptr_t)bt[6] : 0), (unsigned long)(k > 7 ? (uintptr_t)bt[7] : 0));
            in_log = 0;
        }
    }
    if (!p && n) {
        heap_stats_t hs;
        sys_get_heap_stats(&hs);
        if (mem_fail_n++ < 8)
            n64_log("SIN MEMORIA: pedidos %u bytes, libres %d KiB", (unsigned)n, (hs.total - hs.used) / 1024);
    }
    return p;
}
/* realloc tambien (Lua reserva su memoria con realloc) */
void *__real_realloc(void *p, size_t n);
void *__wrap_realloc(void *p, size_t n)
{
    void *r = __real_realloc(p, n);
    if (!r && n) {
        heap_stats_t hs;
        sys_get_heap_stats(&hs);
        if (mem_fail_n++ < 8)
            n64_log("SIN MEMORIA (realloc): pedidos %u bytes, libres %d KiB", (unsigned)n, (hs.total - hs.used) / 1024);
    }
    return r;
}
int n64_heap_free_kib(void);
static int heap_free_kib(void);
int n64_heap_free_kib(void) { return heap_free_kib(); }
static int heap_free_kib(void)
{
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    int f = hs.total - hs.used;
    if ((size_t)f < mem_low)
        mem_low = (size_t)f;
    return f / 1024;
}


/* ---------------- CEL del nivel en la memoria del cartucho ----------------
   La consola NO puede escribir en la memoria del ED64 Plus (sonda: todas las escrituras
   se pierden), pero el propio cartucho si puede copiar de la SD a su memoria por DMA (prueba
   v4: 2 MB/s, datos correctos). Asi que el CEL ya convertido se prepara en el PC, se guarda en
   sd:/devilutionx/n64/ y aqui se copia cluster a cluster con cart_card_rd_cart().
   Al dibujar, los trozos se leen del cartucho a una cache pequena en RAM. */
#include <ioctl.h>
#include <fat.h>
extern int cart_card_rd_cart(uint32_t cart, uint32_t lba, uint32_t count);
#define CELS_CART 0x12000000u          /* misma zona que la prueba v4, que salio bien */
/* Cache de trozos en anillo (f2v20): los trozos se guardan seguidos en un buffer circular,
   cada uno ocupa solo lo que mide (la mayoria 512 bytes). Antes eran 256 huecos de 1280 bytes
   y no cabia lo que se ve en una pantalla (unos 490 trozos): casi todo era fallo. */
/* f2v22: el tamano del anillo se decide al dibujar por primera vez segun la memoria libre
   (antes 304 KiB fijos: una pantalla son ~490 trozos ~ 290 KiB, casi no cabia y al caminar
   se tiraba lo que aun se veia). */
static uint32_t CEL_ARENA = 0;
#define CEL_ARENA_MIN (256 * 1024)
#define CEL_ARENA_MAX (480 * 1024)
#define CEL_KEEP_FREE (340 * 1024) /* f2v33: antes 400 */     /* memoria que se deja libre para el resto del juego */
#define CEL_TAGS 4096                  /* f2v22: indexada por numero de trozo (pueblo 3547), sin choques */
static uint8_t *cel_cache;             /* segmento 2 del anillo (reservado al dibujar por primera vez) */
/* f2v24: el anillo puede tener dos trozos de memoria: el segmento 1 es lo que sobra del bloque
   de la pantalla (reservado al arrancar para 640x480; a 400x300 sobran ~187 KiB) y el 2 se
   reserva aparte. CEL_ARENA = tam1 + tam2; un trozo nunca cruza de un segmento a otro. */
static uint8_t *cel_seg1;
static uint32_t cel_seg1_size;
static uint32_t cel_seg2_size;
static inline uint8_t *cel_ptr(uint32_t phys)
{
    return phys < cel_seg1_size ? cel_seg1 + phys : cel_cache + (phys - cel_seg1_size);
}
/* coloca un trozo de 'need' bytes en la posicion absoluta *w (avanzandola si no cabe seguido) */
static inline uint32_t cel_place(uint32_t *w, uint32_t need)
{
    uint32_t phys = *w % CEL_ARENA;
    uint32_t end = phys < cel_seg1_size ? cel_seg1_size : CEL_ARENA;
    if (phys + need > end) {
        *w += end - phys;
        phys = end == CEL_ARENA ? 0 : end;
        if (phys < cel_seg1_size && need > cel_seg1_size) { /* el segmento 1 es demasiado pequeno */
            *w += cel_seg1_size - phys;
            phys = cel_seg1_size;
        }
    }
    return phys;
}
static uint32_t cel_tag[CEL_TAGS];     /* offset del trozo (0xFFFFFFFF = vacio) */
static uint32_t cel_at[CEL_TAGS];      /* posicion absoluta en el anillo cuando se guardo */
static uint32_t cel_w;                 /* posicion absoluta de escritura (da la vuelta sola) */
static uint8_t cel_big[4096 + 16] __attribute__((aligned(16)));
static uint32_t cel_big_tag = 0xFFFFFFFFu;
static unsigned cel_hits, cel_misses, cel_moves;
static void n64_sfx_init(void);
static unsigned sfx_loaded, sfx_load_ms, sfx_played; /* f2v48 */
static unsigned monc_hits, monc_misses; /* f2v41: cache de fotogramas de monstruos */
static int cache_tried;
/* bloques de ordenes del RDP (f2v32); se sueltan al cargar un nivel (f2v39) */
static rspq_block_t *world_blocks[2]; /* f2v51: uno por cada imagen (doble bufer) */
static int wbs_w[2], wbs_h[2], wbs_dw[2], wbs_dh[2];
static void *wbs_px[2];
static rspq_block_t *panel_block;
static int pb_idx = -1, pb_w, pb_h, pb_dw, pb_dh;
static void *pb_px;
static void rdp_blocks_free(void)
{
    if (world_blocks[0] || world_blocks[1] || panel_block)
        rspq_wait(); /* que el RDP no los este usando */
    for (int i = 0; i < 2; i++) {
        if (world_blocks[i]) {
            rspq_block_free(world_blocks[i]);
            world_blocks[i] = NULL;
        }
    }
    if (panel_block) {
        rspq_block_free(panel_block);
        panel_block = NULL;
    }
}
static void cel_cache_reset(void);
/* f2v38: al empezar a cargar un nivel se suelta el segmento 2 de la cache (se vuelve a pedir al
   dibujar el nivel nuevo, con la memoria que haya entonces) */
void n64_cel_cache_release(void)
{
    if (cel_cache) {
        free(cel_cache);
        cel_cache = NULL;
    }
    cel_seg2_size = 0;
    CEL_ARENA = cel_seg1_size;
    cache_tried = 0;
    cel_cache_reset();
    rdp_blocks_free();
    n64_log("cache de CEL y bloques del RDP soltados para cargar (libre %d KiB)", heap_free_kib());
}
/* f2v22: medidas de tiempo por fotograma (microsegundos acumulados) */
static unsigned cart_us_acc, sleep_us_acc, logic_us_acc, view_us_acc, blit_us_acc, floor_us_acc, content_us_acc;
static unsigned prof_acc[40]; /* f2v43: partes de la logica (ver diablo.cpp) */
void n64_prof_add(int which, unsigned us)
{
    switch (which) {
    case 0: logic_us_acc += us; break; /* game_loop (logica del juego) */
    case 1: view_us_acc += us; break;  /* DrawView (mundo: suelo, paredes, personajes) */
    case 2: blit_us_acc += us; break;  /* DrawAndBlit entero (vista + panel + cursor...) */
    case 3: floor_us_acc += us; break; /* DrawFloor (suelo) */
    case 4: content_us_acc += us; break; /* DrawTileContent (paredes, objetos, personajes) */
    default:
        if (which >= 5 && which < 40)
            prof_acc[which] += us;
        break;
    }
}
unsigned n64_us(void) { return (unsigned)TICKS_TO_US(get_ticks()); }
static FILE *prep_f;
static uint32_t prep_size;
static char prep_name[160];

/* Lectura del cartucho por DMA hacia RAM con cache: el DMA escribe la RAM sin pasar por la
   cache de la CPU, asi que hay que vaciar/invalidar esas lineas antes (libcart hace lo mismo).
   Sin esto la CPU leia datos viejos de la cache (fallo de f2v16). Todos los buffers son de
   16 bytes de alineacion y tienen sitio para redondear a 16. */
static void cart_read(void *buf, uint32_t addr, uint32_t len)
{
    uint32_t t0 = (uint32_t)get_ticks();
    data_cache_hit_writeback_invalidate(buf, (len + 15) & ~15u);
    dma_read(buf, addr, len);
    cart_us_acc += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - t0);
}

static void cel_cache_reset(void)
{
    for (int i = 0; i < CEL_TAGS; i++)
        cel_tag[i] = 0xFFFFFFFFu;
    cel_big_tag = 0xFFFFFFFFu;
}

/* nombre del archivo preparado: "levels\towndata\town.cel" -> sd:/devilutionx/n64/levels_towndata_town.cel.n64 */
int n64_prepared_cel_begin(const char *celPath, uint32_t *frameCount, uint32_t *tableSize, char *err, uint32_t errLen)
{
    char name[160];
    snprintf(name, sizeof name, "sd:/devilutionx/n64/%s.n64", celPath ? celPath : "?");
    for (char *q = name + strlen("sd:/devilutionx/n64/"); *q; q++)
        if (*q == '\\' || *q == '/')
            *q = '_';
    big_alloc_log = 1; /* desde aqui se anotan las reservas grandes de memoria */
    if (prep_f)
        fclose(prep_f);
    snprintf(prep_name, sizeof prep_name, "%s", name);
    prep_f = fopen(name, "rb");
    if (!prep_f) {
        snprintf(err, errLen, "Falta el CEL preparado para la N64:\n%s\nCopia la carpeta n64 a SD:\\devilutionx\\", name);
        return 0;
    }
    setvbuf(prep_f, NULL, _IONBF, 0);
    fseek(prep_f, 0, SEEK_END);
    prep_size = (uint32_t)ftell(prep_f);
    fseek(prep_f, 0, SEEK_SET);
    uint32_t count = 0;
    if (fread(&count, 4, 1, prep_f) != 1) {
        snprintf(err, errLen, "CEL preparado vacio: %s", name);
        return 0;
    }
    count = __builtin_bswap32(count); /* el archivo esta en little endian (formato del PC) */
    *frameCount = count;
    *tableSize = (count + 2) * 4;
    n64_log("CEL preparado: %s (%u KiB, %u trozos)", name, (unsigned)(prep_size / 1024), (unsigned)count);
    return 1;
}

/* copia por DMA del cartucho los clusters que cubren [from, to) del archivo abierto */
static int prep_copy_range(uint32_t from, uint32_t to, uint32_t cbytes)
{
    uint8_t one;
    for (uint32_t pos = (from / cbytes) * cbytes; pos < to && pos < prep_size; pos += cbytes) {
        int64_t sect = 0;
        fseek(prep_f, (long)pos, SEEK_SET);
        fread(&one, 1, 1, prep_f);
        ioctl(fileno(prep_f), IOFAT_GET_SECTOR, &sect);
        uint32_t left = prep_size - pos;
        uint32_t nsect = (left + 511) / 512;
        if (nsect > cbytes / 512)
            nsect = cbytes / 512;
        if (sect <= 0 || cart_card_rd_cart(CELS_CART + pos, (uint32_t)sect, nsect) != 0)
            return 0;
    }
    return 1;
}

static uint32_t fnv32(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811C9DC5u;
    while (n--) {
        h ^= *p++;
        h *= 0x01000193u;
    }
    return h;
}

int n64_prepared_cel_finish(void *table, uint32_t tableSize, char *err, uint32_t errLen)
{
    uint64_t t0 = get_ticks_ms();
    fseek(prep_f, 0, SEEK_SET);
    if (fread(table, 1, tableSize, prep_f) != tableSize) {
        snprintf(err, errLen, "no se pudo leer la tabla del CEL preparado");
        return 0;
    }
    int32_t csize = 0;
    uint8_t one;
    fseek(prep_f, 0, SEEK_SET);
    fread(&one, 1, 1, prep_f);
    ioctl(fileno(prep_f), IOFAT_GET_CLUSTER_SIZE, &csize);
    if (csize <= 0)
        csize = 1;
    const uint32_t cbytes = (uint32_t)csize * 512u;
    if (!prep_copy_range(0, prep_size, cbytes)) {
        snprintf(err, errLen, "error al copiar el CEL a la memoria del cartucho");
        fclose(prep_f);
        prep_f = NULL;
        return 0;
    }
    uint64_t t_copy = get_ticks_ms() - t0;

    /* comprobar cada bloque de 32 KiB con la suma preparada en el PC (archivo .sum) y volver a
       copiar los que no coincidan: el DMA del ED64 Plus a veces deja bloques mal (f2v15) */
    char sname[176];
    snprintf(sname, sizeof sname, "%s", prep_name);
    strncat(sname, ".sum", sizeof sname - strlen(sname) - 1);
    FILE *sf = fopen(sname, "rb");
    unsigned bad_first = 0, retries = 0, still_bad = 0;
    if (!sf) {
        n64_log("AVISO: falta %s, no se puede comprobar el CEL", sname);
    } else {
        const uint32_t nblocks = (prep_size + 32767) / 32768;
        uint8_t *blk = (uint8_t *)memalign(16, 32768);
        uint32_t *sums = (uint32_t *)malloc(nblocks * 4);
        if (blk && sums && fread(sums, 4, nblocks, sf) == nblocks) {
            for (uint32_t i = 0; i < nblocks; i++) {
                uint32_t pos = i * 32768u;
                uint32_t n = prep_size - pos < 32768u ? prep_size - pos : 32768u;
                uint32_t want = __builtin_bswap32(sums[i]);
                int ok = 0;
                for (int attempt = 0; attempt < 6 && !ok; attempt++) {
                    cart_read(blk, CELS_CART + pos, (n + 7) & ~7u);
                    ok = fnv32(blk, n) == want;
                    if (!ok) {
                        if (attempt == 0)
                            bad_first++;
                        retries++;
                        prep_copy_range(pos, pos + n, cbytes);
                    }
                }
                if (!ok)
                    still_bad++;
            }
        } else {
            n64_log("AVISO: no se pudo leer %s", sname);
        }
        free(blk);
        free(sums);
        fclose(sf);
    }
    fclose(prep_f);
    prep_f = NULL;
    cel_cache_reset();
    n64_log("CEL al cartucho por DMA: %u KiB, copia %u ms, total %u ms, cluster %u KiB; bloques mal %u, reintentos %u, sin arreglar %u",
        (unsigned)(prep_size / 1024), (unsigned)t_copy, (unsigned)(get_ticks_ms() - t0), (unsigned)(cbytes / 1024),
        bad_first, retries, still_bad);
    n64_log_flush();
    if (still_bad) {
        snprintf(err, errLen, "El CEL no se pudo copiar bien a la memoria del cartucho (%u bloques mal)", still_bad);
        return 0;
    }
    return 1;
}

/* f2v22: el motor pasa tambien el numero de trozo; la tabla se indexa por el (sin choques).
   Ademas, si un trozo que se usa esta en la mitad vieja del anillo (pronto se pisara), se
   copia de RAM a RAM al principio: asi lo que se ve en pantalla no se tira al caminar
   (copiar 600 bytes en RAM es mucho mas barato que volver a leerlos del cartucho). */
const uint8_t *n64_cel_frame(uint32_t frame, uint32_t offset, uint32_t size)
{
    /* el segmento 2 de la cache se reserva al dibujar por primera vez (de cada nivel, f2v38) */
    if (!cache_tried) {
        cache_tried = 1;
        int fk = heap_free_kib();
        uint32_t want = fk * 1024 > CEL_KEEP_FREE ? (uint32_t)fk * 1024u - CEL_KEEP_FREE : 0;
        uint32_t total_min = CEL_ARENA_MIN > cel_seg1_size ? CEL_ARENA_MIN - cel_seg1_size : 0;
        if (want + cel_seg1_size > CEL_ARENA_MAX)
            want = CEL_ARENA_MAX > cel_seg1_size ? CEL_ARENA_MAX - cel_seg1_size : 0;
        if (want < total_min)
            want = total_min;
        want &= ~16383u;
        while (!cel_cache && want >= 64 * 1024) {
            cel_cache = (uint8_t *)memalign(16, want);
            if (!cel_cache)
                want -= 32 * 1024;
        }
        cel_seg2_size = cel_cache ? want : 0;
        CEL_ARENA = cel_seg1_size + cel_seg2_size;
        cel_cache_reset();
        n64_log("cache de CEL en anillo: %u KiB (%u sobrantes de la pantalla + %u aparte; libre antes %d KiB, despues %d KiB)",
            (unsigned)(CEL_ARENA / 1024), (unsigned)(cel_seg1_size / 1024), (unsigned)(cel_seg2_size / 1024), fk, heap_free_kib());
    }
    /* el cartucho se lee en bloques alineados a 8 bytes */
    uint32_t base = offset & ~7u;
    uint32_t skip = offset - base;
    uint32_t len = (skip + size + 7) & ~7u;
    if (CEL_ARENA < 8192 || len > 4096) {
        if (cel_big_tag != offset) {
            if (len > sizeof cel_big - 16)
                len = sizeof cel_big - 16;
            cart_read(cel_big, CELS_CART + base, len);
            cel_big_tag = offset;
            cel_misses++;
        }
        return cel_big + skip;
    }
    const uint32_t need = (len + 15) & ~15u;
    uint32_t t = frame < CEL_TAGS ? frame : (((offset >> 3) * 2654435761u) >> 20);
    if (cel_tag[t] == offset) {
        uint32_t age = cel_w - cel_at[t];
        if (age <= CEL_ARENA - need) {
            cel_hits++;
            if (age < CEL_ARENA / 2)
                return cel_ptr(cel_at[t] % CEL_ARENA) + skip;
            /* mitad vieja: copiarlo al principio del anillo */
            uint32_t w = cel_w;
            uint32_t phys = cel_place(&w, need);
            if (w - cel_at[t] <= CEL_ARENA - need) { /* sigue entero tras saltar */
                uint8_t *dst = cel_ptr(phys);
                memcpy(dst, cel_ptr(cel_at[t] % CEL_ARENA), need);
                cel_at[t] = w;
                cel_w = w + need;
                cel_moves++;
                return dst + skip;
            }
            cel_hits--; /* el salto al inicio lo pisaria: se lee como un fallo */
        }
    }
    /* fallo: guardar al final del anillo (sin partir el trozo) */
    uint32_t phys = cel_place(&cel_w, need);
    uint8_t *buf = cel_ptr(phys);
    cart_read(buf, CELS_CART + base, len);
    cel_tag[t] = offset;
    cel_at[t] = cel_w;
    cel_w += need;
    cel_misses++;
    return buf + skip;
}

/* ---------------- musica (f2v23) ----------------
   El WAV de la musica (del diabdat de la SD) se copia entero a la memoria del cartucho con el
   DMA del propio cartucho (como el CEL) y se reproduce desde ahi: el mezclador de libdragon
   (RSP) pide trozos y los leemos por PI. Los WAV son little endian: se dan la vuelta a 16 bits. */
/* f2v24: dos huecos en el cartucho para no volver a copiar al volver al pueblo:
   hueco 0 = musica del pueblo, detras del CEL (0x12000000, maximo ~4 MB), hasta los 64 MB;
   hueco 1 = el resto (menu y mazmorras), detras de la ROM (2,2 MB) hasta el CEL. */
static const uint32_t music_slot_base[3] = { 0x12400000u, 0x10400000u, 0x12800000u }; /* f2v53: hueco 2 = menu */
static const uint32_t music_slot_end[3] = { 0x12800000u, 0x10800000u, 0x12F00000u }; /* f2v74: 0x12F00000.. traduccion */ /* f2v48: 0x10800000..0x12000000 es de los sonidos */ /* f2v41: 0x13000000.. es de los monstruos */ /* f2v33: los ultimos 2 MB son la zona de paso de la lectura rapida */
static char music_slot_name[3][64];
static uint32_t music_slot_off[3], music_slot_len[3], music_slot_rate[3];
static int music_slot_ch[3], music_slot_bits[3];
/* f2v37: musica preparada en el PC en IMA ADPCM mono (4 bits por muestra: 4 veces menos que PCM
   mono, 8 menos que el estereo original) -> se copia al cartucho mucho mas rapido. */
static int music_slot_ima[3];
static uint32_t music_slot_align[3], music_slot_spb[3], music_slot_samples[3];
static int music_ima;
static uint32_t ima_align, ima_spb, ima_blk = 0xFFFFFFFFu;
static uint8_t ima_raw[2048 + 16] __attribute__((aligned(16)));
static int16_t ima_dec[4096];
static const int8_t ima_index_tab[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t ima_step_tab[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767 };
#define MUSIC_CH 0
static int audio_ready;
static waveform_t music_wave;
static uint32_t music_pcm_cart;
static uint32_t music_pcm_cart_fwd(void) { return music_pcm_cart; }        /* direccion en el cartucho del primer sample */
static int music_bps;                  /* log2 de bytes por muestra (todas las voces) */
static int music_bits;
static char music_cur[64];
static int music_playing;
static float music_gain = 1.0f;
static uint8_t music_bounce[SAMPLEBUFFER_MARGIN_UNITS * 4 + 16] __attribute__((aligned(16)));

static void music_fix(uint8_t *c, int bytes)
{
    if (music_bits == 16) {
        uint16_t *p = (uint16_t *)c;
        for (int i = 0; i < bytes / 2; i++)
            p[i] = __builtin_bswap16(p[i]);
    } else {
        for (int i = 0; i < bytes; i++)
            c[i] ^= 0x80; /* WAV de 8 bits sin signo -> con signo */
    }
}

static uint32_t music_pcm_cart_fwd(void);
static void ima_decode_block(uint32_t blk)
{
    cart_read(ima_raw, music_pcm_cart_fwd() + blk * ima_align, (ima_align + 7) & ~7u);
    int pred = (int16_t)(ima_raw[0] | (ima_raw[1] << 8));
    int idx = ima_raw[2];
    if (idx > 88)
        idx = 88;
    uint32_t n = 0;
    ima_dec[n++] = (int16_t)pred;
    for (uint32_t i = 4; i < ima_align && n < ima_spb; i++) {
        uint8_t b = ima_raw[i];
        for (int h = 0; h < 2 && n < ima_spb; h++) {
            int nib = h ? (b >> 4) : (b & 15);
            int step = ima_step_tab[idx];
            int diff = ((2 * (nib & 7) + 1) * step) >> 3; /* como el decodificador de ffmpeg (el que codifica) */
            pred += (nib & 8) ? -diff : diff;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            idx += ima_index_tab[nib];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            ima_dec[n++] = (int16_t)pred;
        }
    }
    while (n < ima_spb)
        ima_dec[n++] = (int16_t)pred;
    ima_blk = blk;
}

static unsigned music_read_us, sfx_read_us; /* f2v51 */
static void music_read2(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking);
static void music_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking)
{
    uint32_t t0 = (uint32_t)get_ticks();
    music_read2(ctx, sbuf, wpos, wlen, seeking);
    music_read_us += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - t0);
}
static void music_read2(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking)
{
    (void)ctx; (void)seeking;
    if (music_ima) {
        while (wlen > 0) {
            int n = wlen < SAMPLEBUFFER_MARGIN_UNITS ? wlen : SAMPLEBUFFER_MARGIN_UNITS;
            int16_t *out = (int16_t *)samplebuffer_append(sbuf, n);
            /* f2v51: una division por trozo (antes una por muestra) */
            uint32_t sidx = (uint32_t)wpos;
            uint32_t blk = sidx / ima_spb;
            uint32_t off = sidx - blk * ima_spb;
            for (int i = 0; i < n;) {
                if (blk != ima_blk)
                    ima_decode_block(blk);
                uint32_t take = ima_spb - off;
                if (take > (uint32_t)(n - i))
                    take = (uint32_t)(n - i);
                memcpy(out + i, ima_dec + off, take * sizeof(int16_t));
                i += (int)take;
                off += take;
                if (off >= ima_spb) {
                    off = 0;
                    blk++;
                }
            }
            wlen -= n;
            wpos += n;
        }
        return;
    }
    while (wlen > 0) {
        int n = wlen < SAMPLEBUFFER_MARGIN_UNITS ? wlen : SAMPLEBUFFER_MARGIN_UNITS;
        uint8_t *ram = (uint8_t *)samplebuffer_append(sbuf, n);
        int bytes = n << music_bps;
        uint32_t pi = music_pcm_cart + ((uint32_t)wpos << music_bps);
        if ((((uint32_t)ram ^ pi) & 1) == 0) {
            uint8_t *c = (uint8_t *)CachedAddr(ram);
            uintptr_t a0 = (uintptr_t)c & ~15u;
            data_cache_hit_writeback_invalidate((void *)a0, (((uintptr_t)c + bytes + 15) & ~15u) - a0);
            dma_read(c, pi, bytes);
            music_fix(c, bytes);
            data_cache_hit_writeback((void *)a0, (((uintptr_t)c + bytes + 15) & ~15u) - a0);
        } else {
            uint32_t pa = pi & ~1u;
            cart_read(music_bounce, pa, ((bytes + 1 + 7) & ~7u));
            music_fix(music_bounce + (pi - pa), bytes);
            memcpy(ram, music_bounce + (pi - pa), bytes);
        }
        wlen -= n;
        wpos += n;
    }
}

void n64_audio_pump(void);
void n64_audio_pump(void)
{
    if (audio_ready)
        mixer_try_play();
}

/* f2v25: las lecturas largas de la SD (menus, carga de graficos) bloqueaban la musica.
   fread se envuelve (--wrap=fread): se lee en trozos de 4 KiB (~28 ms a 145 KB/s) y entre
   trozo y trozo se rellena el sonido. */
size_t __real_fread(void *ptr, size_t size, size_t n, FILE *f);
/* f2v33: lectura rapida de la SD. Leer por libcart da ~145 KB/s; en cambio el propio cartucho copia
   de la SD a su memoria a ~1,5 MB/s (como el CEL y la musica). Para lecturas grandes se copian los
   clusters que hacen falta a una zona de paso del cartucho y de ahi a la RAM por PI (~5 MB/s). */
#define FAST_CART 0x13E00000u          /* zona de paso: 2 MB al final de la memoria del cartucho */
#define FAST_MAX (2u * 1024u * 1024u - 64u * 1024u)
static unsigned fast_reads, fast_kib, fast_fail;
/* f2v36: lectura de sectores de la SD mas rapida para TODO (abrir archivos, carpetas, lecturas
   pequenas). libcart en el ED64 lee cada sector byte a byte con la CPU (~145 KB/s). Aqui se envuelve
   cart_card_rd_dram: el cartucho copia los sectores a una zona suya (DMA propio) y de ahi pasan a
   la RAM por PI. Al arrancar se comprueba con unos sectores que da lo mismo que la forma normal. */
#define DISK_CART 0x13FF0000u          /* 64 KiB al final de la memoria del cartucho */
static int disk_fast_ok = 0;
static unsigned disk_fast_sect, disk_slow_sect;
int __real_cart_card_rd_dram(void *dram, uint32_t lba, uint32_t count);
static int disk_rd_fast(void *dram, uint32_t lba, uint32_t count)
{
    uint8_t *d = (uint8_t *)dram;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        if (cart_card_rd_cart(DISK_CART, lba, n) != 0)
            return -1;
        uintptr_t a0 = (uintptr_t)d & ~15u;
        data_cache_hit_writeback_invalidate((void *)a0, (((uintptr_t)d + n * 512u + 15) & ~15u) - a0);
        dma_read(d, DISK_CART, n * 512u);
        d += n * 512u;
        lba += n;
        count -= n;
    }
    return 0;
}
int __wrap_cart_card_rd_dram(void *dram, uint32_t lba, uint32_t count)
{
    if (disk_fast_ok && (((uintptr_t)dram) & 1) == 0) {
        if (disk_rd_fast(dram, lba, count) == 0) {
            disk_fast_sect += count;
            return 0;
        }
    }
    disk_slow_sect += count;
    return __real_cart_card_rd_dram(dram, lba, count);
}
/* se llama una vez montada la SD */
void n64_disk_fast_test(void);
void n64_disk_fast_test(void)
{
    static uint8_t a[4 * 512] __attribute__((aligned(16)));
    static uint8_t b[4 * 512] __attribute__((aligned(16)));
    static const uint32_t lbas[3] = { 0, 1, 2048 };
    int ok = 1;
    uint64_t t0 = get_ticks_ms();
    for (int i = 0; i < 3 && ok; i++) {
        memset(a, 0x11, sizeof a);
        memset(b, 0x22, sizeof b);
        if (__real_cart_card_rd_dram(a, lbas[i], 4) != 0 || disk_rd_fast(b, lbas[i], 4) != 0 || memcmp(a, b, sizeof a) != 0)
            ok = 0;
    }
    disk_fast_ok = ok;
    n64_log("SD por DMA del cartucho: %s (prueba %u ms)", ok ? "SI" : "NO (se lee como antes)", (unsigned)(get_ticks_ms() - t0));
}

/* deja el FILE de stdio leyendo desde 'at' despues de mover el descriptor por debajo */
static void stdio_resync(FILE *f, int fd, long at)
{
    lseek(fd, at, SEEK_SET);
    f->_r = 0;
    f->_p = f->_bf._base;
    f->_flags &= ~(__SOFF | __SEOF);
}

static size_t fast_fread(uint8_t *dst, size_t len, FILE *f)
{
    /* f2v34: en f2v33 se usaba fread de 1 byte para saber el sector de cada cluster, pero con el
       bufer de stdio la posicion real del archivo ya iba 1 KiB mas alla y el sector salia mal
       (datos basura, pantalla azul). Ahora se usa el descriptor directamente (sin bufer) y ademas
       se comprueba el principio y el final de lo copiado contra la SD. */
    long pos = ftell(f);
    if (pos < 0)
        return 0;
    const int fd = fileno(f);
    int32_t csize = 0;
    if (fd < 0 || ioctl(fd, IOFAT_GET_CLUSTER_SIZE, &csize) != 0 || csize <= 0)
        return 0;
    const uint32_t cbytes = (uint32_t)csize * 512u;
    if (cbytes > 64u * 1024u)
        return 0;
    long fsz = lseek(fd, 0, SEEK_END);
    if (fsz <= pos) {
        stdio_resync(f, fd, pos);
        return 0;
    }
    if ((long)len > fsz - pos)
        len = (size_t)(fsz - pos);
    const uint32_t first = (uint32_t)pos / cbytes * cbytes;
    const uint32_t end = (uint32_t)pos + (uint32_t)len;
    if (end - first > FAST_MAX) {
        stdio_resync(f, fd, pos);
        return 0;
    }
    uint8_t one;
    for (uint32_t c = first; c < end; c += cbytes) {
        int64_t sect = 0;
        if (lseek(fd, (long)c, SEEK_SET) != (long)c || read(fd, &one, 1) != 1
            || ioctl(fd, IOFAT_GET_SECTOR, &sect) != 0 || sect <= 0) {
            stdio_resync(f, fd, pos);
            fast_fail++;
            return 0;
        }
        uint32_t left = (uint32_t)fsz - c;
        uint32_t nsect = (left + 511) / 512;
        if (nsect > cbytes / 512)
            nsect = cbytes / 512;
        if (cart_card_rd_cart(FAST_CART + (c - first), (uint32_t)sect, nsect) != 0) {
            stdio_resync(f, fd, pos);
            fast_fail++;
            return 0;
        }
        if (((c - first) / cbytes) % 4 == 3)
            n64_audio_pump();
    }
    /* de la zona de paso a la RAM (el DMA del PI necesita la misma paridad de direccion) */
    const uint32_t src = FAST_CART + ((uint32_t)pos - first);
    if ((((uint32_t)(uintptr_t)dst ^ src) & 1) == 0) {
        uintptr_t a0 = (uintptr_t)dst & ~15u;
        data_cache_hit_writeback_invalidate((void *)a0, (((uintptr_t)dst + len + 15) & ~15u) - a0);
        dma_read(dst, src, len);
    } else {
        static uint8_t bounce[4096 + 16] __attribute__((aligned(16)));
        size_t done = 0;
        while (done < len) {
            size_t n = len - done > 4096 ? 4096 : len - done;
            uint32_t s2 = src + (uint32_t)done;
            uint32_t sa = s2 & ~1u;
            cart_read(bounce, sa, (uint32_t)((n + 1 + 7) & ~7u));
            memcpy(dst + done, bounce + (s2 - sa), n);
            done += n;
        }
    }
    /* comprobacion: los primeros y los ultimos 64 bytes, leidos de la SD de la forma normal */
    {
        uint8_t chk[64];
        size_t n1 = len < 64 ? len : 64;
        int ok = lseek(fd, pos, SEEK_SET) == pos && read(fd, chk, n1) == (int)n1 && memcmp(chk, dst, n1) == 0;
        if (ok && len > 64) {
            long p2 = pos + (long)len - 64;
            ok = lseek(fd, p2, SEEK_SET) == p2 && read(fd, chk, 64) == 64 && memcmp(chk, dst + len - 64, 64) == 0;
        }
        if (!ok) {
            fast_fail++;
            if (fast_fail <= 5)
                n64_log("SD rapida: datos distintos (posicion %ld, %u bytes, cluster %u KiB); se lee normal",
                    pos, (unsigned)len, (unsigned)(cbytes / 1024));
            stdio_resync(f, fd, pos);
            return 0;
        }
    }
    stdio_resync(f, fd, pos + (long)len);
    fast_reads++;
    fast_kib += (unsigned)(len / 1024);
    return len;
}

/* f2v35: anotar en el registro las aperturas y lecturas lentas, para ver donde se va el tiempo */
static unsigned slow_logged;
FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    uint64_t t0 = get_ticks_ms();
    FILE *f = __real_fopen(path, mode);
    unsigned ms = (unsigned)(get_ticks_ms() - t0);
    if (ms >= 100 && slow_logged < 300) {
        slow_logged++;
        n64_log("lento: fopen %u ms %s", ms, path ? path : "?");
    }
    return f;
}

static size_t wrap_fread2(void *ptr, size_t size, size_t n, FILE *f);
size_t __wrap_fread(void *ptr, size_t size, size_t n, FILE *f)
{
    uint64_t t0 = get_ticks_ms();
    unsigned fr0 = fast_reads;
    size_t r = wrap_fread2(ptr, size, n, f);
    unsigned ms = (unsigned)(get_ticks_ms() - t0);
    if (ms >= 150 && slow_logged < 300) {
        slow_logged++;
        n64_log("lento: fread %u KiB en %u ms (%s)", (unsigned)(size * n / 1024), ms, fast_reads != fr0 ? "rapida" : "normal");
    }
    return r;
}

static size_t wrap_fread2(void *ptr, size_t size, size_t n, FILE *f)
{
    size_t total = size * n;
    if (size != 0 && total >= 16384) {
        size_t got = fast_fread((uint8_t *)ptr, total, f);
        n64_audio_pump();
        if (got == total)
            return n;
        if (got > 0) {
            /* fin del archivo: devolver lo que habia */
            return got / size;
        }
    }
    if (!audio_ready || size == 0 || total <= 4096)
        return __real_fread(ptr, size, n, f);
    uint8_t *p = (uint8_t *)ptr;
    size_t done = 0;
    while (done < total) {
        size_t chunk = total - done > 4096 ? 4096 : total - done;
        size_t r = __real_fread(p + done, 1, chunk, f);
        done += r;
        n64_audio_pump();
        if (r < chunk)
            break;
    }
    return done / size;
}

void n64_music_volume(int volume, int muted)
{
    /* volumen del motor en centesimas de dB: 0 = maximo, -1600 = minimo */
    float g = 1.0f;
    if (muted || volume <= -1600)
        g = 0.0f;
    else if (volume < 0)
        g = powf(10.0f, (float)volume / 2000.0f);
    music_gain = g;
    if (audio_ready && music_playing)
        mixer_ch_set_vol(MUSIC_CH, g, g);
}

void n64_music_stop(void)
{
    if (audio_ready && music_playing)
        mixer_ch_stop(MUSIC_CH);
    music_playing = 0;
}

/* copia [0, size) de un archivo abierto a la memoria del cartucho, cluster a cluster */
static int copy_file_to_cart(FILE *f, uint32_t size, uint32_t cart)
{
    int32_t csize = 0;
    uint8_t one;
    fseek(f, 0, SEEK_SET);
    fread(&one, 1, 1, f);
    ioctl(fileno(f), IOFAT_GET_CLUSTER_SIZE, &csize);
    if (csize <= 0)
        csize = 1;
    const uint32_t cbytes = (uint32_t)csize * 512u;
    for (uint32_t pos = 0; pos < size; pos += cbytes) {
        int64_t sect = 0;
        fseek(f, (long)pos, SEEK_SET);
        fread(&one, 1, 1, f);
        ioctl(fileno(f), IOFAT_GET_SECTOR, &sect);
        uint32_t left = size - pos;
        uint32_t nsect = (left + 511) / 512;
        if (nsect > cbytes / 512)
            nsect = cbytes / 512;
        if (sect <= 0 || cart_card_rd_cart(cart + pos, (uint32_t)sect, nsect) != 0)
            return 0;
        if ((pos / cbytes) % 16 == 15)
            n64_audio_pump();
    }
    return 1;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void n64_music_start(const char *path)
{
    if (!audio_ready || !path)
        return;
    if (music_playing && strcmp(path, music_cur) == 0)
        return;
    n64_music_stop();
    uint64_t t0 = get_ticks_ms();
    const int slot = strstr(path, "towne") ? 0 : (strstr(path, "dintro") ? 2 : 1);
    const uint32_t MUSIC_CART = music_slot_base[slot];
    uint32_t data_off, data_len, rate;
    int ch, bits;
    if (strcmp(music_slot_name[slot], path) == 0) {
        /* ya estaba copiada en el cartucho */
        data_off = music_slot_off[slot];
        data_len = music_slot_len[slot];
        rate = music_slot_rate[slot];
        ch = music_slot_ch[slot];
        bits = music_slot_bits[slot];
        music_ima = music_slot_ima[slot];
        ima_align = music_slot_align[slot];
        ima_spb = music_slot_spb[slot];
        n64_log("musica: %s ya estaba en el cartucho", path);
    } else {
    music_slot_name[slot][0] = 0;
    char name[128];
    /* f2v37: primero la musica preparada en el PC (sd:/devilutionx/n64/music/) */
    static const char *const dirs[] = { "sd:/devilutionx/n64/", "sd:/devilutionx/diabdat/", "sd:/devilutionx/spawn/" };
    FILE *f = NULL;
    for (int d = 0; d < 3 && !f; d++) {
        snprintf(name, sizeof name, "%s%s", dirs[d], path);
        for (char *q = name + strlen(dirs[d]); *q; q++)
            if (*q == '\\')
                *q = '/';
        f = fopen(name, "rb");
    }
    if (!f) {
        n64_log("musica: falta %s", path);
        return;
    }
    setvbuf(f, NULL, _IONBF, 0);
    fseek(f, 0, SEEK_END);
    uint32_t fsize = (uint32_t)ftell(f);
    uint8_t hdr[512];
    fseek(f, 0, SEEK_SET);
    int got = (int)fread(hdr, 1, sizeof hdr, f);
    data_off = 0; data_len = 0; rate = 0;
    int fmt = 0;
    uint32_t balign = 0, spb = 0, fact = 0;
    ch = 0; bits = 0;
    if (got >= 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0) {
        int p = 12;
        while (p + 8 <= got) {
            uint32_t clen = rd32le(hdr + p + 4);
            if (memcmp(hdr + p, "fmt ", 4) == 0 && p + 24 <= got) {
                fmt = rd16le(hdr + p + 8);
                ch = rd16le(hdr + p + 10);
                rate = rd32le(hdr + p + 12);
                bits = rd16le(hdr + p + 22);
                balign = rd16le(hdr + p + 20);
                if (clen >= 20 && p + 28 <= got)
                    spb = rd16le(hdr + p + 26);
            } else if (memcmp(hdr + p, "fact", 4) == 0 && p + 12 <= got) {
                fact = rd32le(hdr + p + 8);
            } else if (memcmp(hdr + p, "data", 4) == 0) {
                data_off = (uint32_t)p + 8;
                data_len = clen;
                break;
            }
            p += 8 + (int)((clen + 1) & ~1u);
        }
    }
    if (data_len > fsize - data_off)
        data_len = fsize - data_off;
    const int is_ima = fmt == 0x11 && ch == 1 && bits == 4 && balign >= 8 && balign <= 2048;
    if (is_ima && (spb == 0 || spb > 4096))
        spb = (balign - 4) * 2 + 1;
    if (!(is_ima || fmt == 1) || (ch != 1 && ch != 2) || (!is_ima && bits != 8 && bits != 16) || !data_off || !rate) {
        n64_log("musica: %s formato no soportado (fmt %d, %d voces, %d bits, %u Hz, datos en %u)", path, fmt, ch, bits,
            (unsigned)rate, (unsigned)data_off);
        fclose(f);
        return;
    }
    uint32_t total = data_off + data_len;
    if (total > music_slot_end[slot] - MUSIC_CART) {
        total = music_slot_end[slot] - MUSIC_CART;
        data_len = total - data_off;
    }
    int ok = copy_file_to_cart(f, total, MUSIC_CART);
    /* comprobacion: un trozo del principio y otro del final, cartucho contra SD */
    int same = 0;
    if (ok) {
        static uint8_t a[1024] __attribute__((aligned(16))), b[1024];
        uint32_t checks[2] = { data_off & ~7u, (total > 2048 ? total - 2048 : 0) & ~7u };
        same = 1;
        for (int i = 0; i < 2; i++) {
            cart_read(a, MUSIC_CART + checks[i], 1024);
            fseek(f, (long)checks[i], SEEK_SET);
            int r = (int)fread(b, 1, 1024, f);
            if (r <= 0 || memcmp(a, b, (size_t)r) != 0)
                same = 0;
        }
    }
    fclose(f);
    if (!ok || !same) {
        n64_log("musica: %s no se pudo copiar bien al cartucho (copia %d, comprobacion %d)", path, ok, same);
        return;
    }
    snprintf(music_slot_name[slot], sizeof music_slot_name[slot], "%s", path);
    music_slot_off[slot] = data_off;
    music_slot_len[slot] = data_len;
    music_slot_rate[slot] = rate;
    music_slot_ch[slot] = ch;
    music_slot_bits[slot] = bits;
    music_slot_ima[slot] = is_ima;
    music_slot_align[slot] = balign;
    music_slot_spb[slot] = spb;
    music_slot_samples[slot] = is_ima ? (fact ? fact : data_len / balign * spb) : 0;
    music_ima = is_ima;
    ima_align = balign;
    ima_spb = spb;
    n64_log("musica: %s (%u KiB, %d voces, %d bits, %u Hz) copiada en %u ms", path, (unsigned)(total / 1024), ch, bits,
        (unsigned)rate, (unsigned)(get_ticks_ms() - t0));
    n64_log_flush();
    }
    music_bits = bits;
    music_bps = (bits == 16 ? 1 : 0) + (ch == 2 ? 1 : 0);
    music_pcm_cart = MUSIC_CART + data_off;
    ima_blk = 0xFFFFFFFFu;
    memset(&music_wave, 0, sizeof music_wave);
    snprintf(music_cur, sizeof music_cur, "%s", path);
    music_wave.name = music_cur;
    music_wave.bits = (uint8_t)(music_ima ? 16 : bits);
    music_wave.channels = (uint8_t)ch;
    music_wave.format = WAVEFORM_FORMAT_PCM;
    music_wave.frequency = (float)rate;
    music_wave.len = music_ima ? (int)music_slot_samples[slot] : (int)(data_len >> music_bps);
    music_wave.loop_len = music_wave.len; /* en bucle, como en el juego */
    music_wave.read = music_read;
    mixer_ch_play(MUSIC_CH, &music_wave);
    mixer_ch_set_vol(MUSIC_CH, music_gain, music_gain);
    music_playing = 1;
}

/* ---------------- tiempo ---------------- */
static unsigned yield_cnt;
Uint32 SDL_GetTicks(void)
{
    if ((++yield_cnt & 31) == 0) {
        n64_audio_pump();
        kthread_yield();
    }
    return (Uint32)get_ticks_ms();
}

void SDL_Delay(Uint32 ms)
{
    uint32_t t0 = (uint32_t)get_ticks();
    uint64_t end = get_ticks_ms() + ms;
    do {
        n64_audio_pump();
        kthread_yield();
    } while (get_ticks_ms() < end);
    sleep_us_acc += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - t0);
}

/* ---------------- superficies 8 bpp ---------------- */
typedef struct {
    SDL_Surface s; /* debe ser el primer miembro */
    SDL_PixelFormat f;
    SDL_Palette p;
    SDL_Color c[256];
    void *own_pixels;
    size_t guard_bytes;
} N64Surface;

/* Bytes de guarda antes y despues de los pixeles de cada superficie: si el dibujado se sale
   de la imagen, se pisan y lo avisamos en el registro (f2v18). */
#define SURF_GUARD 512
#define SURF_GUARD_BYTE 0x5A
#define GUARD_MAX 48
static N64Surface *guard_list[GUARD_MAX];
static int guard_reported;
static void guard_add(N64Surface *n)
{
    for (int i = 0; i < GUARD_MAX; i++)
        if (!guard_list[i]) {
            guard_list[i] = n;
            return;
        }
}
static void guard_remove(N64Surface *n)
{
    for (int i = 0; i < GUARD_MAX; i++)
        if (guard_list[i] == n)
            guard_list[i] = NULL;
}
static int guard_check_one(N64Surface *n, const char *where)
{
    const uint8_t *base = (const uint8_t *)n->own_pixels;
    const uint8_t *after = base + SURF_GUARD + n->guard_bytes;
    int bad = 0, first = -1, last = -1;
    for (int i = 0; i < SURF_GUARD; i++)
        if (base[i] != SURF_GUARD_BYTE) { bad++; if (first < 0) first = i - SURF_GUARD; last = i - SURF_GUARD; }
    for (int i = 0; i < SURF_GUARD; i++)
        if (after[i] != SURF_GUARD_BYTE) { bad++; if (first < 0) first = (int)n->guard_bytes + i; last = (int)n->guard_bytes + i; }
    if (bad && guard_reported < 16) {
        guard_reported++;
        n64_log("GUARDA PISADA [%s]: superficie %dx%d pitch %d, %d bytes mal (desde %d hasta %d)", where,
            n->s.w, n->s.h, n->s.pitch, bad, first, last);
        /* reponer para ver si vuelve a pasar */
        memset((uint8_t *)base, SURF_GUARD_BYTE, SURF_GUARD);
        memset((uint8_t *)after, SURF_GUARD_BYTE, SURF_GUARD);
    }
    return bad;
}
static Uint8 *video_pixels;
/* f2v51: doble bufer. Con 400x300 o 480x270 caben dos imagenes en el bloque de 640x480: el motor
   dibuja en una mientras el RDP convierte la otra (antes la CPU esperaba ~5 ms al RDP). */
static Uint8 *video_pixels1, *video_pixels2;
static Uint8 *last_drawn_px;      /* ultima imagen completa que dibujo el motor */
static int world_drawn;           /* el motor dibujo la imagen entera en este fotograma */
static int rdp_sp_valid;
static rspq_syncpoint_t rdp_sp;
static Uint8 *video_block;
#define VIDEO_BLOCK (640u * 480u + 2u * SURF_GUARD)
static int video_w, video_h;
int n64_check_guards(const char *where)
{
    int bad = 0;
    if (video_pixels) {
        static N64Surface vfake;
        vfake.own_pixels = video_pixels - SURF_GUARD;
        vfake.guard_bytes = (size_t)video_w * (size_t)video_h;
        vfake.s.w = video_w; vfake.s.h = video_h; vfake.s.pitch = (Uint16)video_w;
        bad += guard_check_one(&vfake, where) ? 1 : 0;
        if (video_pixels2) {
            Uint8 *other = video_pixels == video_pixels2 ? video_pixels1 : video_pixels2;
            static N64Surface vfake2;
            vfake2 = vfake;
            vfake2.own_pixels = other - SURF_GUARD;
            bad += guard_check_one(&vfake2, where) ? 1 : 0;
        }
    }
    for (int i = 0; i < GUARD_MAX; i++)
        if (guard_list[i])
            bad += guard_check_one(guard_list[i], where) ? 1 : 0;
    return bad;
}

static SDL_Surface video_surface;
static SDL_PixelFormat video_format;
static SDL_Palette video_palette;
static SDL_Color video_colors[256];
static int video_ready;
static uint16_t pal16[256] __attribute__((aligned(16)));

static SDL_Surface *new_surface8(int w, int h, int pitch, void *pixels)
{
    N64Surface *n = (N64Surface *)calloc(1, sizeof(N64Surface));
    if (!n) {
        SDL_SetError("sin memoria (superficie %dx%d)", w, h);
        return NULL;
    }
    if (!pixels) {
        pitch = (w + 7) & ~7;
        size_t bytes = (size_t)pitch * (size_t)(h > 0 ? h : 1);
        uint8_t *base = (uint8_t *)malloc(bytes + 2 * SURF_GUARD);
        if (!base) {
            free(n);
            SDL_SetError("sin memoria (pixeles %dx%d)", w, h);
            return NULL;
        }
        memset(base, SURF_GUARD_BYTE, SURF_GUARD);
        memset(base + SURF_GUARD, 0, bytes);
        memset(base + SURF_GUARD + bytes, SURF_GUARD_BYTE, SURF_GUARD);
        pixels = base + SURF_GUARD;
        n->own_pixels = base;
        n->guard_bytes = bytes;
        guard_add(n);
    }
    n->p.ncolors = 256;
    n->p.colors = n->c;
    n->f.palette = &n->p;
    n->f.BitsPerPixel = 8;
    n->f.BytesPerPixel = 1;
    n->s.flags = SDL_SWSURFACE;
    n->s.format = &n->f;
    n->s.w = w;
    n->s.h = h;
    n->s.pitch = (Uint16)pitch;
    n->s.pixels = pixels;
    n->s.clip_rect.w = (Uint16)w;
    n->s.clip_rect.h = (Uint16)h;
    n->s.refcount = 1;
    return &n->s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    if (depth != 8) {
        SDL_SetError("capa N64: solo 8 bpp (pedido %d)", depth);
        return NULL;
    }
    if (w <= 0 || h <= 0) {
        SDL_SetError("tamano de superficie invalido");
        return NULL;
    }
    return new_surface8(w, h, 0, NULL);
}

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    if (depth != 8 || !pixels) {
        SDL_SetError("capa N64: solo 8 bpp con memoria propia (pedido %d)", depth);
        return NULL;
    }
    return new_surface8(w, h, pitch, pixels);
}

void SDL_FreeSurface(SDL_Surface *s)
{
    N64Surface *n;
    if (!s || s == &video_surface)
        return;
    if (--s->refcount > 0)
        return;
    n = (N64Surface *)s;
    if (n->own_pixels) {
        guard_check_one(n, "al liberar");
        guard_remove(n);
    }
    free(n->own_pixels);
    free(n);
}

SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat *fmt, Uint32 flags)
{
    SDL_Surface *d;
    (void)flags;
    if (!src || !fmt || src->format->BitsPerPixel != 8 || fmt->BitsPerPixel != 8) {
        SDL_SetError("capa N64: ConvertSurface solo de 8 a 8 bpp");
        return NULL;
    }
    d = new_surface8(src->w, src->h, 0, NULL);
    if (!d)
        return NULL;
    for (int y = 0; y < src->h; y++)
        memcpy((Uint8 *)d->pixels + y * d->pitch, (Uint8 *)src->pixels + y * src->pitch, (size_t)src->w);
    memcpy(d->format->palette->colors, src->format->palette->colors, 256 * sizeof(SDL_Color));
    d->flags |= src->flags & SDL_SRCCOLORKEY;
    d->format->colorkey = src->format->colorkey;
    return d;
}

int SDL_SetColorKey(SDL_Surface *s, Uint32 flag, Uint32 key)
{
    if (!s)
        return -1;
    if (flag & SDL_SRCCOLORKEY) {
        s->flags |= SDL_SRCCOLORKEY;
        s->format->colorkey = key;
    } else {
        s->flags &= ~SDL_SRCCOLORKEY;
    }
    return 0;
}

SDL_bool SDL_SetClipRect(SDL_Surface *s, const SDL_Rect *r)
{
    int x0, y0, x1, y1;
    if (!s)
        return SDL_FALSE;
    if (!r) {
        s->clip_rect.x = 0;
        s->clip_rect.y = 0;
        s->clip_rect.w = (Uint16)s->w;
        s->clip_rect.h = (Uint16)s->h;
        return SDL_TRUE;
    }
    x0 = r->x; y0 = r->y; x1 = r->x + r->w; y1 = r->y + r->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s->clip_rect.x = (Sint16)x0;
    s->clip_rect.y = (Sint16)y0;
    s->clip_rect.w = (Uint16)(x1 - x0);
    s->clip_rect.h = (Uint16)(y1 - y0);
    return SDL_TRUE;
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *r, Uint32 color)
{
    int x, y, w, h, c0x, c0y, c1x, c1y;
    if (!dst || dst->format->BitsPerPixel != 8) {
        SDL_SetError("capa N64: FillRect solo en 8 bpp");
        return -1;
    }
    if (r) { x = r->x; y = r->y; w = r->w; h = r->h; }
    else { x = 0; y = 0; w = dst->w; h = dst->h; }
    c0x = dst->clip_rect.x; c0y = dst->clip_rect.y;
    c1x = c0x + dst->clip_rect.w; c1y = c0y + dst->clip_rect.h;
    if (x < c0x) { w -= c0x - x; x = c0x; }
    if (y < c0y) { h -= c0y - y; y = c0y; }
    if (x + w > c1x) w = c1x - x;
    if (y + h > c1y) h = c1y - y;
    if (w <= 0 || h <= 0) {
        if (r) { r->w = 0; r->h = 0; }
        return 0;
    }
    for (int j = 0; j < h; j++)
        memset((Uint8 *)dst->pixels + (y + j) * dst->pitch + x, (int)(color & 0xFF), (size_t)w);
    if (r) { r->x = (Sint16)x; r->y = (Sint16)y; r->w = (Uint16)w; r->h = (Uint16)h; }
    return 0;
}

int SDL_UpperBlit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    int sx, sy, w, h, dx, dy;
    const SDL_Rect *c;
    if (!src || !dst) {
        SDL_SetError("Blit con superficie NULL");
        return -1;
    }
    if (src->format->BitsPerPixel != 8 || dst->format->BitsPerPixel != 8) {
        SDL_SetError("capa N64: Blit solo entre superficies de 8 bpp");
        return -1;
    }
    if (srcrect) { sx = srcrect->x; sy = srcrect->y; w = srcrect->w; h = srcrect->h; }
    else { sx = 0; sy = 0; w = src->w; h = src->h; }
    dx = dstrect ? dstrect->x : 0;
    dy = dstrect ? dstrect->y : 0;
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    c = &dst->clip_rect;
    if (dx < c->x) { int d = c->x - dx; sx += d; w -= d; dx = c->x; }
    if (dy < c->y) { int d = c->y - dy; sy += d; h -= d; dy = c->y; }
    if (dx + w > c->x + c->w) w = c->x + c->w - dx;
    if (dy + h > c->y + c->h) h = c->y + c->h - dy;
    if (w <= 0 || h <= 0) {
        if (dstrect) { dstrect->w = 0; dstrect->h = 0; }
        return 0;
    }
    if (src->flags & SDL_SRCCOLORKEY) {
        Uint8 key = (Uint8)src->format->colorkey;
        for (int j = 0; j < h; j++) {
            const Uint8 *s = (const Uint8 *)src->pixels + (sy + j) * src->pitch + sx;
            Uint8 *d = (Uint8 *)dst->pixels + (dy + j) * dst->pitch + dx;
            for (int i = 0; i < w; i++)
                if (s[i] != key)
                    d[i] = s[i];
        }
    } else {
        for (int j = 0; j < h; j++)
            memmove((Uint8 *)dst->pixels + (dy + j) * dst->pitch + dx,
                (const Uint8 *)src->pixels + (sy + j) * src->pitch + sx, (size_t)w);
    }
    if (dstrect) { dstrect->x = (Sint16)dx; dstrect->y = (Sint16)dy; dstrect->w = (Uint16)w; dstrect->h = (Uint16)h; }
    return 0;
}

/* escalado simple por vecino mas cercano (8 bpp) */
int SDL_SoftStretch(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    SDL_Rect sr = { 0, 0, (Uint16)src->w, (Uint16)src->h };
    SDL_Rect dr = { 0, 0, (Uint16)dst->w, (Uint16)dst->h };
    if (srcrect) sr = *srcrect;
    if (dstrect) dr = *dstrect;
    if (!sr.w || !sr.h || !dr.w || !dr.h)
        return 0;
    for (int y = 0; y < dr.h; y++) {
        const Uint8 *s = (const Uint8 *)src->pixels + (sr.y + y * sr.h / dr.h) * src->pitch + sr.x;
        Uint8 *d = (Uint8 *)dst->pixels + (dr.y + y) * dst->pitch + dr.x;
        for (int x = 0; x < dr.w; x++)
            d[x] = s[x * sr.w / dr.w];
    }
    return 0;
}

/* ---------------- video ---------------- */
/* Vista alejada (como en Hellspawn): el juego dibuja a GAME_W x GAME_H y el RDP lo
   reduce a la pantalla de 320x240 con suavizado. 400x300 = 0,8 de escala. */
#ifndef N64_GAME_W
#define N64_GAME_W 400
#define N64_GAME_H 300
#endif
SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags)
{
    (void)bpp; (void)flags;
    /* la N64 trabaja a 320x240: asi el motor no reserva una superficie intermedia para escalar */
    if (!((w == N64_GAME_W && h == N64_GAME_H) || (w == 480 && h == 270) || (w == 640 && h == 480))) {
        w = N64_GAME_W;
        h = N64_GAME_H;
    }
    if (video_ready && (w != video_w || h != video_h))
        video_ready = 0;
    if (!video_ready) {
        /* f2v24: un solo bloque reservado al arrancar con sitio para 640x480 (antes se liberaba y
           se volvia a pedir, y a 640x480 no habia un hueco seguido de 300 KiB: "sin memoria").
           Lo que sobra detras de la imagen lo usa la cache de CEL (segmento 1). */
        if (!video_block)
            video_block = (Uint8 *)memalign(64, VIDEO_BLOCK); /* f2v53: 64 para que el RDP pueda escribir en ella */
        if (!video_block) {
            SDL_SetError("sin memoria para la pantalla %dx%d", w, h);
            return NULL;
        }
        video_pixels = video_block + SURF_GUARD;
        memset(video_block, SURF_GUARD_BYTE, SURF_GUARD);
        memset(video_pixels + (size_t)w * (size_t)h, SURF_GUARD_BYTE, SURF_GUARD);
        {
            uint32_t used = ((uint32_t)(SURF_GUARD + w * h + SURF_GUARD) + 63u) & ~63u;
            if (rdp_sp_valid) {
                rspq_syncpoint_wait(rdp_sp);
                rdp_sp_valid = 0;
            }
            video_pixels1 = video_pixels;
            video_pixels2 = NULL;
            last_drawn_px = video_pixels;
            if (w <= 480 && 2u * used + 8192u <= VIDEO_BLOCK) {
                video_pixels2 = video_block + used + SURF_GUARD;
                memset(video_pixels2 - SURF_GUARD, SURF_GUARD_BYTE, SURF_GUARD);
                memset(video_pixels2 + (size_t)w * (size_t)h, SURF_GUARD_BYTE, SURF_GUARD);
                memset(video_pixels2, 0, (size_t)w * (size_t)h);
                used *= 2;
            }
            cel_seg1 = video_block + used;
            cel_seg1_size = (VIDEO_BLOCK - used) & ~15u;
            if (cel_seg1_size < 8192)
                cel_seg1_size = 0;
            CEL_ARENA = cel_seg1_size + cel_seg2_size;
            cel_cache_reset(); /* lo guardado ya no vale: el segmento 1 ha cambiado */
        }
        memset(video_pixels, 0, (size_t)w * (size_t)h);
        video_palette.ncolors = 256;
        video_palette.colors = video_colors;
        video_format.palette = &video_palette;
        video_format.BitsPerPixel = 8;
        video_format.BytesPerPixel = 1;
        /* f2v24: SDL_DOUBLEBUF hace que el motor dibuje directamente aqui (antes tenia otra
           superficie igual y la copiaba entera en cada fotograma) */
        video_surface.flags = SDL_SWSURFACE | SDL_HWPALETTE | SDL_FULLSCREEN | SDL_DOUBLEBUF;
        video_surface.format = &video_format;
        video_surface.w = w;
        video_surface.h = h;
        video_surface.pitch = (Uint16)w;
        video_surface.pixels = video_pixels;
        video_surface.clip_rect.x = 0;
        video_surface.clip_rect.y = 0;
        video_surface.clip_rect.w = (Uint16)w;
        video_surface.clip_rect.h = (Uint16)h;
        video_surface.refcount = 1;
        video_w = w;
        video_h = h;
        video_ready = 1;
        n64_log("video: superficie %dx%d 8 bpp", w, h);
    }
    return &video_surface;
}

SDL_Surface *SDL_GetVideoSurface(void) { return video_ready ? &video_surface : NULL; }

/* f2v23: tres tamanos a elegir en Ajustes > Graficos > Resolucion (desde el menu principal) */
static SDL_Rect mode_game = { 0, 0, N64_GAME_W, N64_GAME_H };
/* f2v29: 400x300 y panoramica 480x270 (16:9, la tele la estira); los menus van a 640x480 */
static SDL_Rect mode_wide = { 0, 0, 480, 270 };
static SDL_Rect *mode_list[4] = { &mode_game, &mode_wide, NULL, NULL };
SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags)
{
    (void)format; (void)flags;
    return mode_list;
}
int SDL_VideoModeOK(int w, int h, int bpp, Uint32 flags)
{
    (void)w; (void)h; (void)bpp; (void)flags;
    return 8;
}

int SDL_SetPalette(SDL_Surface *s, int flags, SDL_Color *colors, int first, int n)
{
    (void)flags;
    if (!s || !s->format || !s->format->palette || !colors)
        return 0;
    if (first < 0 || n < 0 || first + n > s->format->palette->ncolors)
        return 0;
    memcpy(&s->format->palette->colors[first], colors, (size_t)n * sizeof(SDL_Color));
    if (s == &video_surface)
        for (int i = 0; i < n; i++)
            pal16[first + i] = (uint16_t)(((colors[i].r >> 3) << 11) | ((colors[i].g >> 3) << 6) | ((colors[i].b >> 3) << 1) | 1);
    return 1;
}
int SDL_SetColors(SDL_Surface *s, SDL_Color *colors, int first, int n)
{
    return SDL_SetPalette(s, SDL_LOGPAL | SDL_PHYSPAL, colors, first, n);
}

static unsigned flip_count;
static int level_drawn; /* 1 = ya se mostro el primer fotograma del nivel */
static int rdp_ready;
static int rdp_copy_checked, rdp_copy_bad; /* f2v55 */
static int rdp_fill_checked, rdp_fill_bad; /* f2v56 */
/* medidas para el contador en pantalla */
static unsigned fps_shown, fps_frames, flip_us_acc, flip_us_shown, game_us_acc, game_us_shown;
static uint64_t fps_t0, flip_end_us;

static unsigned get_us_acc, rdp_us_acc, txt_us_acc, wb_us_acc, sub_us_acc, snd_us_acc;

/* ---------------- capa del panel (f2v26) ----------------
   El motor dibuja el panel en una capa de 640x160 aparte (scrollrt.cpp) y aqui el RDP la pone
   reducida abajo y centrada, encima del mundo (que ahora ocupa toda la pantalla).
   Se ve desde x=88 (frasco de vida) hasta x=560 (frasco de mana). Tamano: L+R+C arriba/abajo. */
static uint8_t *panel_px;
static int panel_pitch, panel_rows, panel_row0;
static int panel_drawn_this_frame;
#define PANEL_X0 88
#define PANEL_X1 560
static const int panel_widths[] = { 320, 288, 256, 224, 192 };
#define PANEL_NSIZES ((int)(sizeof panel_widths / sizeof panel_widths[0]))
static int panel_size_idx = 4; /* f2v29: por defecto el mas pequeno */
static int panel_size_loaded;
static uint16_t pal16_key[256] __attribute__((aligned(16)));
/* f2v56: cuadro de texto del panel (nombres de monstruos, objetos...) en una capa propia a tamano
   del juego (no reducida con el panel): se lee como en el DSi */
static uint8_t *itx_px;
static int itx_pitch, itx_w, itx_h, itx_show;
void n64_infotext_attach(uint8_t *px, int pitch, int w, int h);
void n64_infotext_attach(uint8_t *px, int pitch, int w, int h)
{
    itx_px = px;
    itx_pitch = pitch;
    itx_w = w;
    itx_h = h;
    itx_show = 0;
}
void n64_infotext_show(int show);
void n64_infotext_show(int show) { itx_show = show; }
/* f2v59: el texto lo escribe el RDP con la letra de 8 pixeles de libdragon, a tamano de la pantalla */
static char itx_text[256];
static int itx_r = 255, itx_g = 255, itx_b = 255;
static rdpq_font_t *itx_font;
void n64_infotext_set(const char *text, int r, int g, int b);
void n64_infotext_set(const char *text, int r, int g, int b)
{
    strncpy(itx_text, text ? text : "", sizeof itx_text - 1);
    itx_text[sizeof itx_text - 1] = 0;
    itx_r = r;
    itx_g = g;
    itx_b = b;
    itx_show = itx_text[0] != 0;
}
/* f2v57: barra de experiencia verde de 8 tramos (como en el DSi), dibujada por el RDP */
static int xpb_permille = -1, xpb_blink;
void n64_xpbar_set(int permille, int blink);
void n64_xpbar_set(int permille, int blink)
{
    xpb_permille = permille > 1000 ? 1000 : permille;
    xpb_blink = blink;
}
/* f2v50: capa de 640x480 (texto de misiones, tiendas) encima de todo; indice 0 transparente */
static uint8_t *ovl_px;
static int ovl_pitch;
static rspq_block_t *ovl_block;
static void *ob_px;
static int ob_dw, ob_dh;
void n64_overlay_attach(uint8_t *pixels, int pitch);
void n64_overlay_attach(uint8_t *pixels, int pitch)
{
    if (!pixels && ovl_block) {
        rspq_wait();
        rspq_block_free(ovl_block);
        ovl_block = NULL;
    }
    ovl_px = pixels;
    ovl_pitch = pitch;
}
/* el color mas oscuro de la paleta que no sea el 0 (para la trama de oscurecido) */
int n64_overlay_black(void);
int n64_overlay_black(void)
{
    int best = 1, bv = 1 << 30;
    for (int i = 1; i < 256; i++) {
        int v = video_colors[i].r * 3 + video_colors[i].g * 6 + video_colors[i].b;
        if (v < bv) {
            bv = v;
            best = i;
        }
    }
    return best;
}
void n64_panel_attach(uint8_t *pixels, int pitch, int rows, int panelRow)
{
    if (!pixels) { /* f2v39: el motor suelta la capa del panel para cargar un nivel */
        rdp_blocks_free();
        panel_px = NULL;
        return;
    }
    panel_px = pixels;
    panel_pitch = pitch;
    panel_rows = rows;
    panel_row0 = panelRow;
    n64_log("panel en capa aparte: %dx%d, panel desde la fila %d", pitch, rows, panelRow);
}
static int panel_visible = 1;
void n64_panel_visible(int visible);
void n64_panel_visible(int visible) { panel_visible = visible; } /* f2v53: oculto con el menu de START */
void n64_panel_frame(void) { panel_drawn_this_frame = 1; world_drawn = 1; }
static void panel_size_load(void);
static unsigned panel_serial = 1;
unsigned n64_panel_serial(void) { return panel_serial; }
/* f2v29: rectangulo (en pixeles del juego) que tapa del todo el cuerpo del panel reducido:
   el motor no dibuja los trozos de suelo y paredes que caen enteros dentro. */
int n64_panel_occluder(int *x0, int *y0, int *x1, int *y1)
{
    if (!panel_px || !video_ready || !panel_visible) {
        *x0 = *y0 = *x1 = *y1 = 0;
        return 0;
    }
    if (!panel_size_loaded)
        panel_size_load();
    const float k = (float)panel_widths[panel_size_idx] / (0.8f * (float)(PANEL_X1 - PANEL_X0));
    const float wg = (float)(PANEL_X1 - PANEL_X0) * k; /* ancho en pixeles del juego */
    const float hg = 128.0f * k;
    /* f2v55: pegado a la derecha (a la izquierda queda el minimapa) */
    *x1 = video_w - video_w / 24 - 3;
    *x0 = video_w - video_w / 24 - (int)wg + 3;
    *y0 = video_h - (int)hg + 3;
    *y1 = video_h;
    return 1;
}
static void panel_size_load(void)
{
    panel_size_loaded = 1;
    FILE *f = fopen("sd:/devilutionx/saves/n64_panel.txt", "rb");
    if (f) {
        int v = -1;
        if (fscanf(f, "%d", &v) == 1 && v >= 0 && v < PANEL_NSIZES)
            panel_size_idx = v;
        fclose(f);
    }
}
static void panel_size_change(int d)
{
    int v = panel_size_idx + d;
    if (v < 0 || v >= PANEL_NSIZES)
        return;
    panel_size_idx = v;
    panel_serial++;
    FILE *f = fopen("sd:/devilutionx/saves/n64_panel.txt", "wb");
    if (f) {
        fprintf(f, "%d\n", v);
        fclose(f);
    }
    n64_log("panel: ancho %d de 320", panel_widths[v]);
}

/* f2v62: ritmo parejo de imagen. Cada imagen se muestra un numero fijo de refrescos de la tele
   (2 = 30 fps, 3 = 20 fps, 4 = 15 fps) segun lo que tarda el juego en hacerla, en vez de 2, 3 o 4
   refrescos al azar (eso se veia como tirones) */
static volatile uint32_t vbl_count;
static void vbl_cb(void) { vbl_count++; }
static uint32_t pace_last_vbl;
static uint64_t pace_prev_us;
static unsigned pace_ema_us = 50000, pace_wait_acc, pace_hist[6];
static int pace_P = 3, pace_want = 3, pace_want_n;
static const int pace_on = 0;
static void pace_wait(void)
{
    const uint64_t now = TICKS_TO_US(get_ticks());
    if (pace_prev_us) {
        unsigned work = (unsigned)(now - pace_prev_us);
        if (work > 200000)
            work = 200000;
        pace_ema_us = (pace_ema_us * 7 + work) / 8;
    }
    const unsigned vper = get_tv_type() == TV_PAL ? 20000u : 16683u;
    int want = (int)((pace_ema_us + 3000 + vper - 1) / vper);
    if (want < 2)
        want = 2;
    if (want > 4)
        want = 4;
    if (want == pace_want) {
        if (++pace_want_n >= 8)
            pace_P = want; /* cambia solo si se mantiene 8 imagenes (sin saltos por un cuadro suelto) */
    } else {
        pace_want = want;
        pace_want_n = 0;
    }
    const uint32_t target = pace_last_vbl + (uint32_t)pace_P - 1u;
    while ((int32_t)(vbl_count - target) < 0) {
        n64_audio_pump();
        kthread_yield();
        if (TICKS_TO_US(get_ticks()) - now > 90000)
            break; /* por si no llegan los refrescos */
    }
    const uint64_t after = TICKS_TO_US(get_ticks());
    pace_wait_acc += (unsigned)(after - now);
    const uint32_t shown = vbl_count + 1u;
    uint32_t gap = shown - pace_last_vbl;
    if (gap > 5)
        gap = 5;
    pace_hist[gap]++;
    pace_last_vbl = shown;
    pace_prev_us = after;
}
int SDL_Flip(SDL_Surface *s)
{
    (void)s;
    if (!video_ready)
        return -1;
    uint64_t t_start = TICKS_TO_US(get_ticks());
    if (flip_end_us)
        game_us_acc += (unsigned)(t_start - flip_end_us);
    surface_t *d = display_get();
    uint64_t t_got = TICKS_TO_US(get_ticks());
    get_us_acc += (unsigned)(t_got - t_start);
    const int dw = d->width, dh = d->height;
    /* f2v51: lo que el RDP hizo en el fotograma anterior ya tiene que estar acabado antes de tocar
       la paleta, el panel o volver a dibujar en esa imagen (casi siempre lo esta) */
    if (rdp_sp_valid) {
        uint32_t tq = (uint32_t)get_ticks();
        rspq_syncpoint_wait(rdp_sp);
        rdp_sp_valid = 0;
        rdp_us_acc += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - tq);
    }
    /* f2v51: el sonido se rellena antes de mandar la imagen al RDP: el mezclador usa el RSP, que
       atiende las ordenes en orden, y despues tendria que esperar a que el RDP acabe la imagen */
    {
        uint32_t ta0 = (uint32_t)get_ticks();
        n64_audio_pump();
        snd_us_acc += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - ta0);
    }
    /* si el motor no dibujo la imagen entera (fundidos de paleta), se muestra la ultima completa */
    const int async_ok = rdp_ready && video_pixels2 && !log_overlay && world_drawn;
    Uint8 *const show_px = (video_pixels2 && !world_drawn) ? last_drawn_px : video_pixels;
    Uint8 *const saved_px = video_pixels;
    video_pixels = show_px;
    if (rdp_ready) {
        /* el RDP convierte la imagen de 8 bits con la paleta (CI8 + TLUT) y, si hace falta,
           la reduce al tamano de la pantalla con filtro bilineal */
        uint32_t tw0 = (uint32_t)get_ticks();
        data_cache_hit_writeback(video_pixels, (unsigned long)video_w * (unsigned long)video_h);
        data_cache_hit_writeback(pal16, sizeof pal16);
        wb_us_acc += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - tw0);
        uint32_t ts0 = (uint32_t)get_ticks();
        surface_t src = surface_make_linear(video_pixels, FMT_CI8, (uint16_t)video_w, (uint16_t)video_h);
        rdpq_attach(d, NULL);
        if (video_w == dw && video_h == dh) {
            rdpq_set_mode_copy(false);
            rdpq_mode_tlut(TLUT_RGBA16);
            rdpq_tex_upload_tlut(pal16, 0, 256);
            rdpq_tex_blit(&src, 0, 0, NULL);
        } else {
            /* f2v32: las ordenes del RDP para reducir la imagen se preparan una sola vez en un
               bloque y se repiten en cada fotograma (antes la CPU tardaba 13 ms en prepararlas) */
            if (video_w > 480) {
                /* f2v39: a 640x480 (menus y pantalla de carga) sin bloque: el bloque de 640x480
                   ocupaba ~100 KiB de RAM y en los menus da igual la velocidad */
                rdpq_set_mode_standard();
                rdpq_mode_tlut(TLUT_RGBA16);
                rdpq_mode_filter(FILTER_BILINEAR);
                rdpq_tex_upload_tlut(pal16, 0, 256);
                rdpq_blitparms_t bp0 = { .scale_x = (float)dw / (float)video_w, .scale_y = (float)dh / (float)video_h, .filtering = true };
                rdpq_tex_blit(&src, 0, 0, &bp0);
            } else {
            const int bi = (video_pixels == video_pixels2) ? 1 : 0;
            if (!world_blocks[bi] || wbs_w[bi] != video_w || wbs_h[bi] != video_h || wbs_dw[bi] != dw || wbs_dh[bi] != dh || wbs_px[bi] != (void *)video_pixels) {
                if (world_blocks[bi]) {
                    rspq_wait();
                    rspq_block_free(world_blocks[bi]);
                }
                rspq_block_begin();
                rdpq_set_mode_standard();
                rdpq_mode_tlut(TLUT_RGBA16);
                rdpq_mode_filter(FILTER_BILINEAR);
                rdpq_tex_upload_tlut(pal16, 0, 256);
                rdpq_blitparms_t bp = { .scale_x = (float)dw / (float)video_w, .scale_y = (float)dh / (float)video_h, .filtering = true };
                rdpq_tex_blit(&src, 0, 0, &bp);
                world_blocks[bi] = rspq_block_end();
                wbs_w[bi] = video_w; wbs_h[bi] = video_h; wbs_dw[bi] = dw; wbs_dh[bi] = dh; wbs_px[bi] = video_pixels;
            }
            rspq_block_run(world_blocks[bi]);
            }
        }
        if (panel_px && panel_drawn_this_frame && panel_visible) {
            if (!panel_size_loaded)
                panel_size_load();
            /* f2v29: solo de frasco a frasco (x 88..560); a los lados se ve el mundo. Escala en
               pixeles del juego (k) y luego la de la pantalla en cada eje (asi en panoramico no
               se estira). */
            const float k = (float)panel_widths[panel_size_idx] / (0.8f * (float)(PANEL_X1 - PANEL_X0));
            const float scx = k * (float)dw / (float)video_w;
            const float scy = k * (float)dh / (float)video_h;
            const int cx0 = PANEL_X0, cw = PANEL_X1 - PANEL_X0;
            const int W = (int)((float)cw * scx + 0.5f);
            const int top = panel_row0 - 13; /* parte de arriba de los frascos */
            const int body_h = panel_rows - panel_row0;
            const int H = (int)((float)(panel_rows - top) * scy + 0.5f);
            const int x = dw - W - (int)((float)(video_w / 24) * (float)dw / (float)video_w + 0.5f); /* f2v55: a la derecha */
            const int y = dh - H;
            data_cache_hit_writeback(panel_px, (unsigned long)panel_pitch * (unsigned long)panel_rows);
            memcpy(pal16_key, pal16, sizeof pal16_key);
            pal16_key[0] = 0; /* franja de encima (tapas de los frascos): el indice 0 es transparente */
            data_cache_hit_writeback(pal16_key, sizeof pal16_key);
            if (!panel_block || pb_idx != panel_size_idx || pb_w != video_w || pb_h != video_h || pb_dw != dw || pb_dh != dh || pb_px != (void *)panel_px) {
                if (panel_block)
                    rspq_block_free(panel_block);
                rspq_block_begin();
                surface_t ps = surface_make_linear(panel_px, FMT_CI8, (uint16_t)panel_pitch, (uint16_t)panel_rows);
                rdpq_set_mode_standard();
                rdpq_mode_tlut(TLUT_RGBA16);
                rdpq_mode_filter(FILTER_BILINEAR);
                /* cuerpo del panel: opaco */
                rdpq_tex_upload_tlut(pal16, 0, 256);
                rdpq_blitparms_t bp = { .s0 = cx0, .t0 = panel_row0, .width = cw, .height = body_h,
                    .scale_x = scx, .scale_y = scy, .filtering = true };
                rdpq_tex_blit(&ps, x, y + (int)((float)(panel_row0 - top) * scy + 0.5f), &bp);
                rdpq_mode_alphacompare(128);
                rdpq_tex_upload_tlut(pal16_key, 0, 256);
                rdpq_blitparms_t bt = { .s0 = cx0, .t0 = top, .width = cw, .height = panel_row0 - top,
                    .scale_x = scx, .scale_y = scy, .filtering = true };
                rdpq_tex_blit(&ps, x, y, &bt);
                rdpq_mode_alphacompare(0);
                panel_block = rspq_block_end();
                pb_idx = panel_size_idx; pb_w = video_w; pb_h = video_h; pb_dw = dw; pb_dh = dh; pb_px = panel_px;
            }
            rspq_block_run(panel_block);
            if (xpb_permille >= 0) {
                /* donde iba la barra original: x 165, fila 149, 313x9 en la capa del panel */
                const int bx0 = x + (int)((float)(165 - cx0) * scx + 0.5f);
                const int bw = (int)(313.0f * scx + 0.5f);
                int bh = (int)(9.0f * scy + 0.5f);
                if (bh < 4)
                    bh = 4;
                const int by0 = y + (int)(((float)(149 - top) + 4.5f) * scy + 0.5f) - bh / 2;
                const int fill = bw * xpb_permille / 1000;
                const int on = xpb_blink && ((get_ticks_ms() / 200u) & 1u);
                const color_t green = RGBA32(49, 214, 82, 255), hi = RGBA32(150, 245, 160, 255);
                const color_t dark = RGBA32(40, 40, 40, 255), black = RGBA32(0, 0, 0, 255);
                rdpq_set_mode_standard(); /* color plano (el modo de relleno va de 4 en 4 pixeles) */
                rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
                rdpq_set_prim_color(black);
                rdpq_fill_rectangle(bx0 - 1, by0 - 1, bx0 + bw + 1, by0 + bh + 1);
                rdpq_set_prim_color(dark);
                rdpq_fill_rectangle(bx0, by0, bx0 + bw, by0 + bh);
                if (on) {
                    rdpq_set_prim_color(hi);
                    rdpq_fill_rectangle(bx0, by0, bx0 + bw, by0 + bh);
                } else if (fill > 0) {
                    rdpq_set_prim_color(green);
                    rdpq_fill_rectangle(bx0, by0 + 1, bx0 + fill, by0 + bh);
                    rdpq_set_prim_color(hi);
                    rdpq_fill_rectangle(bx0, by0, bx0 + fill, by0 + 1);
                }
                rdpq_set_prim_color(black);
                for (int i = 1; i < 8; i++) {
                    const int sx = bx0 + bw * i / 8;
                    rdpq_fill_rectangle(sx, by0, sx + 1, by0 + bh);
                }
            }
            if (itx_show && itx_text[0]) {
                /* dentro del cuadro de texto del panel (x 177, y 46, 288x64 en el panel de 640) */
                if (!itx_font) {
                    itx_font = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_VAR);
                    rdpq_text_register_font(1, itx_font);
                }
                rdpq_font_style(itx_font, 0, &(rdpq_fontstyle_t){ .color = RGBA32(itx_r, itx_g, itx_b, 255) });
                const int bx = x + (int)((float)(177 - cx0 + 6) * scx + 0.5f);
                const int by = y + (int)((float)(panel_row0 + 46 - top + 2) * scy + 0.5f);
                const int bw = (int)((float)(288 - 12) * scx + 0.5f);
                const int bh = (int)((float)(64 - 4) * scy + 0.5f);
                rdpq_set_mode_standard();
                rdpq_textparms_t tp = { .width = (int16_t)bw, .height = (int16_t)bh, .align = ALIGN_CENTER, .valign = VALIGN_CENTER, .wrap = WRAP_WORD };
                rdpq_text_print(&tp, 1, (float)bx, (float)by, itx_text);
            }
        }
        panel_drawn_this_frame = 0;
        if (ovl_px) {
            data_cache_hit_writeback(ovl_px, (unsigned long)ovl_pitch * 480ul);
            memcpy(pal16_key, pal16, sizeof pal16_key);
            pal16_key[0] = 0;
            data_cache_hit_writeback(pal16_key, sizeof pal16_key);
            /* f2v52: las ordenes se preparan una vez en un bloque (antes 35 ms de CPU por fotograma) */
            if (!ovl_block || ob_px != (void *)ovl_px || ob_dw != dw || ob_dh != dh) {
                if (ovl_block) {
                    rspq_wait();
                    rspq_block_free(ovl_block);
                }
                rspq_block_begin();
                surface_t os = surface_make_linear(ovl_px, FMT_CI8, (uint16_t)ovl_pitch, 480);
                rdpq_set_mode_standard();
                rdpq_mode_tlut(TLUT_RGBA16);
                rdpq_mode_filter(FILTER_BILINEAR);
                rdpq_mode_alphacompare(1);
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY); /* la trama filtrada queda como oscurecido */
                rdpq_tex_upload_tlut(pal16_key, 0, 256);
                /* f2v59: en tiras de 3 filas (2 se dibujan, la tercera para el filtro): el bloque de
                   rdpq_tex_blit ocupaba 224 KiB y en el pueblo dejaba 109 KiB libres */
                const float osy = (float)dh / 480.0f;
                for (int t = 0; t < 480; t += 2) {
                    const int t1 = t + 3 > 480 ? 480 : t + 3;
                    rdpq_tex_upload_sub(TILE0, &os, NULL, 0, t, 640, t1);
                    rdpq_texture_rectangle_scaled(TILE0, 0.0f, (float)t * osy, (float)dw, (float)(t + 2) * osy, 0.0f, (float)t, 640.0f, (float)(t + 2));
                }
                rdpq_mode_blender(0);
                rdpq_mode_alphacompare(0);
                ovl_block = rspq_block_end();
                ob_px = ovl_px;
                ob_dw = dw;
                ob_dh = dh;
                n64_log("capa de textos: bloque del RDP hecho (libre %d KiB)", heap_free_kib());
            }
            rspq_block_run(ovl_block);
        }
        sub_us_acc += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - ts0);
        uint64_t t_sub = TICKS_TO_US(get_ticks());
        if (async_ok) {
            /* f2v51: el RDP acaba solo y muestra la imagen; el motor dibuja ya en la otra */
            if (pace_on && video_w < 640) /* f2v63: apagado (en f2v62 no se veia mas suave y recortaba fps) */
                pace_wait();
            rdpq_detach_show();
            rdp_sp = rspq_syncpoint_new();
            rdp_sp_valid = 1;
        } else {
            rdpq_detach_wait(); /* el motor vuelve a dibujar en cuanto salimos: hay que esperar */
        }
        rdp_us_acc += (unsigned)(TICKS_TO_US(get_ticks()) - t_sub);
    } else {
        /* conversion por la CPU (si el tamano no coincide) */
        for (int y = 0; y < dh; y++) {
            const Uint8 *src = video_pixels + (y * video_h / dh) * video_w;
            uint16_t *dst = (uint16_t *)((uint8_t *)d->buffer + y * d->stride);
            for (int x = 0; x < dw; x++)
                dst[x] = pal16[src[x * video_w / dw]];
        }
    }
    if (log_overlay) {
        uint64_t t_txt0 = TICKS_TO_US(get_ticks());
        char t[96];
        snprintf(t, sizeof t, "%ufps i%u j%u L%dK m%dK", fps_shown, flip_us_shown / 1000, game_us_shown / 1000,
            heap_free_kib(), (int)(mem_low / 1024));
        graphics_set_color(graphics_make_color(0, 255, 0, 255), graphics_make_color(0, 0, 0, 160));
        graphics_draw_text(d, dw - 4 - 8 * (int)strlen(t), 2, t);
        snprintf(t, sizeof t, "#%u sd %u/%d pila %uK", flip_count, sd_err, sd_errno, stack_used_kib());
        graphics_draw_text(d, 4, 12, t);
        log_draw(d, 0, 2, dh - 20);
        txt_us_acc += (unsigned)(TICKS_TO_US(get_ticks()) - t_txt0);
    }
    video_pixels = saved_px;
    if (async_ok) {
        last_drawn_px = video_pixels;
        video_pixels = (video_pixels == video_pixels1) ? video_pixels2 : video_pixels1;
        video_surface.pixels = video_pixels;
    } else {
        display_show(d);
    }
    world_drawn = 0;
    flip_count++;
    if ((flip_count & 63) == 0) /* f2v31: antes en cada fotograma */
        n64_check_guards("Flip");
    if (CEL_ARENA >= 8192 && level_drawn < 300) {
        /* primeros fotogramas del nivel: una linea por fotograma, guardada al momento */
        level_drawn++;
        if (level_drawn <= 5) { /* (f2v21) antes cada 20 fotogramas: escribir en la SD daba tirones */
            n64_log("fotograma de nivel %d: cache %u/%u, libre %d KiB, pila %u KiB", level_drawn, cel_hits, cel_misses,
                heap_free_kib(), stack_used_kib());
            n64_log_flush();
        }
    }
    uint64_t t_end = TICKS_TO_US(get_ticks());
    flip_us_acc += (unsigned)(t_end - t_start);
    fps_frames++;
    if (t_end - fps_t0 >= 1000000) {
        n64_log_flush();
        fps_shown = fps_frames;
        flip_us_shown = fps_frames ? flip_us_acc / fps_frames : 0;
        game_us_shown = fps_frames ? game_us_acc / fps_frames : 0;
        static unsigned secs;
        if ((++secs % 10) == 0) {
            unsigned nf = fps_frames ? fps_frames : 1;
            n64_log("estado: %u fps, imagen %u ms (vaciar cache %u, ordenes RDP %u, esperar RDP %u, sonido %u), juego %u ms = logica %u + dibujo %u (mundo %u = suelo %u + resto %u, de ello cartucho %u) + dormir %u + otros, libre %d KiB (minimo %d KiB), cache CEL %u/%u/%u, pila %u/%u KiB, SD rapida %u lecturas %u KiB fallos %u, sectores DMA %u lentos %u, monstruos %u/%u, sonidos %u cargados en %u ms, %u tocados (leer musica %u ms, efectos %u ms por cuadro)", fps_shown,
                flip_us_shown / 1000, wb_us_acc / nf / 1000, sub_us_acc / nf / 1000, rdp_us_acc / nf / 1000, snd_us_acc / nf / 1000,
                game_us_shown / 1000, logic_us_acc / nf / 1000, (blit_us_acc > flip_us_acc ? blit_us_acc - flip_us_acc : 0) / nf / 1000,
                view_us_acc / nf / 1000, floor_us_acc / nf / 1000, content_us_acc / nf / 1000, cart_us_acc / nf / 1000, sleep_us_acc / nf / 1000,
                heap_free_kib(), (int)(mem_low / 1024), cel_hits, cel_misses, cel_moves,
                stack_used_kib(), ENGINE_STACK / 1024, fast_reads, fast_kib, fast_fail, disk_fast_sect, disk_slow_sect, monc_hits, monc_misses, sfx_loaded, sfx_load_ms, sfx_played, music_read_us / nf / 1000, sfx_read_us / nf / 1000);
        }
        prof_acc[20] += fps_frames;
        if ((secs % 10) == 0 && prof_acc[13]) {
            /* f2v43: de que se compone la logica (media por tic, en decimas de ms) */
            unsigned nt = prof_acc[13], nf = prof_acc[20];
#define PT(i) (prof_acc[i] / nt / 100)
            n64_log("logica por tic (decimas de ms): %u tics en %u cuadros; entrada %u (mando %u, cursor %u, objetivo %u, objetos %u, salidas %u, repetir %u), jugador %u, monstruos %u (de ello caminos %u en %u busquedas/tic x10), "
                    "objetos %u, misiles %u, objetos del suelo %u, luces %u, vision %u, aldeanos %u, resto %u; monstruos activos %u/tic x10 de %u",
                nt, nf, PT(18), PT(24), PT(25), PT(21), PT(22), PT(23), PT(26), PT(5), PT(6), PT(14), prof_acc[15] * 10 / nt, PT(7), PT(8), PT(9), PT(10), PT(11), PT(17), PT(12),
                prof_acc[16] * 10 / nt, prof_acc[19] / nt);
#undef PT
        }
        if ((secs % 10) == 0 && prof_acc[20]) {
            unsigned nf2 = prof_acc[20];
            n64_log("dibujo por cuadro (decimas de ms): paredes %u, mapa %u; suelo copiado en %u cuadros y dibujado en %u; copias del RDP comprobadas %d, mal %d; borrados comprobados %d, mal %d; minimapa redibujado %u veces",
                prof_acc[29] / nf2 / 100, prof_acc[30] / nf2 / 100, prof_acc[27], prof_acc[28], rdp_copy_checked, rdp_copy_bad, rdp_fill_checked, rdp_fill_bad, prof_acc[31]);
            n64_log("suelo desplazado: %u cuadros (casillas redibujadas %u por cuadro), impar o sin poder %u; ritmo: imagenes de 2 refrescos %u, de 3 %u, de 4 %u, de 5 o mas %u, espera %u ms por cuadro, objetivo %d",
                prof_acc[32], prof_acc[32] ? prof_acc[33] / prof_acc[32] : 0, prof_acc[34], pace_hist[2], pace_hist[3], pace_hist[4], pace_hist[5], pace_wait_acc / 1000 / nf2, pace_P);
            memset(pace_hist, 0, sizeof pace_hist);
            pace_wait_acc = 0;
        }
        if ((secs % 10) == 0)
            memset(prof_acc, 0, sizeof prof_acc);
        cart_us_acc = sleep_us_acc = logic_us_acc = view_us_acc = blit_us_acc = 0;
        music_read_us = sfx_read_us = 0;
        wb_us_acc = sub_us_acc = snd_us_acc = floor_us_acc = content_us_acc = 0;
        fps_frames = 0;
        get_us_acc = 0;
        txt_us_acc = 0;
        rdp_us_acc = 0;
        flip_us_acc = 0;
        game_us_acc = 0;
        fps_t0 = t_end;
    }
    flip_end_us = t_end;
    kthread_yield();
    return 0;
}

void SDL_VideoQuit(void) {}

/* ---------------- inicio / cierre ---------------- */
int SDL_Init(Uint32 flags) { (void)flags; return 0; }
Uint32 SDL_WasInit(Uint32 flags) { return flags ? flags : SDL_INIT_VIDEO; }
void SDL_Quit(void) {}
int SDL_EnableUNICODE(int enable) { (void)enable; return 0; }
int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
void SDL_WarpMouse(Uint16 x, Uint16 y) { (void)x; (void)y; }
SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode) { (void)mode; return SDL_GRAB_OFF; }
void SDL_WM_SetCaption(const char *title, const char *icon) { (void)title; (void)icon; }
int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int index) { (void)index; return NULL; }
const char *SDL_JoystickName(int index) { (void)index; return NULL; }

/* ---------------- archivos ---------------- */
static int rw_seek(SDL_RWops *c, int offset, int whence)
{
    FILE *fp = (FILE *)c->hidden.unknown.data1;
    if (fseek(fp, offset, whence) != 0)
        return -1;
    return (int)ftell(fp);
}
static int rw_read(SDL_RWops *c, void *ptr, int size, int maxnum)
{
    return (int)fread(ptr, (size_t)size, (size_t)maxnum, (FILE *)c->hidden.unknown.data1);
}
static int rw_write(SDL_RWops *c, const void *ptr, int size, int num)
{
    return (int)fwrite(ptr, (size_t)size, (size_t)num, (FILE *)c->hidden.unknown.data1);
}
static int rw_close(SDL_RWops *c)
{
    int r = 0;
    if (c) {
        FILE *fp = (FILE *)c->hidden.unknown.data1;
        if (fp)
            r = (fclose(fp) == 0) ? 0 : -1;
        free(c);
    }
    return r;
}
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
    FILE *fp;
    SDL_RWops *c;
    if (!file || !mode) {
        SDL_SetError("RWFromFile: argumentos invalidos");
        return NULL;
    }
    fp = fopen(file, mode);
    if (!fp) {
        SDL_SetError("RWFromFile: no se pudo abrir %s", file);
        return NULL;
    }
    c = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    if (!c) {
        fclose(fp);
        return NULL;
    }
    c->seek = rw_seek;
    c->read = rw_read;
    c->write = rw_write;
    c->close = rw_close;
    c->hidden.unknown.data1 = fp;
    return c;
}

/* ---------------- hilos cooperativos (kernel de libdragon) ----------------
   NOTA: kthread_join de esta version de libdragon falla con un assert
   (th_cur->flags & TH_FLAG_INLIST), asi que los hilos se crean "desprendidos"
   y la espera se hace mirando una bandera. */
#define N64_THREAD_STACK (64 * 1024)
typedef struct {
    int (*fn)(void *);
    void *data;
    volatile int done;
    int result;
} N64Thread;

static int thread_tramp(void *arg)
{
    N64Thread *t = (N64Thread *)arg;
    redirect_std();
    t->result = t->fn(t->data);
    t->done = 1;
    return t->result;
}

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data)
{
    N64Thread *t = (N64Thread *)calloc(1, sizeof(N64Thread));
    if (!t)
        return NULL;
    t->fn = fn;
    t->data = data;
    kthread_t *k = kthread_new("sdl", N64_THREAD_STACK, 1, thread_tramp, t); /* misma prioridad que el motor */
    if (!k) {
        free(t);
        SDL_SetError("kthread_new ha fallado");
        return NULL;
    }
    kthread_detach(k);
    n64_log("hilo creado");
    return (SDL_Thread *)t;
}
Uint32 SDL_GetThreadID(SDL_Thread *th)
{
    return (Uint32)(uintptr_t)(th ? (void *)th : (void *)kthread_current());
}
void SDL_WaitThread(SDL_Thread *th, int *status)
{
    N64Thread *t = (N64Thread *)th;
    if (!t)
        return;
    while (!t->done)
        kthread_yield();
    if (status)
        *status = t->result;
    free(t);
}
SDL_mutex *SDL_CreateMutex(void)
{
    kmutex_t *m = (kmutex_t *)calloc(1, sizeof(kmutex_t));
    if (m)
        kmutex_init(m, KMUTEX_RECURSIVE);
    return (SDL_mutex *)m;
}
int SDL_mutexP(SDL_mutex *m)
{
    if (!m)
        return -1;
    kmutex_lock((kmutex_t *)m);
    return 0;
}
int SDL_mutexV(SDL_mutex *m)
{
    if (!m)
        return -1;
    kmutex_unlock((kmutex_t *)m);
    return 0;
}
void SDL_DestroyMutex(SDL_mutex *m)
{
    if (m) {
        kmutex_destroy((kmutex_t *)m);
        free(m);
    }
}

/* ---------------- entrada: mando N64 como teclado (igual que el RG99) ---------------- */
#define EVQ_SIZE 64
static SDL_Event evq[EVQ_SIZE];
static unsigned evq_head, evq_tail;
static Uint8 key_state[SDLK_LAST];

static void push_key(SDLKey sym, int down)
{
    unsigned next = (evq_head + 1) % EVQ_SIZE;
    if (next == evq_tail || sym == SDLK_UNKNOWN)
        return;
    SDL_Event *e = &evq[evq_head];
    memset(e, 0, sizeof *e);
    e->type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e->key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e->key.keysym.sym = sym;
    e->key.keysym.unicode = 0;
    key_state[sym] = down ? 1 : 0;
    evq_head = next;
}

/* bit del boton -> tecla (ver CMake/platforms/n64.cmake) */
static SDLKey map_button(int bit)
{
    switch (bit) {
    case 0: return SDLK_LCTRL;     /* A          -> aceptar / accion (f2v30: antes al reves) */
    case 1: return SDLK_LALT;      /* B          -> atras */
    case 2: return SDLK_TAB;       /* Z          -> pocion de vida (f2v45; antes ESCAPE) */
    case 3: return SDLK_RETURN;    /* START      -> Start */
    case 4: return SDLK_UP;        /* cruceta */
    case 5: return SDLK_DOWN;
    case 6: return SDLK_LEFT;
    case 7: return SDLK_RIGHT;
    case 10: return SDLK_END;      /* L          -> lista de hechizos (f2v45; antes TAB) */
    case 11: return SDLK_BACKSPACE;/* R          -> hombro derecho */
    case 12: return SDLK_PAGEUP;   /* C arriba   -> gatillo izquierdo */
    case 13: return SDLK_PAGEDOWN; /* C abajo    -> gatillo derecho */
    case 14: return SDLK_SPACE;    /* C izquierda-> Y */
    case 15: return SDLK_LSHIFT;   /* C derecha  -> X */
    default: return SDLK_UNKNOWN;
    }
}

static uint16_t prev_bits;

static int stick_x, stick_y;
void n64_stick(int *x, int *y)
{
    *x = stick_x;
    *y = stick_y;
}
static void poll_pad(void)
{
    static uint64_t last_ms;
    uint64_t now = get_ticks_ms();
    if (now - last_ms < 8)
        return;
    last_ms = now;
    joypad_poll();
    joypad_buttons_t b = joypad_get_buttons(JOYPAD_PORT_1);
    uint16_t bits = 0;
    /* orden de bits propio: 0 A,1 B,2 Z,3 START,4..7 cruceta,10 L,11 R,12..15 C */
    if (b.a) bits |= 1 << 0;
    if (b.b) bits |= 1 << 1;
    if (b.z) bits |= 1 << 2;
    if (b.start) bits |= 1 << 3;
    if (b.d_up) bits |= 1 << 4;
    if (b.d_down) bits |= 1 << 5;
    if (b.d_left) bits |= 1 << 6;
    if (b.d_right) bits |= 1 << 7;
    if (b.l) bits |= 1 << 10;
    if (b.r) bits |= 1 << 11;
    if (b.c_up) bits |= 1 << 12;
    if (b.c_down) bits |= 1 << 13;
    if (b.c_left) bits |= 1 << 14;
    if (b.c_right) bits |= 1 << 15;
    uint16_t changed = bits ^ prev_bits;
    static uint16_t swallowed; /* C arriba/abajo usados para el tamano del panel: no llegan al juego */
    const int lr = (bits & ((1 << 10) | (1 << 11))) == ((1 << 10) | (1 << 11));
    for (int i = 0; i < 16; i++)
        if (changed & (1 << i)) {
            int down = (bits >> i) & 1;
            if ((i == 12 || i == 13) && down && lr) {
                swallowed |= (uint16_t)(1 << i);
                continue;
            }
            if (!down && (swallowed & (1 << i))) {
                swallowed &= (uint16_t)~(1 << i);
                continue;
            }
            push_key(map_button(i), down);
        }
    prev_bits = bits;

    /* f2v30: la palanca analogica ya no son flechas: el motor la lee como stick (n64_stick) */
    {
        joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
        stick_x = in.stick_x;
        stick_y = in.stick_y;
    }
    /* L + R + C arriba / C abajo: panel mas grande / mas pequeno (f2v26) */
    if ((bits & ((1 << 10) | (1 << 11))) == ((1 << 10) | (1 << 11))) {
        if ((changed & (1 << 12)) && (bits & (1 << 12)))
            panel_size_change(-1);
        if ((changed & (1 << 13)) && (bits & (1 << 13)))
            panel_size_change(+1);
    }
    /* Z + L + R: mostrar/ocultar el registro sobre la imagen */
    if ((bits & ((1 << 2) | (1 << 10) | (1 << 11))) == ((1 << 2) | (1 << 10) | (1 << 11))
        && (changed & ((1 << 2) | (1 << 10) | (1 << 11))))
        log_overlay = !log_overlay;
}

int SDL_PollEvent(SDL_Event *event)
{
    poll_pad();
    if (evq_tail == evq_head)
        return 0;
    if (event)
        *event = evq[evq_tail];
    evq_tail = (evq_tail + 1) % EVQ_SIZE;
    return 1;
}
int SDL_PushEvent(SDL_Event *event)
{
    unsigned next = (evq_head + 1) % EVQ_SIZE;
    if (!event || next == evq_tail)
        return -1;
    evq[evq_head] = *event;
    evq_head = next;
    return 0;
}
Uint8 *SDL_GetKeyState(int *numkeys)
{
    if (numkeys)
        *numkeys = SDLK_LAST;
    return key_state;
}
SDLMod SDL_GetModState(void) { return KMOD_NONE; }
Uint8 SDL_GetMouseState(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

/* ---------------- arranque ---------------- */
static int (*real_main)(int, char **);
static int n_argc;
static char **n_argv;

static cookie_io_functions_t log_fns = { NULL, log_write, NULL, NULL };
static FILE *log_file;

static void redirect_std(void)
{
    if (log_file) {
        stdout = log_file;
        stderr = log_file;
    }
}

/* si el motor llama a exit(), la consola apagaria la imagen (pantalla azul de la tele):
   en su lugar se muestran los ultimos mensajes y se queda ahi */
static void on_exit_show_log(void)
{
    n64_log_flush();
    log_overlay = 0;
    for (;;)
        log_screen("Belial 64 - el motor se cerro (exit). Ultimos mensajes:");
}

static volatile int engine_done;
static int engine_result;
static uint8_t *stack_paint_lo, *stack_paint_hi; /* zona pintada con 0xA5 para medir la pila */
static unsigned stack_used_kib(void)
{
    if (!stack_paint_lo)
        return 0;
    uint8_t *p = stack_paint_lo;
    while (p < stack_paint_hi && *p == 0xA5)
        p++;
    return (unsigned)((stack_paint_hi - p) / 1024);
}

/* errores de CPU: anotar en el registro y guardarlo en la SD antes de la pantalla de libdragon */
static void n64_exception_handler(exception_t *ex)
{
    static int inside;
    if (!inside) {
        inside = 1;
        n64_log("ERROR DE CPU: %s", ex->info ? ex->info : "?");
        n64_log("  pc %08lx ra %08lx sp %08lx dir %08lx", (unsigned long)ex->regs->epc, (unsigned long)ex->regs->ra,
            (unsigned long)ex->regs->sp, (unsigned long)C0_BADVADDR());
        n64_log("  a0 %08lx a1 %08lx v0 %08lx s0 %08lx", (unsigned long)ex->regs->a0, (unsigned long)ex->regs->a1,
            (unsigned long)ex->regs->v0, (unsigned long)ex->regs->s0);
        n64_log("  fotogramas %u, nivel dibujado %d, pila %u KiB, libre %d KiB", flip_count, level_drawn, stack_used_kib(), heap_free_kib());
        n64_log_flush();
    }
    exception_default_handler(ex);
}

static int engine_thread(void *arg)
{
    (void)arg;
    {
        /* pintar la pila libre (por debajo de aqui) para medir luego cuanto se usa */
        volatile uint8_t marca = 0;
        uintptr_t here = (uintptr_t)&marca;
        uint8_t *top = (uint8_t *)(here - 2048);
        uint8_t *bottom = (uint8_t *)(here - (ENGINE_STACK - 32768)); /* margen de 32 KiB */
        memset(bottom, 0xA5, (size_t)(top - bottom));
        stack_paint_lo = bottom;
        stack_paint_hi = top;
    }
    redirect_std(); /* cada hilo tiene su propio stdout/stderr */
    n64_log("6/6 hilo del motor en marcha");
    log_screen("Belial 64 - arrancando...");
    engine_result = real_main(n_argc, n_argv);
    engine_done = 1;
    return engine_result;
}

int n64_main(int argc, char **argv, int (*fn)(int, char **))
{
    (void)argc; (void)argv;
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE); /* 320x240: resolucion nativa de la N64 */
    register_VI_handler(vbl_cb); /* f2v62: cuenta de refrescos para el ritmo */
    rdpq_init();
    /* sonido (f2v23): 22050 Hz como la musica de Diablo, ~300 ms de margen por si un
       fotograma tarda */
    audio_init(22050, AUDIO_INIT_LATENCY_MS(500));
    mixer_init(8); /* f2v48: canal 0 musica, 1..7 efectos y voces */
    n64_sfx_init();
    audio_ready = 1;
    rdp_ready = 1;
    n64_log("Belial 64 (DevilutionX) - fase 2 v78 (sin parpadeo en menus) - Vanar");
    n64_log("RAM: %d KiB, Expansion Pak: %s", get_memory_size() / 1024, is_memory_expanded() ? "SI" : "NO");
    log_screen("Belial 64 - arrancando...");
    if (!is_memory_expanded()) {
        n64_log("ERROR: hace falta el Expansion Pak");
        log_screen("Belial 64 - ERROR");
        for (;;) {}
    }
    n64_log("1/6 montando la SD...");
    log_screen("Belial 64 - arrancando...");
    if (!debug_init_sdfs("sd:/", -1)) {
        n64_log("ERROR: no se pudo montar la SD");
        log_screen("Belial 64 - ERROR");
        for (;;) {}
    }
    struct stat st;
    if (stat("sd:/devilutionx", &st) != 0)
        mkdir("sd:/devilutionx", 0777);
    {
        FILE *f = fopen(LOG_SD_PATH, "w");
        if (f) {
            /* volcar lo que ya habia en pantalla */
            for (int i = LOG_LINES - 1; i >= 0; i--) {
                int idx = (log_head - i + LOG_LINES) % LOG_LINES;
                if (log_buf[idx][0])
                    fprintf(f, "%s\n", log_buf[idx]);
            }
            fclose(f);
            log_sd = 1;
        }
    }
    n64_log("2/6 SD OK (registro en sd:/devilutionx/n64_log.txt). comprobando carpetas...");
    n64_disk_fast_test();
    log_screen("Belial 64 - arrancando...");
    if (stat("sd:/devilutionx/saves", &st) != 0)
        mkdir("sd:/devilutionx/saves", 0777);
    n64_log("assets: %s", stat("sd:/devilutionx/assets", &st) == 0 ? "OK" : "FALTA sd:/devilutionx/assets");
    n64_log("diabdat: %s", stat("sd:/devilutionx/diabdat", &st) == 0 ? "OK" : "FALTA sd:/devilutionx/diabdat");
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    n64_log("heap libre: %d KiB", (hs.total - hs.used) / 1024);
    n64_log("3/6 iniciando hilos y mando...");
    log_screen("Belial 64 - arrancando...");
    kernel_init();
    joypad_init();
    n64_log("4/6 redirigiendo mensajes...");
    log_screen("Belial 64 - arrancando...");
    log_file = fopencookie(NULL, "w", log_fns);
    if (log_file)
        setvbuf(log_file, NULL, _IOLBF, 0);
    redirect_std();
    atexit(on_exit_show_log);
    register_exception_handler(n64_exception_handler);
    n64_log("5/6 arrancando el motor (puede tardar minutos)...");
    log_screen("Belial 64 - arrancando...");

    static char *args[] = {
        "devilutionx", /* f2v65: sin "-n" (saltaba el logo y la intro) */
        "--data-dir", "sd:/devilutionx/",
        "--save-dir", "sd:/devilutionx/saves/",
        "--config-dir", "sd:/devilutionx/saves/",
        "--verbose",
        NULL
    };
    n_argc = (int)(sizeof args / sizeof args[0]) - 1; /* f2v65 */
    n_argv = args;
    real_main = fn;
    /* el motor corre en un hilo con pila grande (en la DSi hizo falta 512 KiB) */
    kthread_t *t = kthread_new("motor", ENGINE_STACK, 1, engine_thread, NULL); /* prioridad 1 > hilo principal (0) */
    if (!t) {
        n64_log("ERROR: no se pudo crear el hilo del motor");
    } else {
        kthread_detach(t);
        while (!engine_done)
            kthread_sleep(TICKS_FROM_MS(200));
    }
    n64_log("el motor termino con codigo %d", engine_result);
    log_overlay = 0;
    for (;;)
        log_screen("Diablo N64 - fin");
    return engine_result;
}


/* ---------------- graficos de monstruos en el cartucho (f2v41) ----------------
   Cada tipo de monstruo son 300-600 KiB de CLX y en la RAM no caben mas de dos. Los archivos .clx
   de la SD se copian tal cual a la memoria del cartucho (DMA del propio cartucho, como el CEL) y en
   la RAM queda solo la cabecera (tabla de fotogramas). Al dibujar, cada fotograma se trae por PI a
   una cache en anillo y ahi se le aplica la paleta del tipo (TRN). */
#define MON_CART 0x13000000u
#define MON_CART_END 0x13E00000u
#define MON_FILES 96
static char mon_name[MON_FILES][112];
static uint32_t mon_addr[MON_FILES], mon_size[MON_FILES];
static int mon_count;
static uint32_t mon_w = MON_CART;
static unsigned mon_copied_kib, mon_reused;

/* al empezar a cargar un nivel: si la zona va por la mitad, se empieza de cero */
void n64_mon_level_begin(void);
void n64_mon_level_begin(void)
{
    if (mon_w - MON_CART > (MON_CART_END - MON_CART) / 2) {
        n64_log("monstruos: zona del cartucho llena (%u KiB), se vacia", (unsigned)((mon_w - MON_CART) / 1024));
        mon_count = 0;
        mon_w = MON_CART;
    }
}

/* copia un archivo abierto (de tamano sz) a la memoria del cartucho en addr y lo comprueba */
static int fd_copy_to_cart(FILE *f, long sz, uint32_t addr, const char *path)
{
    /* f2v42: como la lectura rapida (que funciona): el sector de cada cluster se pregunta con el
       descriptor (sin el bufer de stdio). En f2v41 se usaba copy_file_to_cart y salian mal todos. */
    int ok = 1;
    {
        const int fd = fileno(f);
        int32_t csize = 0;
        if (fd < 0 || ioctl(fd, IOFAT_GET_CLUSTER_SIZE, &csize) != 0 || csize <= 0)
            ok = 0;
        const uint32_t cbytes = (uint32_t)(csize > 0 ? csize : 1) * 512u;
        uint8_t one;
        for (uint32_t c = 0; ok && c < (uint32_t)sz; c += cbytes) {
            int64_t sect = 0;
            if (lseek(fd, (long)c, SEEK_SET) != (long)c || read(fd, &one, 1) != 1
                || ioctl(fd, IOFAT_GET_SECTOR, &sect) != 0 || sect <= 0) {
                n64_log("monstruos: sin sector en %u de %s", (unsigned)c, path);
                ok = 0;
                break;
            }
            uint32_t left = (uint32_t)sz - c;
            uint32_t nsect = (left + 511) / 512;
            if (nsect > cbytes / 512)
                nsect = cbytes / 512;
            if (cart_card_rd_cart(addr + c, (uint32_t)sect, nsect) != 0) {
                n64_log("monstruos: fallo el DMA del cartucho en %u de %s", (unsigned)c, path);
                ok = 0;
                break;
            }
            if ((c / cbytes) % 8 == 7)
                n64_audio_pump();
        }
        stdio_resync(f, fd, 0);
    }
    /* comprobar el principio, el medio y el final contra la lectura normal */
    if (ok) {
        static uint8_t a[512 + 16] __attribute__((aligned(16)));
        static uint8_t b[512];
        const uint32_t n = sz < 512 ? (uint32_t)sz : 512;
        const uint32_t at[3] = { 0, ((uint32_t)sz / 2) & ~7u, ((uint32_t)sz - n) & ~7u };
        for (int k = 0; k < 3 && ok; k++) {
            uint32_t len = n;
            if (at[k] + len > (uint32_t)sz)
                len = (uint32_t)sz - at[k];
            cart_read(a, addr + at[k], (len + 7) & ~7u);
            const int fd = fileno(f);
            ok = lseek(fd, (long)at[k], SEEK_SET) == (long)at[k] && read(fd, b, len) == (int)len && memcmp(a, b, len) == 0;
            if (!ok) {
                uint32_t d = 0;
                while (d < len && a[d] == b[d])
                    d++;
                if (d + 4 > len)
                    d = 0;
                n64_log("monstruos: la copia al cartucho salio mal: %s (tam %ld, prueba %d en %u, distinto en +%u: cart %02x%02x%02x%02x sd %02x%02x%02x%02x)",
                    path, sz, k, (unsigned)at[k], (unsigned)d, a[d & ~3u], a[(d & ~3u) + 1], a[(d & ~3u) + 2], a[(d & ~3u) + 3],
                    b[d & ~3u], b[(d & ~3u) + 1], b[(d & ~3u) + 2], b[(d & ~3u) + 3]);
            }
        }
    }
    return ok;
}
/* f2v74: traduccion (es.gmo) en el cartucho: el motor solo guarda en la RAM un indice y los textos que
   va usando (antes ~300 KiB de RAM con el archivo entero) */
#define LANG_CART 0x12F00000u
#define LANG_MAX (1024u * 1024u)
uint32_t n64_lang_to_cart(const char *path);
uint32_t n64_lang_to_cart(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    setvbuf(f, NULL, _IONBF, 0);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t r = 0;
    if (sz > 0 && (uint32_t)sz <= LANG_MAX && fd_copy_to_cart(f, sz, LANG_CART, path))
        r = (uint32_t)sz;
    fclose(f);
    n64_log("traduccion: %s al cartucho, %u bytes (libre %d KiB)", path, (unsigned)r, heap_free_kib());
    return r;
}
void n64_lang_read(void *dst, uint32_t off, uint32_t len);
void n64_lang_read(void *dst, uint32_t off, uint32_t len)
{
    /* por un bufer alineado (el DMA del PI quiere direcciones alineadas) */
    static uint8_t tmp[2048] __attribute__((aligned(16)));
    uint8_t *d = (uint8_t *)dst;
    while (len) {
        const uint32_t a = off & ~7u;
        const uint32_t skip = off - a;
        uint32_t n = (skip + len + 7u) & ~7u;
        if (n > sizeof tmp)
            n = sizeof tmp;
        data_cache_hit_writeback_invalidate(tmp, n);
        dma_read(tmp, LANG_CART + a, n);
        uint32_t take = n - skip;
        if (take > len)
            take = len;
        memcpy(d, tmp + skip, take);
        d += take;
        off += take;
        len -= take;
    }
}

/* copia un .clx al cartucho (o lo reutiliza si ya estaba); devuelve su direccion o 0 */
uint32_t n64_mon_copy(const char *path, uint32_t *size_out);
uint32_t n64_mon_copy(const char *path, uint32_t *size_out)
{
    for (int i = 0; i < mon_count; i++) {
        if (strcmp(mon_name[i], path) == 0) {
            *size_out = mon_size[i];
            mon_reused++;
            return mon_addr[i];
        }
    }
    if (mon_count >= MON_FILES || strlen(path) >= sizeof mon_name[0])
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 16 || (uint32_t)sz > MON_CART_END - mon_w) {
        fclose(f);
        if (sz > 16)
            n64_log("monstruos: no cabe %s (%ld bytes)", path, sz);
        return 0;
    }
    const uint32_t addr = mon_w;
    int ok = fd_copy_to_cart(f, sz, addr, path);
    fclose(f);
    if (!ok)
        return 0;
    snprintf(mon_name[mon_count], sizeof mon_name[0], "%s", path);
    mon_addr[mon_count] = addr;
    mon_size[mon_count] = (uint32_t)sz;
    mon_count++;
    mon_w = (addr + (uint32_t)sz + 2047) & ~2047u;
    mon_copied_kib += (unsigned)(sz / 1024);
    *size_out = (uint32_t)sz;
    return addr;
}

/* lee bytes del cartucho a cualquier direccion de RAM (cabeceras) */
void n64_mon_read(void *dst, uint32_t addr, uint32_t len);
void n64_mon_read(void *dst, uint32_t addr, uint32_t len)
{
    static uint8_t bb[1024 + 32] __attribute__((aligned(16)));
    uint8_t *d = (uint8_t *)dst;
    while (len) {
        uint32_t base = addr & ~7u, skip = addr - base;
        uint32_t n = len > 1024 ? 1024 : len;
        cart_read(bb, base, (skip + n + 7) & ~7u);
        memcpy(d, bb + skip, n);
        d += n;
        addr += n;
        len -= n;
    }
}

/* cache en anillo de fotogramas */
#define MONC_TAGS 1024
static uint8_t *monc;
static int monc_tried;
static uint32_t monc_size, monc_w;
static uint32_t monc_key[MONC_TAGS], monc_at[MONC_TAGS];
static uintptr_t monc_trn[MONC_TAGS];
static uint8_t *monc_big;
static uint32_t monc_big_size, monc_big_key = 0xFFFFFFFFu;
static uintptr_t monc_big_trn;

void n64_mon_cache_release(void);
void n64_mon_cache_release(void)
{
    free(monc);
    monc = NULL;
    monc_tried = 0;
    monc_size = 0;
    free(monc_big);
    monc_big = NULL;
    monc_big_size = 0;
    monc_big_key = 0xFFFFFFFFu;
    for (int i = 0; i < MONC_TAGS; i++)
        monc_key[i] = 0xFFFFFFFFu;
}

static void mon_apply_trn(uint8_t *frame, uint32_t size, const uint8_t *trn)
{
    uint32_t hdr = rd16le(frame);
    if (hdr >= size)
        return;
    uint8_t *d = frame + hdr;
    uint32_t remaining = size - hdr;
    while (remaining != 0) {
        uint8_t v = *d++;
        --remaining;
        if (v < 0x80)
            continue;
        if (v <= 0xBE) {
            if (remaining == 0)
                break;
            --remaining;
            *d = trn[*d];
            d++;
        } else {
            uint32_t w = (uint8_t)(-(int8_t)v);
            if (w > remaining)
                w = remaining;
            remaining -= w;
            while (w--) {
                *d = trn[*d];
                d++;
            }
        }
    }
}

/* 'list' apunta a la cabecera de la lista en RAM; delante van la direccion de la lista en el
   cartucho y el puntero a la paleta TRN (o 0) */
const uint8_t *n64_mon_frame(const uint8_t *list, uint32_t begin, uint32_t size);
const uint8_t *n64_mon_frame(const uint8_t *list, uint32_t begin, uint32_t size)
{
    const uint32_t cart = ((const uint32_t *)list)[-2] + begin;
    const uintptr_t trn = ((const uintptr_t *)list)[-1];
    const uint32_t base = cart & ~7u, skip = cart - base;
    const uint32_t len = (skip + size + 7) & ~7u;
    if (!monc && !monc_tried) {
        monc_tried = 1;
        for (uint32_t want = 128 * 1024; want >= 32 * 1024 && !monc; want -= 32 * 1024) {
            if (heap_free_kib() * 1024 > (int)want + 200 * 1024)
                monc = (uint8_t *)memalign(16, want);
            if (monc)
                monc_size = want;
        }
        for (int i = 0; i < MONC_TAGS; i++)
            monc_key[i] = 0xFFFFFFFFu;
        monc_w = 0;
        n64_log("cache de monstruos: %u KiB (libre %d KiB)", (unsigned)(monc_size / 1024), heap_free_kib());
    }
    const uint32_t need = (len + 15) & ~15u;
    if (!monc || need > monc_size / 4) {
        if (monc_big_key == cart && monc_big_trn == trn)
            return monc_big + skip;
        if (need > monc_big_size) {
            free(monc_big);
            monc_big = (uint8_t *)memalign(16, need);
            monc_big_size = monc_big ? need : 0;
            if (!monc_big) {
                monc_big_key = 0xFFFFFFFFu;
                static uint8_t empty[8] = { 6, 0, 0, 0, 0, 0, 0, 0 };
                return empty; /* sin memoria: fotograma vacio */
            }
        }
        cart_read(monc_big, base, len);
        if (trn)
            mon_apply_trn(monc_big + skip, size, (const uint8_t *)trn);
        monc_big_key = cart;
        monc_big_trn = trn;
        monc_misses++;
        return monc_big + skip;
    }
    const uint32_t t = ((cart >> 3) * 2654435761u + (uint32_t)trn * 40503u) >> 22;
    if (monc_key[t] == cart && monc_trn[t] == trn && monc_w - monc_at[t] <= monc_size - need) {
        monc_hits++;
        return monc + monc_at[t] % monc_size + skip;
    }
    uint32_t phys = monc_w % monc_size;
    if (phys + need > monc_size) {
        monc_w += monc_size - phys; /* no partir el fotograma: saltar al principio */
        phys = 0;
    }
    uint8_t *buf = monc + phys;
    cart_read(buf, base, len);
    if (trn)
        mon_apply_trn(buf + skip, size, (const uint8_t *)trn);
    monc_key[t] = cart;
    monc_trn[t] = trn;
    monc_at[t] = monc_w;
    monc_w += need;
    monc_misses++;
    return buf + skip;
}

void n64_mon_cache_prepare(void);
void n64_mon_cache_prepare(void)
{
    /* la cache se reserva al cargar, antes que la del CEL (que se ajusta a lo que quede) */
    if (!monc) {
        monc_tried = 1;
        int fk = heap_free_kib();
        uint32_t want = fk > 900 ? 128 * 1024 : (fk > 600 ? 96 * 1024 : 64 * 1024);
        monc = (uint8_t *)memalign(16, want);
        monc_size = monc ? want : 0;
        for (int i = 0; i < MONC_TAGS; i++)
            monc_key[i] = 0xFFFFFFFFu;
        monc_w = 0;
        n64_log("cache de monstruos: %u KiB (libre antes %d KiB)", (unsigned)(monc_size / 1024), fk);
    }
}

void n64_mon_log(void);
void n64_mon_log(void)
{
    if (mon_count > 0)
        n64_mon_cache_prepare();
    n64_log("monstruos en el cartucho: %d archivos, %u KiB copiados, %u reutilizados, zona usada %u KiB",
        mon_count, mon_copied_kib, mon_reused, (unsigned)((mon_w - MON_CART) / 1024));
}


/* ---------------- efectos de sonido y voces (f2v48) ----------------
   Cada WAV del diabdat se copia de la SD a la memoria del cartucho la primera vez que suena
   (como los monstruos) y el mezclador lo lee de alli por PI. En la RAM solo queda la tabla. */
#define SFX_CART 0x10800000u
#define SFX_CART_END 0x12000000u
#define SFX_FILES 320
#define SFX_FIRST_CH 1
#define SFX_LAST_CH 7
typedef struct {
    char name[72];
    uint32_t pcm;      /* direccion en el cartucho de la primera muestra */
    uint32_t len;      /* muestras */
    uint32_t rate;
    uint8_t bits, ch, bps, bad;
} sfx_file_t;
static sfx_file_t sfx_files[SFX_FILES];
static int sfx_count;
static uint32_t sfx_w = SFX_CART;
#define SFX_BLK 2048
typedef struct {
    int file;            /* -1 libre */
    waveform_t wave;
    uint32_t started;    /* para quitar el mas viejo si no hay canal libre */
    /* f2v52: trozo del WAV ya leido del cartucho (antes un DMA cada 128 muestras) */
    uint32_t blk_off, blk_len;
    uint8_t blk[SFX_BLK + 16] __attribute__((aligned(16)));
} sfx_chan_t;
static sfx_chan_t sfx_ch[8];
static uint32_t sfx_serial;

static void sfx_fix(uint8_t *c, int bytes, int bits)
{
    if (bits == 16) {
        uint16_t *p = (uint16_t *)c;
        for (int i = 0; i < bytes / 2; i++)
            p[i] = __builtin_bswap16(p[i]);
    } else {
        for (int i = 0; i < bytes; i++)
            c[i] ^= 0x80;
    }
}

static void sfx_read2(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen);
static void sfx_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking)
{
    (void)seeking;
    uint32_t t0 = (uint32_t)get_ticks();
    sfx_read2(ctx, sbuf, wpos, wlen);
    sfx_read_us += (unsigned)TICKS_TO_US((uint32_t)get_ticks() - t0);
}
static void sfx_read2(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen)
{
    sfx_chan_t *ch = &sfx_ch[(int)(intptr_t)ctx];
    if (ch->file < 0)
        return;
    const sfx_file_t *sf = &sfx_files[ch->file];
    const uint32_t total = sf->len << sf->bps;
    while (wlen > 0) {
        int n = wlen < SAMPLEBUFFER_MARGIN_UNITS ? wlen : SAMPLEBUFFER_MARGIN_UNITS;
        uint8_t *ram = (uint8_t *)samplebuffer_append(sbuf, n);
        uint32_t off = (uint32_t)wpos << sf->bps;
        uint32_t bytes = (uint32_t)n << sf->bps;
        while (bytes > 0) {
            if (off < ch->blk_off || off >= ch->blk_off + ch->blk_len) {
                /* leer el trozo siguiente: un solo DMA de hasta 2 KiB, alineado a 8 */
                uint32_t a = (sf->pcm + off) & ~7u;
                uint32_t skip = (sf->pcm + off) - a;
                uint32_t len = SFX_BLK;
                if (off - skip + len > total + 8)
                    len = ((total - (off - skip)) + 7) & ~7u;
                if (len == 0 || len > SFX_BLK)
                    len = SFX_BLK;
                cart_read(ch->blk, a, len);
                ch->blk_off = off - skip;
                ch->blk_len = len;
                sfx_fix(ch->blk, (int)len, sf->bits);
            }
            uint32_t in = off - ch->blk_off;
            uint32_t take = ch->blk_len - in;
            if (take > bytes)
                take = bytes;
            memcpy(ram, ch->blk + in, take);
            ram += take;
            off += take;
            bytes -= take;
        }
        wlen -= n;
        wpos += n;
    }
}

static int sfx_find(const char *path)
{
    for (int i = 0; i < sfx_count; i++)
        if (strcmp(sfx_files[i].name, path) == 0)
            return i;
    return -1;
}

void n64_sfx_stop_all(void);
/* f2v64: cinematicas. Convertidas en el PC (videoconv64 de libdragon: MPEG-1 320x144 + audio wav64)
   y leidas de la SD (sd:/devilutionx/n64/cine/). Las reproduce fmv_play encima de la pantalla actual;
   A, B o START las saltan si el juego lo permite. Devuelve 0 si no esta el archivo. */
static int fmv_can_skip;
static uint32_t fmv_prev_btn;
static uint32_t fmv_buttons(void)
{
    joypad_poll();
    joypad_buttons_t b = joypad_get_buttons(JOYPAD_PORT_1);
    return (b.a ? 1u : 0u) | (b.b ? 2u : 0u) | (b.start ? 4u : 0u);
}
static void fmv_osd(void *ctx, int frame_idx, float time_sec, fmv_control_t *ctrl)
{
    (void)ctx;
    (void)frame_idx;
    (void)time_sec;
    n64_audio_pump(); /* f2v66: el sonido de la cinematica va por la musica propia */
    const uint32_t cur = fmv_buttons();
    if (fmv_can_skip && (cur & ~fmv_prev_btn))
        ctrl->stop(ctrl);
    fmv_prev_btn = cur;
}
/* f2v69: cinematicas ya vistas (para el menu Cinematicas): una por linea en saves/n64_cine.txt */
#define CINE_SEEN_FILE "sd:/devilutionx/saves/n64_cine.txt"
int n64_cine_seen(const char *name);
int n64_cine_seen(const char *name)
{
    FILE *f = fopen(CINE_SEEN_FILE, "rb");
    if (!f)
        return 0;
    char line[64];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        found = strcmp(line, name) == 0;
    }
    fclose(f);
    return found;
}
static void cine_mark_seen(const char *path)
{
    const char *b = strrchr(path, '/');
    char name[48];
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = 0;
    if (n64_cine_seen(name))
        return;
    FILE *f = fopen(CINE_SEEN_FILE, "ab");
    if (f) {
        fprintf(f, "%s\n", name);
        fclose(f);
    }
}
int n64_play_movie(const char *path, int canSkip);
int n64_play_movie(const char *path, int canSkip)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        n64_log("cinematica: no esta %s", path);
        return 0;
    }
    fclose(f);
    n64_sfx_stop_all();
    rspq_wait();
    fmv_can_skip = canSkip;
    fmv_prev_btn = fmv_buttons(); /* lo que ya estaba pulsado no la salta */
    const uint64_t t0 = get_ticks_ms();
    n64_log("cinematica: %s (libre %d KiB)", path, heap_free_kib());
    /* f2v66: con el audio de fmv (wav64) la consola se colgaba en pantalla azul al empezar el logo.
       El sonido va ahora como la musica: WAV IMA de sd:/devilutionx/n64/cine/<nombre>.wav copiado al
       cartucho, y el video sin audio */
    {
        const char *base = strrchr(path, '/');
        char wav[96];
        snprintf(wav, sizeof wav, "cine\\%s", base ? base + 1 : path);
        char *dot = strrchr(wav, '.');
        if (dot)
            strcpy(dot, ".wav");
        n64_music_start(wav);
    }
    n64_log_flush();
    /* f2v67: reproductor propio con video.h + yuv.h (sin fmv_play): con fmv_play la tele se quedaba
       sin senal (pantalla azul de la tele, sin texto). Cada paso se anota y se vuelca a la SD. */
    static int codec_ok;
    if (!codec_ok) {
        video_register_codec(&mpeg1_codec); /* f2v68: sin registrar, video_open fallaba (assert) */
        codec_ok = 1;
    }
    video_t *v = video_open(path, NULL);
    n64_log("cinematica: abierto %s", v ? "si" : "NO");
    n64_log_flush();
    if (v) {
        video_info_t info = video_get_info(v);
        n64_log("cinematica: %dx%d %.2f fps", info.width, info.height, (double)info.framerate);
        n64_log_flush();
        yuv_init();
        yuv_blitter_t yb = yuv_blitter_new_fmv(info.width, info.height, display_get_width(), display_get_height(),
            &(yuv_fmv_parms_t){ .cs = &info.colorspace, .video_aspect_ratio = info.aspect_ratio, .display_aspect_ratio = vi_get_aspect_ratio(), .enable_dithering = true });
        n64_log("cinematica: blitter listo");
        n64_log_flush();
        const float fps = info.framerate > 1.0f && info.framerate < 61.0f ? info.framerate : 30.0f;
        const uint64_t us_per = (uint64_t)(1000000.0f / fps);
        uint64_t next = TICKS_TO_US(get_ticks());
        int frame = 0, stop = 0;
        while (!stop && video_next_frame(v)) {
            if (frame < 3) {
                n64_log("cinematica: cuadro %d decodificado", frame);
                n64_log_flush();
            }
            surface_t *d = display_get();
            rdpq_attach(d, NULL);
            yuv_frame_t yf = video_get_frame(v);
            yuv_blitter_run(&yb, &yf);
            rdpq_detach_show();
            if (frame < 3) {
                n64_log("cinematica: cuadro %d mostrado", frame);
                n64_log_flush();
            }
            frame++;
            next += us_per;
            do {
                n64_audio_pump();
                const uint32_t cur = fmv_buttons();
                if (fmv_can_skip && (cur & ~fmv_prev_btn))
                    stop = 1;
                fmv_prev_btn = cur;
                if (!stop)
                    kthread_yield();
            } while (!stop && TICKS_TO_US(get_ticks()) < next);
        }
        rspq_wait();
        yuv_blitter_free(&yb);
        yuv_close();
        video_close(v);
        n64_log("cinematica: %d cuadros", frame);
        if (frame > 0)
            cine_mark_seen(path);
    }
    n64_music_stop();
    n64_log("cinematica: fin tras %u ms (libre %d KiB)", (unsigned)(get_ticks_ms() - t0), heap_free_kib());
    /* espera a soltar el boton con el que se salto (si no, el menu lo tomaria como una pulsacion) */
    const uint64_t t1 = get_ticks_ms();
    while (fmv_buttons() && get_ticks_ms() - t1 < 1500)
        kthread_yield();
    return 1;
}
static int sfx_load(const char *path)
{
    int i = sfx_find(path);
    if (i >= 0)
        return sfx_files[i].bad ? -1 : i;
    if (strlen(path) >= sizeof sfx_files[0].name)
        return -1;
    uint32_t t0 = (uint32_t)get_ticks_ms();
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 44 || sz > 8 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    if (sfx_count >= SFX_FILES || (uint32_t)sz > SFX_CART_END - sfx_w) {
        /* zona llena: se empieza de cero (lo que suena se corta) */
        n64_sfx_stop_all();
        n64_log("sonidos: zona del cartucho llena (%d archivos, %u KiB), se vacia", sfx_count, (unsigned)((sfx_w - SFX_CART) / 1024));
        sfx_count = 0;
        sfx_w = SFX_CART;
    }
    const uint32_t addr = sfx_w;
    int ok = fd_copy_to_cart(f, sz, addr, path);
    fclose(f);
    sfx_file_t *sf = &sfx_files[sfx_count];
    memset(sf, 0, sizeof *sf);
    snprintf(sf->name, sizeof sf->name, "%s", path);
    sf->bad = 1;
    if (ok) {
        /* cabecera del WAV, leida del cartucho */
        static uint8_t h[512 + 16] __attribute__((aligned(16)));
        uint32_t hn = sz < 512 ? (uint32_t)sz : 512;
        cart_read(h, addr, (hn + 7) & ~7u);
        if (memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0) {
            uint32_t p = 12, fmt = 0, data_off = 0, data_len = 0;
            while (p + 8 <= hn) {
                uint32_t clen = rd32le(h + p + 4);
                if (memcmp(h + p, "fmt ", 4) == 0 && p + 24 <= hn) {
                    fmt = rd16le(h + p + 8);
                    sf->ch = (uint8_t)rd16le(h + p + 10);
                    sf->rate = rd32le(h + p + 12);
                    sf->bits = (uint8_t)rd16le(h + p + 22);
                } else if (memcmp(h + p, "data", 4) == 0) {
                    data_off = p + 8;
                    data_len = clen;
                    break;
                }
                p += 8 + ((clen + 1) & ~1u);
            }
            if (data_off && data_len > (uint32_t)sz - data_off)
                data_len = (uint32_t)sz - data_off;
            if (fmt == 1 && (sf->ch == 1 || sf->ch == 2) && (sf->bits == 8 || sf->bits == 16) && data_off && sf->rate) {
                sf->bps = (uint8_t)((sf->bits == 16 ? 1 : 0) + (sf->ch == 2 ? 1 : 0));
                sf->pcm = addr + data_off;
                sf->len = data_len >> sf->bps;
                sf->bad = sf->len == 0;
            } else {
                n64_log("sonidos: formato no soportado %s (fmt %u, %u voces, %u bits)", path, (unsigned)fmt, sf->ch, sf->bits);
            }
        }
        sfx_w = (addr + (uint32_t)sz + 2047) & ~2047u;
    }
    i = sfx_count++;
    sfx_loaded++;
    sfx_load_ms += (uint32_t)get_ticks_ms() - t0;
    {
        const char *nm = strrchr(path, '/');
        n64_log("sonido: %s %u Hz %u bits %u voces, %u KiB, %u ms%s", nm ? nm + 1 : path, (unsigned)sf->rate, sf->bits, sf->ch,
            (unsigned)(sz / 1024), (unsigned)((uint32_t)get_ticks_ms() - t0), sf->bad ? " (MAL)" : "");
    }
    return sf->bad ? -1 : i;
}

int n64_sfx_preload(const char *path);
int n64_sfx_preload(const char *path)
{
    if (!audio_ready || !path)
        return 0;
    n64_audio_pump();
    return sfx_load(path) >= 0;
}

int n64_sfx_play(const char *path, float left, float right);
int n64_sfx_play(const char *path, float left, float right)
{
    if (!audio_ready || !path)
        return 0;
    const int fi = sfx_load(path);
    if (fi < 0)
        return 0;
    /* canal libre, o el que lleva mas tiempo sonando */
    int best = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int c = SFX_FIRST_CH; c <= SFX_LAST_CH; c++) {
        if (sfx_ch[c].file < 0 || !mixer_ch_playing(c)) {
            best = c;
            break;
        }
        if (sfx_ch[c].started < oldest) {
            oldest = sfx_ch[c].started;
            best = c;
        }
    }
    if (best < 0)
        return 0;
    sfx_chan_t *ch = &sfx_ch[best];
    if (mixer_ch_playing(best))
        mixer_ch_stop(best);
    const sfx_file_t *sf = &sfx_files[fi];
    memset(&ch->wave, 0, sizeof ch->wave);
    ch->wave.name = sf->name;
    ch->wave.bits = sf->bits;
    ch->wave.channels = sf->ch;
    ch->wave.format = WAVEFORM_FORMAT_PCM;
    ch->wave.frequency = (float)sf->rate;
    ch->wave.len = (int)sf->len;
    ch->wave.loop_len = 0;
    ch->wave.read = sfx_read;
    ch->wave.ctx = (void *)(intptr_t)best;
    ch->file = fi;
    ch->blk_off = 0;
    ch->blk_len = 0;
    ch->started = ++sfx_serial;
    mixer_ch_play(best, &ch->wave);
    mixer_ch_set_vol(best, left, right);
    sfx_played++;
    return 1;
}

int n64_sfx_is_playing(const char *path);
int n64_sfx_is_playing(const char *path)
{
    const int fi = sfx_find(path);
    if (fi < 0)
        return 0;
    for (int c = SFX_FIRST_CH; c <= SFX_LAST_CH; c++)
        if (sfx_ch[c].file == fi && mixer_ch_playing(c))
            return 1;
    return 0;
}

void n64_sfx_stop(const char *path);
void n64_sfx_stop(const char *path)
{
    const int fi = sfx_find(path);
    if (fi < 0 || !audio_ready)
        return;
    for (int c = SFX_FIRST_CH; c <= SFX_LAST_CH; c++) {
        if (sfx_ch[c].file == fi) {
            if (mixer_ch_playing(c))
                mixer_ch_stop(c);
            sfx_ch[c].file = -1;
        }
    }
}

void n64_sfx_stop_all(void)
{
    if (!audio_ready)
        return;
    for (int c = SFX_FIRST_CH; c <= SFX_LAST_CH; c++) {
        if (mixer_ch_playing(c))
            mixer_ch_stop(c);
        sfx_ch[c].file = -1;
    }
}

static void n64_sfx_init(void)
{
    for (int c = 0; c < 8; c++)
        sfx_ch[c].file = -1;
}


/* ---------------- copias de 8 bits con el RDP (f2v53) ----------------
   Copiar el suelo guardado con la CPU costaba ~10 ms (120 KiB); el RDP en modo copia lo hace en
   ~1 ms. Las dos zonas tienen que estar alineadas a 64 bytes y el ancho ser multiplo de 8. */
int n64_rdp_copy8(uint8_t *dst, int dpitch, const uint8_t *src, int spitch, int w, int h);
int n64_rdp_copy8(uint8_t *dst, int dpitch, const uint8_t *src, int spitch, int w, int h)
{
    if (rdp_copy_bad || !rdp_ready || ((uintptr_t)dst & 63) || ((uintptr_t)src & 7) || (w & 7) || (dpitch & 7) || (spitch & 7) || w <= 0 || h <= 0)
        return 0;
    data_cache_hit_writeback((void *)src, (unsigned long)spitch * (unsigned long)h);
    data_cache_hit_writeback_invalidate(dst, (unsigned long)dpitch * (unsigned long)h);
    /* f2v56: en f2v55 la copia en I8 salio MAL en la consola (fila 0 columna 17): ahora los bytes se
       copian como pixeles de 16 bits (la mitad de ancho), el uso normal del modo copia */
    surface_t d = surface_make(dst, FMT_RGBA16, (uint16_t)(w / 2), (uint16_t)h, (uint16_t)dpitch);
    surface_t sr = surface_make((void *)src, FMT_RGBA16, (uint16_t)(w / 2), (uint16_t)h, (uint16_t)spitch);
    rdpq_attach(&d, NULL);
    rdpq_set_mode_copy(false);
    rdpq_tex_blit(&sr, 0, 0, NULL);
    rdpq_detach_wait();
    if (rdp_copy_checked < 12) {
        /* f2v55: las primeras copias se comparan con el original (se lee la RAM: la cache de dst
           se invalido antes). Si alguna sale mal, el suelo se copia con la CPU. */
        rdp_copy_checked++;
        for (int y = 0; y < h; y++) {
            const uint8_t *a = dst + (size_t)y * (size_t)dpitch;
            const uint8_t *b = src + (size_t)y * (size_t)spitch;
            if (memcmp(a, b, (size_t)w) != 0) {
                int x = 0;
                while (x < w && a[x] == b[x])
                    x++;
                rdp_copy_bad++;
                n64_log("copia del RDP MAL (%dx%d, pasos %d/%d): fila %d columna %d; se copia con la CPU", w, h, dpitch, spitch, y, x);
                return 0;
            }
        }
        if (rdp_copy_checked == 12)
            n64_log("copia del RDP comprobada 12 veces: bien");
    }
    return 1;
}
/* f2v62: copia un trozo de una imagen de 8 bits a otra con desplazamiento (suelo que se mueve al
   caminar). Como pixeles de 16 bits: x y ancho pares */
int n64_rdp_blit8(uint8_t *dst, int dpitch, int dh, int dx, int dy, const uint8_t *src, int spitch, int sh, int sx, int sy, int w, int h);
int n64_rdp_blit8(uint8_t *dst, int dpitch, int dh, int dx, int dy, const uint8_t *src, int spitch, int sh, int sx, int sy, int w, int h)
{
    if (rdp_copy_bad || !rdp_ready || ((uintptr_t)dst & 63) || ((uintptr_t)src & 7) || (dpitch & 7) || (spitch & 7)
        || ((dx | sx | w) & 1) || w <= 0 || h <= 0)
        return 0;
    data_cache_hit_writeback((void *)src, (unsigned long)spitch * (unsigned long)sh);
    data_cache_hit_writeback_invalidate(dst, (unsigned long)dpitch * (unsigned long)dh);
    surface_t d = surface_make(dst, FMT_RGBA16, (uint16_t)(dpitch / 2), (uint16_t)dh, (uint16_t)dpitch);
    surface_t sr = surface_make((void *)src, FMT_RGBA16, (uint16_t)(spitch / 2), (uint16_t)sh, (uint16_t)spitch);
    rdpq_attach(&d, NULL);
    rdpq_set_mode_copy(false);
    rdpq_blitparms_t bp = { .s0 = sx / 2, .t0 = sy, .width = w / 2, .height = h };
    rdpq_tex_blit(&sr, dx / 2, dy, &bp);
    rdpq_detach_wait();
    return 1;
}
int n64_rdp_fill8(uint8_t *dst, int dpitch, int w, int h, int value);
int n64_rdp_fill8(uint8_t *dst, int dpitch, int w, int h, int value)
{
    if (rdp_fill_bad || value != 0 || !rdp_ready || ((uintptr_t)dst & 63) || (w & 7) || (dpitch & 7) || w <= 0 || h <= 0)
        return 0;
    data_cache_hit_writeback_invalidate(dst, (unsigned long)dpitch * (unsigned long)h);
    surface_t d = surface_make(dst, FMT_RGBA16, (uint16_t)(w / 2), (uint16_t)h, (uint16_t)dpitch);
    rdpq_attach(&d, NULL);
    rdpq_set_mode_fill(RGBA32(0, 0, 0, 0));
    rdpq_fill_rectangle(0, 0, w / 2, h);
    rdpq_detach_wait();
    if (rdp_fill_checked < 6) {
        rdp_fill_checked++;
        for (int y = 0; y < h; y++) {
            const uint8_t *a = dst + (size_t)y * (size_t)dpitch;
            for (int x = 0; x < w; x++) {
                if (a[x] != 0) {
                    rdp_fill_bad++;
                    n64_log("borrado del RDP MAL: fila %d columna %d; se borra con la CPU", y, x);
                    return 0;
                }
            }
        }
    }
    return 1;
}

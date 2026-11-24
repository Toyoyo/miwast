/* menu.c - the game menu, the save and load screens, and the confirmation
 * dialog, drawn by hand: there is no screen language here.
 *
 * They follow the game's own screens.rpy where there is something to
 * follow: its navigation labels (Sauvegarde, Charger, Quitter), its 3x2
 * grid of slots with a thumbnail each (gui.rpy:265), and Ren'Py's French
 * strings for the confirmations (tutorial/game/tl/french/common.rpy) --
 * or all of it in English, by the language preference. The Atari font has
 * no Ê, so "Etes-vous", as French capitals often are.
 *
 * Esc or the right button opens the menu and backs out of it, as Ren'Py's
 * game_menu key does. Keyboard and mouse both work: the pointer is shown
 * while a menu is up, and hovering selects.
 *
 * Everything is drawn into the back screen and flipped, so nothing is seen
 * half-drawn; the pointer is hidden across the flip, because it is drawn
 * into the screen being shown and puts back what was under it there
 * (gfx.c).
 *
 * The saves are one file, MIWA.SAV, beside the program: a header naming
 * the build of the script they belong to, then SAVE_SLOTS fixed records.
 */

#include <string.h>
#include <stdlib.h>
#include <mint/osbind.h>

#include "st.h"

#define SAVE_FILE   "MIWA.SAV"
#define SAVE_MAGIC  0x4D575356UL        /* "MWSV" */
#define PER_PAGE    6                   /* gui.file_slot_cols x _rows */
#define NPAGES      5
#define SAVE_SLOTS  (PER_PAGE * NPAGES)
#define HDR_BYTES   12

/* Scancodes and characters. */
#define SC_UP     0x48
#define SC_DOWN   0x50
#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_DELETE 0x53
#define SC_UNDO   0x61

/* The strings, in each language. The English ones are the game's own
 * (tl/english/screens.rpy) and Ren'Py's originals of the confirmations,
 * which the French ones translate (tl/None/common.rpym). Restart stands for
 * the game's "Menu principal" (screens.rpy:323) then "Lecture": there is no
 * main menu here. Its question is Ren'Py's for the main menu
 * (00gui.rpy:453, tl/None/common.rpym:518), reworded for it.
 *
 * Atari charset: ç 0x87, é 0x82, è 0x8A, î 0x8C, û 0x96. A hex escape runs
 * on into a following hex digit, hence the split strings. */
enum {
    S_RESUME, S_SAVE, S_LOAD, S_SPEED, S_LANG, S_QUIT, S_YES, S_NO, S_EMPTY,
    Q_QUIT, Q_OVERWRITE, Q_DELETE, Q_LOAD, S_HELP, S_RESTART, Q_RESTART,
    S_NSTR
};

static const char *const g_ui[MWB_NLANG][S_NSTR] = {
    {
        "Reprendre", "Sauvegarde", "Charger", "Vitesse du texte", "Langue",
        "Quitter", "Oui", "Non", "Emplacement vide.",
        "Etes-vous s\x96r de vouloir quitter ?",
        "Etes-vous s\x96r de vouloir \x82" "craser cette sauvegarde ?",
        "Etes-vous s\x96r de vouloir supprimer cette sauvegarde ?",
        "Le chargement entra\x8cnera la perte de votre progression depuis la "
        "derni\x8a" "re sauvegarde. Etes-vous s\x96r de vouloir faire cela ?",
        "Fl\x8a" "ches : choisir   Entr\x82" "e : valider   "
        "Suppr : effacer   Echap : retour",
        "Recommencer",
        "Etes-vous s\x96r de vouloir recommencer depuis le d\x82" "but ? "
        "Votre progression depuis la derni\x8a" "re sauvegarde sera perdue."
    }, {
        "Resume", "Save", "Load", "Text Speed", "Language",
        "Quit", "Yes", "No", "Empty slot.",
        "Are you sure you want to quit?",
        "Are you sure you want to overwrite your save?",
        "Are you sure you want to delete this save?",
        "Loading will lose unsaved progress. "
        "Are you sure you want to do this?",
        "Arrows: choose   Return: confirm   Delete: erase   Esc: back",
        "Restart",
        "Are you sure you want to restart from the beginning? "
        "This will lose unsaved progress."
    }
};

/* The languages by their own names, as the game's flag buttons show them. */
static const char *const g_lang_label[MWB_NLANG] = {
    "Fran\x87" "ais", "English"
};

#define UI(s) (g_ui[g_lang][s])

/* ---- drawing ---- */

static void text(u8 *scr, int x, int y, const char *s, int style)
{
    gfx_text(scr, x / 8, y, (const u8 *)s, (int)strlen(s), style);
}

/* Centred between pixel columns x0 and x1. */
static void text_mid(u8 *scr, int x0, int x1, int y, const char *s,
                     int style)
{
    int len = (int)strlen(s);
    text(scr, (x0 + x1) / 2 - len * 4, y, s, style);
}

static void show(void)
{
    gfx_pointer(0);
    gfx_flip();
    gfx_pointer(1);
}

static int inside(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

/* Up to `max` lines of at most `width` characters, broken at spaces. */
static int wrap(const char *s, int width, const char **start, int *len,
                int max)
{
    int n = 0;

    while (*s && n < max) {
        int l = (int)strlen(s), cut;
        if (l > width) {
            for (cut = width; cut > 0 && s[cut] != ' '; cut--)
                ;
            if (cut == 0)
                cut = width;
            l = cut;
        }
        start[n] = s;
        len[n] = l;
        n++;
        s += l;
        while (*s == ' ')
            s++;
    }
    return n;
}

/* ---- input ---- */

typedef struct ui {
    long key;                   /* Bconin's value, or 0 */
    int  c, sc;                 /* its character and scancode */
    int  mx, my, moved;         /* the pointer, and whether it moved */
    int  click, rclick;         /* a button went down */
} ui;

static int g_ui_buttons, g_ui_mx = -1, g_ui_my = -1;

static void ui_reset(void)
{
    g_ui_buttons = gfx_mouse_buttons();
    gfx_mouse_pos(&g_ui_mx, &g_ui_my);
}

/* The next thing the player did; waits a VBL at a time for it. */
static void ui_wait(ui *u)
{
    for (;;) {
        int b, pressed;

        u->key = read_key();
        u->c = (int)(u->key & 0xFF);
        u->sc = (int)((u->key >> 16) & 0xFF);
        b = gfx_mouse_buttons();
        pressed = b & ~g_ui_buttons;
        g_ui_buttons = b;
        u->click = pressed & 1;
        u->rclick = (pressed & 2) != 0;
        gfx_mouse_pos(&u->mx, &u->my);
        u->moved = u->mx != g_ui_mx || u->my != g_ui_my;
        g_ui_mx = u->mx;
        g_ui_my = u->my;
        if (u->key || u->click || u->rclick || u->moved)
            return;
        Vsync();
    }
}

static int is_back(const ui *u)
{
    return u->c == 0x1B || u->sc == SC_UNDO || u->rclick;
}

static int is_ok(const ui *u)
{
    return u->c == '\r' || u->c == ' ';
}

/* ---- the confirmation dialog ---- */

#define DLG_X0 112
#define DLG_X1 528

/* Yes or no to `q`. The rectangle it covers is put back on both screens
 * afterwards, so whatever it was over need not be redrawn. Oui is
 * selected to begin with; Esc, the right button and N are Non. */
static int confirm(const char *q)
{
    const char *ls[6];
    int ll[6], n = wrap(q, (DLG_X1 - DLG_X0) / 8 - 4, ls, ll, 6);
    int y0 = 140 - n * 5, y1 = y0 + 24 + n * 10 + 32;
    int by = y1 - 28, sel = 0, i, r = -1;
    int bx[2] = { 232, 344 };           /* Oui, Non: 64px buttons */
    u32 rect = (u32)(y1 - y0) * LINE_BYTES;
    u8 *saved = malloc(rect);
    ui u;

    /* The pointer is drawn into the shown screen: off while copying it. */
    gfx_pointer(0);
    if (saved)
        memcpy(saved, gfx_front() + (u32)y0 * LINE_BYTES, rect);
    memcpy(gfx_back(), gfx_front(), SCREEN_BYTES);
    gfx_pointer(1);
    ui_reset();
    while (r < 0) {
        u8 *scr = gfx_back();

        gfx_box(scr, DLG_X0, y0, DLG_X1, y1);
        gfx_box(scr, DLG_X0 + 2, y0 + 2, DLG_X1 - 2, y1 - 2);
        for (i = 0; i < n; i++) {
            char line[64];
            memcpy(line, ls[i], (size_t)ll[i]);
            line[ll[i]] = 0;
            text_mid(scr, DLG_X0, DLG_X1, y0 + 14 + i * 10, line, 0);
        }
        for (i = 0; i < 2; i++) {
            gfx_box(scr, bx[i], by, bx[i] + 64, by + 16);
            text_mid(scr, bx[i], bx[i] + 64, by + 4, UI(i ? S_NO : S_YES),
                     GFX_BOLD);
            if (i == sel)
                gfx_rect(scr, bx[i] + 2, by + 2, bx[i] + 62, by + 14,
                         BLT_OP_NOT_D);
        }
        show();
        for (;;) {
            ui_wait(&u);
            if (is_back(&u) || u.c == 'n' || u.c == 'N') {
                r = 0;
            } else if (u.c == 'o' || u.c == 'O' || u.c == 'y' ||
                       u.c == 'Y') {
                r = 1;
            } else if (is_ok(&u)) {
                r = sel == 0;
            } else if (u.sc == SC_LEFT || u.sc == SC_RIGHT) {
                sel ^= 1;
            } else if (u.moved || u.click) {
                int hit = -1;
                for (i = 0; i < 2; i++)
                    if (inside(u.mx, u.my, bx[i], by, bx[i] + 64, by + 16))
                        hit = i;
                if (hit < 0)
                    continue;
                if (u.click) {
                    r = hit == 0;
                } else if (hit != sel) {
                    sel = hit;
                } else {
                    continue;
                }
            } else {
                continue;
            }
            break;
        }
    }

    /* Put back what it covered, shown screen first so the flip that
     * follows cannot show a half-restored one. */
    if (saved) {
        gfx_pointer(0);
        memcpy(gfx_front() + (u32)y0 * LINE_BYTES, saved, rect);
        memcpy(gfx_back() + (u32)y0 * LINE_BYTES, saved, rect);
        gfx_pointer(1);
        free(saved);
    }
    return r;
}

/* For the rhythm game's quit dialog (rhythm_game_quit_confirm, whose Oui
 * and Non are these too). */
int menu_confirm(const char *q)
{
    return confirm(q);
}

/* ---- the save file ---- */

static long save_open_read(void)
{
    u32 hdr[3];
    long fd = Fopen(SAVE_FILE, 0);

    if (fd < 0)
        return -1;
    if (Fread((short)fd, HDR_BYTES, hdr) != HDR_BYTES ||
        hdr[0] != SAVE_MAGIC || hdr[1] != g_script_sig ||
        hdr[2] != (((u32)SAVE_SLOTS << 16) | sizeof(save_rec))) {
        Fclose((short)fd);
        return -1;
    }
    return fd;
}

/* A page's records; unused ones, or all of them if there is no file or
 * it belongs to another build of the script, have used = 0. */
static void read_page(int page, save_rec *recs)
{
    long fd = save_open_read();
    long want = (long)PER_PAGE * (long)sizeof(save_rec);

    memset(recs, 0, (size_t)want);
    if (fd < 0)
        return;
    Fseek(HDR_BYTES + (long)page * want, (short)fd, 0);
    if (Fread((short)fd, want, recs) != want)
        memset(recs, 0, (size_t)want);
    Fclose((short)fd);
}

/* After the slots, the preferences: Ren'Py keeps them apart from any save,
 * and so do they here -- one record, read at start, whatever build of the
 * script the slots belong to. A file from before it simply ends early, and
 * the defaults stand; one from before the language has the text speed
 * only. */
#define PREF_OFF  (HDR_BYTES + (long)SAVE_SLOTS * (long)sizeof(save_rec))
#define PREF_TAG  0x5052UL                  /* "PR" */
#define PREF_BYTES 6                        /* tag, cps, language */

int g_text_cps = 50;                        /* options.rpy:128 */
#ifdef MW_LANG
int g_lang = MW_LANG;                       /* test builds */
#else
int g_lang = MWB_LANG_FR;                   /* None: the script's own */
#endif

static int write_pref(long fd)
{
    u16 p[3];

    p[0] = (u16)PREF_TAG;
    p[1] = (u16)g_text_cps;
    p[2] = (u16)g_lang;
    Fseek(PREF_OFF, (short)fd, 0);
    return Fwrite((short)fd, PREF_BYTES, p) == PREF_BYTES;
}

/* A new, empty file for this script, open for writing; -1 if not. */
static long create_file(void)
{
    save_rec *zero = calloc(1, sizeof(save_rec));
    u32 hdr[3];
    long fd = Fcreate(SAVE_FILE, 0);
    int i, ok;

    if (fd < 0 || !zero) {
        free(zero);
        if (fd >= 0)
            Fclose((short)fd);
        return -1;
    }
    hdr[0] = SAVE_MAGIC;
    hdr[1] = g_script_sig;
    hdr[2] = ((u32)SAVE_SLOTS << 16) | sizeof(save_rec);
    ok = Fwrite((short)fd, HDR_BYTES, hdr) == HDR_BYTES;
    for (i = 0; ok && i < SAVE_SLOTS; i++)
        ok = Fwrite((short)fd, sizeof(save_rec), zero) ==
             (long)sizeof(save_rec);
    free(zero);
    if (ok)
        ok = write_pref(fd);
    if (!ok) {
        Fclose((short)fd);
        return -1;
    }
    return fd;
}

void menu_init(void)
{
    u32 magic;
    u16 p[3];
    long fd = Fopen(SAVE_FILE, 0), n = 0;

    if (fd < 0)
        return;
    if (Fread((short)fd, 4, &magic) == 4 && magic == SAVE_MAGIC &&
        Fseek(PREF_OFF, (short)fd, 0) == PREF_OFF)
        n = Fread((short)fd, PREF_BYTES, p);
    if (n >= 4 && p[0] == PREF_TAG && p[1] <= 200)
        g_text_cps = p[1];
    if (n >= 6 && p[0] == PREF_TAG && p[2] < MWB_NLANG)
        g_lang = p[2];
    Fclose((short)fd);
    trace("menu: text speed %d, language %d", g_text_cps, g_lang);
}

/* The preferences into the file -- any file of ours, or a new one. */
static void save_pref(void)
{
    u32 magic = 0;
    long fd = Fopen(SAVE_FILE, 2);

    if (fd >= 0 && (Fread((short)fd, 4, &magic) != 4 ||
                    magic != SAVE_MAGIC)) {
        Fclose((short)fd);
        fd = -1;
    }
    if (fd < 0) {
        fd = create_file();
        if (fd < 0)
            return;
    } else if (!write_pref(fd)) {
        trace("menu: cannot write the preferences");
    }
    Fclose((short)fd);
}

/* Write slot `slot`, making the file -- empty -- first if there is none
 * for this script. */
static int write_slot(int slot, const save_rec *r)
{
    long fd = save_open_read();
    long ok;

    if (fd >= 0) {
        Fclose((short)fd);
        fd = Fopen(SAVE_FILE, 2);
    } else {
        fd = create_file();
    }
    if (fd < 0)
        return 0;
    Fseek(HDR_BYTES + (long)slot * (long)sizeof(save_rec), (short)fd, 0);
    ok = Fwrite((short)fd, sizeof(save_rec), r) == (long)sizeof(save_rec);
    Fclose((short)fd);
    return (int)ok;
}

/* Save `cur` in slot `slot`, dated now, with the rollback log beside it
 * (vm.c log_write). */
int save_put(int slot, const save_rec *cur)
{
    save_rec *w = malloc(sizeof(save_rec));
    int ok = 0;

    if (!w)
        return 0;
    *w = *cur;
    w->date = (u16)Tgetdate();
    w->time = (u16)Tgettime();
    if (!write_slot(slot, w))
        trace("menu: cannot write %s", SAVE_FILE);
    else
        ok = log_write(slot, w);
    free(w);
    return ok;
}

/* Slot `slot` as it is in the file: used = 0 if it is empty. */
void save_get(int slot, save_rec *r)
{
    long fd = save_open_read();

    memset(r, 0, sizeof(*r));
    if (fd < 0)
        return;
    Fseek(HDR_BYTES + (long)slot * (long)sizeof(save_rec), (short)fd, 0);
    if (Fread((short)fd, sizeof(save_rec), r) != (long)sizeof(save_rec))
        memset(r, 0, sizeof(*r));
    Fclose((short)fd);
}

/* ---- the save and load screens ---- */

/* Cell c, r of the 3x2 grid: 208px wide; the thumbnail 24px in. */
#define CELL_X(c)  (16 + (c) * 208)
#define CELL_Y(r)  (40 + (r) * 150)
#define THUMB_X(c) (CELL_X(c) + 24)
#define FOOT_Y     340

static void put2(char *p, int v)
{
    p[0] = (char)('0' + v / 10 % 10);
    p[1] = (char)('0' + v % 10);
}

/* A slot's date as the game's FileTime shows it -- "%A %d %B %Y, %H:%M"
 * (screens.rpy:657), "%A, %B %d %Y, %H:%M" in English
 * (tl/english/screens.rpy:91) -- but with Ren'Py's short names, %a and %b
 * (00action_file.rpy:94-99): the long ones run to 35 characters, and a
 * slot is 26 wide. "Dim 15 Nov 2026, 14:03", "Sun, Nov 15 2026, 14:03".
 * The French names are the game's (tl/None/common.rpym); Monday first, as
 * Ren'Py's tables. */
static const char *const g_wday[MWB_NLANG][7] = {
    { "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam", "Dim" },
    { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" }
};
static const char *const g_month[MWB_NLANG][12] = {
    { "Jan", "F\x82v", "Mar", "Avr", "Mai", "Juin", "Juil", "Ao\x96",
      "Sep", "Oct", "Nov", "D\x82" "c" },
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
      "Sep", "Oct", "Nov", "Dec" }
};
static const char g_day_first[MWB_NLANG] = { 1, 0 };

/* Monday 0 (Sakamoto's method, which counts from Sunday). */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d + 6) % 7;
}

static char *put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

/* From GEMDOS's packed date and time; `d` holds 24 and the NUL. */
static void slot_date(char *d, u16 date, u16 time)
{
    int day = date & 31, mon = (date >> 5) & 15, yr = 1980 + (date >> 9);
    const char *mn;
    char *p = d;

    if (mon < 1 || mon > 12)
        mon = 1;                        /* not from Tgetdate */
    mn = g_month[g_lang][mon - 1];
    p = put_s(p, g_wday[g_lang][weekday(yr, mon, day)]);
    if (g_day_first[g_lang]) {
        *p++ = ' ';
        put2(p, day);
        p += 2;
        *p++ = ' ';
        p = put_s(p, mn);
    } else {
        *p++ = ',';
        *p++ = ' ';
        p = put_s(p, mn);
        *p++ = ' ';
        put2(p, day);
        p += 2;
    }
    *p++ = ' ';
    put2(p, yr / 100);
    put2(p + 2, yr);
    p += 4;
    *p++ = ',';
    *p++ = ' ';
    put2(p, time >> 11);
    p[2] = ':';
    put2(p + 3, (time >> 5) & 63);
    p[5] = 0;
}

static void draw_slot(u8 *scr, int c, int r, const save_rec *s, int sel)
{
    int x = THUMB_X(c), y = CELL_Y(r);

    gfx_box(scr, x - 2, y - 2, x + THUMB_W + 2, y + THUMB_H + 2);
    if (s->used) {
        char d[32];
        const char *ls[2];
        char line[32];
        int ll[2], n, i;

        gfx_put_thumb(scr, x / 8, y, s->thumb);
        slot_date(d, s->date, s->time);
        text_mid(scr, x, x + THUMB_W, y + THUMB_H + 8, d, GFX_BOLD);
        n = wrap((const char *)s->text, 24, ls, ll, 2);
        for (i = 0; i < n; i++) {
            memcpy(line, ls[i], (size_t)ll[i]);
            line[ll[i]] = 0;
            text_mid(scr, CELL_X(c), CELL_X(c) + 208,
                     y + THUMB_H + 20 + i * 9, line, 0);
        }
    } else {
        text_mid(scr, x, x + THUMB_W, y + THUMB_H / 2 - 4, UI(S_EMPTY), 0);
    }
    if (sel) {
        /* A thick frame around the whole cell. */
        int x0 = CELL_X(c) + 8, x1 = CELL_X(c) + 200;
        int y0 = y - 8, y1 = y + THUMB_H + 46;
        gfx_rect(scr, x0, y0, x1, y0 + 3, BLT_OP_ONE);
        gfx_rect(scr, x0, y1 - 3, x1, y1, BLT_OP_ONE);
        gfx_rect(scr, x0, y0, x0 + 3, y1, BLT_OP_ONE);
        gfx_rect(scr, x1 - 3, y0, x1, y1, BLT_OP_ONE);
    }
}

static void draw_files(const char *title, int page, const save_rec *recs,
                       int sel)
{
    u8 *scr = gfx_back();
    char foot[24] = "<   Page 1 / 5   >";
    int i;

    memset(scr, 0, SCREEN_BYTES);
    text_mid(scr, 0, 640, 12, title, GFX_BOLD);
    gfx_rect(scr, 16, 24, 624, 25, BLT_OP_ONE);
    for (i = 0; i < PER_PAGE; i++)
        draw_slot(scr, i % 3, i / 3, &recs[i], i == sel);
    foot[9] = (char)('1' + page);
    foot[13] = (char)('0' + NPAGES);
    gfx_rect(scr, 16, FOOT_Y - 8, 624, FOOT_Y - 7, BLT_OP_ONE);
    text_mid(scr, 0, 640, FOOT_Y + 4, foot, GFX_BOLD);
    text_mid(scr, 0, 640, FOOT_Y + 32, UI(S_HELP), 0);
    show();
}

/* The slot under the pointer, or -1. */
static int slot_at(int x, int y)
{
    int i;
    for (i = 0; i < PER_PAGE; i++) {
        int c = i % 3, r = i / 3;
        if (inside(x, y, CELL_X(c) + 8, CELL_Y(r) - 8, CELL_X(c) + 200,
                   CELL_Y(r) + THUMB_H + 46))
            return i;
    }
    return -1;
}

/* The page last looked at, kept between visits as Ren'Py does. */
static int g_page;

/* Save mode writes `cur` into the chosen slot and stays; load mode fills
 * `loaded` and returns 1. Returns 0 when the player backs out. */
static int files(int saving, const save_rec *cur, save_rec *loaded,
                 int *slot)
{
    save_rec *recs = malloc(PER_PAGE * sizeof(save_rec));
    int sel = 0, redraw = 1, page_changed = 1, result = 0;
    ui u;

    if (!recs)
        return 0;
    ui_reset();
    for (;;) {
        int act = -1, del = -1;

        if (page_changed) {
            read_page(g_page, recs);
            page_changed = 0;
        }
        if (redraw) {
            draw_files(UI(saving ? S_SAVE : S_LOAD), g_page, recs, sel);
            redraw = 0;
        }
        ui_wait(&u);
        if (is_back(&u))
            break;
        if (is_ok(&u)) {
            act = sel;
        } else if (u.sc == SC_DELETE || u.c == 0x7F) {
            del = sel;
        } else if (u.sc == SC_LEFT || u.sc == SC_RIGHT) {
            int col = sel % 3 + (u.sc == SC_LEFT ? -1 : 1);
            if (col < 0 || col > 2) {
                g_page = (g_page + NPAGES + (col < 0 ? -1 : 1)) % NPAGES;
                col = col < 0 ? 2 : 0;
                page_changed = 1;
            }
            sel = sel / 3 * 3 + col;
            redraw = 1;
        } else if (u.sc == SC_UP || u.sc == SC_DOWN) {
            sel = (sel + 3) % PER_PAGE;
            redraw = 1;
        } else if (u.moved || u.click) {
            int hit = slot_at(u.mx, u.my);
            if (hit >= 0 && hit != sel) {
                sel = hit;
                redraw = 1;
            }
            if (u.click) {
                if (hit >= 0) {
                    act = hit;
                } else if (inside(u.mx, u.my, 0, FOOT_Y - 4, 640,
                                  FOOT_Y + 16)) {
                    g_page = (g_page + NPAGES + (u.mx < 320 ? -1 : 1)) %
                             NPAGES;
                    page_changed = redraw = 1;
                }
            }
        }

        if (act >= 0) {
            save_rec *s = &recs[act];
            if (saving) {
                if (!s->used || confirm(UI(Q_OVERWRITE))) {
                    save_put(g_page * PER_PAGE + act, cur);
                    page_changed = 1;
                }
                redraw = 1;
            } else if (s->used) {
                if (confirm(UI(Q_LOAD))) {
                    *loaded = *s;
                    *slot = g_page * PER_PAGE + act;
                    result = 1;
                    break;
                }
            }
        } else if (del >= 0 && recs[del].used) {
            if (confirm(UI(Q_DELETE))) {
                save_rec *z = calloc(1, sizeof(save_rec));
                if (z) {
                    write_slot(g_page * PER_PAGE + del, z);
                    log_delete(g_page * PER_PAGE + del);
                    free(z);
                }
                page_changed = 1;
                redraw = 1;
            }
        }
    }
    free(recs);
    return result;
}

/* ---- the choice screen ----
 *
 * The game's `screen choice` (Hinata's screens.rpy:204, as Ren'Py's
 * template): a vbox of buttons, centred across the stage. Its middle is at
 * ypos 270 of 720 there, above Ren'Py's textbox, which covers the stage's
 * bottom; here the text has its own strip below the stage, so the middle
 * is the stage's (the user's choice). Each gui.choice_button_width 790 wide
 * (400 here, or as wide as the longest option needs) and
 * gui.choice_spacing 22 apart (11). A button is a box with its line in
 * the middle; the focused one is inverted. vm.c runs it. */
#define CH_MID   (MWB_STAGE_H / 2)
#define CH_H     16
#define CH_STEP  (CH_H + 11)
#define CH_MINW  400

/* Characters a label shows: style codes take no room. */
static int label_len(const u8 *s)
{
    int n = 0;
    for (; *s; s++)
        if (*s < MWB_TXT_ITALIC_ON || *s > MWB_TXT_CODE_LAST)
            n++;
    return n;
}

static void ch_layout(const u8 *const *label, int n, int *x0, int *x1,
                      int *y0)
{
    int i, w = CH_MINW;

    for (i = 0; i < n; i++)
        if ((label_len(label[i]) + 4) * 8 > w)
            w = (label_len(label[i]) + 4) * 8;
    w = (w + 15) & ~15;
    *x0 = MWB_STAGE_W / 2 - w / 2;
    *x1 = *x0 + w;
    *y0 = CH_MID - (n * CH_STEP - (CH_STEP - CH_H)) / 2;
    if (*y0 < 4)
        *y0 = 4;
}

void choice_draw(u8 *scr, const u8 *const *label, int n, int sel)
{
    int x0, x1, y0, i;

    ch_layout(label, n, &x0, &x1, &y0);
    for (i = 0; i < n; i++, y0 += CH_STEP) {
        int len = label_len(label[i]);
        gfx_box(scr, x0, y0, x1, y0 + CH_H);
        gfx_text(scr, ((x0 + x1) / 2 - len * 4) / 8, y0 + 4, label[i],
                 (int)strlen((const char *)label[i]), 0);
        if (i == sel)
            gfx_rect(scr, x0 + 2, y0 + 2, x1 - 2, y0 + CH_H - 2,
                     BLT_OP_NOT_D);
    }
}

int choice_at(int x, int y, const u8 *const *label, int n)
{
    int x0, x1, y0, i;

    ch_layout(label, n, &x0, &x1, &y0);
    for (i = 0; i < n; i++, y0 += CH_STEP)
        if (inside(x, y, x0, y0, x1, y0 + CH_H))
            return i;
    return -1;
}

/* ---- the game menu ---- */

/* The box, and its rows: the game's navigation labels, and the text speed
 * from its preferences screen (screens.rpy:782) -- Ren'Py's bar, 0 to 200
 * characters a second with the far end meaning instant
 * (00preferences.rpy:347, max_is_zero). Here in steps of 10. Then the
 * language: the full game's navigation has a flag for each (screens.rpy,
 * `Language("english")`, `Language(None)`); here each is named in itself,
 * the one in force marked. */
#define GM_X0 200
#define GM_X1 440
#define GM_Y0 97
#define GM_ITEMS 7
#define GM_SPEED 3
#define GM_LANG  4
#define GM_RESTART 5
#define GM_QUIT  6
#define BAR_X0 232
#define BAR_X1 408
static const int g_gm_y[GM_ITEMS] = { 117, 139, 161, 183, 219, 255, 277 };
#define GM_Y1 (GM_Y0 + 202)
#define ROW_Y0(i) (g_gm_y[i] - 6)
#define ROW_Y1(i) (g_gm_y[i] + ((i) == GM_SPEED || (i) == GM_LANG ? 26 : 14))
#define BAR_Y (g_gm_y[GM_SPEED] + 12)
#define LANG_Y (g_gm_y[GM_LANG] + 11)
#define LANG_W 80
static const int g_lang_x[MWB_NLANG] = { 236, 324 };

static int speed_bar(void)
{
    return g_text_cps ? g_text_cps : 200;
}

static void set_speed_bar(int v)
{
    v = (v + 5) / 10 * 10;
    if (v < 10)
        v = 10;
    if (v > 200)
        v = 200;
    g_text_cps = v == 200 ? 0 : v;
}

static void draw_game_menu(int sel)
{
    static const int items[GM_ITEMS] = {
        S_RESUME, S_SAVE, S_LOAD, S_SPEED, S_LANG, S_RESTART, S_QUIT
    };
    u8 *scr = gfx_back();
    int i;

    gfx_box(scr, GM_X0, GM_Y0, GM_X1, GM_Y1);
    gfx_box(scr, GM_X0 + 2, GM_Y0 + 2, GM_X1 - 2, GM_Y1 - 2);
    for (i = 0; i < GM_ITEMS; i++)
        text_mid(scr, GM_X0, GM_X1, g_gm_y[i], UI(items[i]), GFX_BOLD);
    gfx_box(scr, BAR_X0, BAR_Y, BAR_X1, BAR_Y + 8);
    gfx_rect(scr, BAR_X0 + 2, BAR_Y + 2,
             BAR_X0 + 2 + (BAR_X1 - BAR_X0 - 4) * speed_bar() / 200,
             BAR_Y + 6, BLT_OP_ONE);
    for (i = 0; i < MWB_NLANG; i++) {
        if (i == g_lang)
            gfx_box(scr, g_lang_x[i], LANG_Y, g_lang_x[i] + LANG_W,
                    LANG_Y + 14);
        text_mid(scr, g_lang_x[i], g_lang_x[i] + LANG_W, LANG_Y + 3,
                 g_lang_label[i], i == g_lang ? GFX_BOLD : 0);
    }
    gfx_rect(scr, GM_X0 + 8, ROW_Y0(sel), GM_X1 - 8, ROW_Y1(sel),
             BLT_OP_NOT_D);
    show();
}

int menu_run(const save_rec *cur, save_rec *loaded, int *slot)
{
    int sel = 0, result = -1, cps0 = g_text_cps, lang0 = g_lang;
    ui u;

    /* Over the frame the player was looking at. */
    memcpy(gfx_back(), gfx_front(), SCREEN_BYTES);
    draw_game_menu(sel);
    ui_reset();

    while (result < 0) {
        int act = -1, redraw = 0;

        ui_wait(&u);
        if (is_back(&u)) {
            result = MENU_RESUME;
            break;
        }
        if (is_ok(&u)) {
            act = sel;
        } else if (u.sc == SC_UP || u.sc == SC_DOWN) {
            sel = (sel + (u.sc == SC_UP ? GM_ITEMS - 1 : 1)) % GM_ITEMS;
            redraw = 1;
        } else if ((u.sc == SC_LEFT || u.sc == SC_RIGHT) && sel == GM_SPEED) {
            set_speed_bar(speed_bar() + (u.sc == SC_LEFT ? -10 : 10));
            redraw = 1;
        } else if ((u.sc == SC_LEFT || u.sc == SC_RIGHT) && sel == GM_LANG) {
            g_lang = u.sc == SC_LEFT ? 0 : MWB_NLANG - 1;
            redraw = 1;
        } else if (u.moved || u.click) {
            int i;
            for (i = 0; i < GM_ITEMS; i++)
                if (inside(u.mx, u.my, GM_X0 + 8, ROW_Y0(i), GM_X1 - 8,
                           ROW_Y1(i)))
                    break;
            if (i < GM_ITEMS) {
                if (i != sel) {
                    sel = i;
                    redraw = 1;
                }
                if (u.click && i == GM_SPEED) {
                    if (inside(u.mx, u.my, BAR_X0, BAR_Y - 3, BAR_X1,
                               BAR_Y + 11)) {
                        set_speed_bar((u.mx - BAR_X0) * 200 /
                                      (BAR_X1 - BAR_X0));
                        redraw = 1;
                    }
                } else if (u.click && i == GM_LANG) {
                    int l;
                    for (l = 0; l < MWB_NLANG; l++)
                        if (inside(u.mx, u.my, g_lang_x[l], LANG_Y - 3,
                                   g_lang_x[l] + LANG_W, LANG_Y + 17)) {
                            g_lang = l;
                            redraw = 1;
                        }
                } else if (u.click) {
                    act = i;
                }
            }
        }
        if (redraw)
            draw_game_menu(sel);

        switch (act) {
        case 0:
            result = MENU_RESUME;
            break;
        case 1:
            /* Backing out of a file screen returns to the game, as
             * Ren'Py's game_menu key does from any menu screen. */
            files(1, cur, loaded, slot);
            result = MENU_RESUME;
            break;
        case 2:
            result = files(0, cur, loaded, slot) ? MENU_LOAD : MENU_RESUME;
            break;
        case GM_LANG:
            g_lang = (g_lang + 1) % MWB_NLANG;
            draw_game_menu(sel);
            break;
        case GM_RESTART:
            if (confirm(UI(Q_RESTART)))
                result = MENU_RESTART;
            else
                ui_reset();
            break;
        case GM_QUIT:
            if (confirm(UI(Q_QUIT)))
                result = MENU_QUIT;
            else
                ui_reset();
            break;
        }
    }
    gfx_pointer(0);
    if (g_text_cps != cps0 || g_lang != lang0)
        save_pref();
    return result;
}

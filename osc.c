#include "osc.h"

#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "osc"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "base64.h"
#include "config.h"
#include "grid.h"
#include "render.h"
#include "selection.h"
#include "spawn.h"
#include "terminal.h"
#include "uri.h"
#include "vt.h"
#include "xmalloc.h"

#define UNHANDLED() LOG_DBG("unhandled: OSC: %.*s", (int)term->vt.osc.idx, term->vt.osc.data)

static void
osc_to_clipboard(struct terminal *term, const char *target,
                 const char *base64_data)
{
    bool to_clipboard = false;
    bool to_primary = false;

    if (target[0] == '\0')
        to_clipboard = true;

    for (const char *t = target; *t != '\0'; t++) {
        switch (*t) {
        case 'c':
            to_clipboard = true;
            break;

        case 's':
        case 'p':
            to_primary = true;
            break;

        default:
            LOG_WARN("unimplemented: clipboard target '%c'", *t);
            break;
        }
    }

    /* Find a seat in which the terminal has focus */
    struct seat *seat = NULL;
    tll_foreach(term->wl->seats, it) {
        if (it->item.kbd_focus == term) {
            seat = &it->item;
            break;
        }
    }

    if (seat == NULL) {
        LOG_WARN("OSC52: client tried to write to clipboard data while window was unfocused");
        return;
    }

    char *decoded = base64_decode(base64_data);
    if (decoded == NULL) {
        if (errno == EINVAL)
            LOG_WARN("OSC: invalid clipboard data: %s", base64_data);
        else
            LOG_ERRNO("base64_decode() failed");

        if (to_clipboard)
            selection_clipboard_unset(seat);
        if (to_primary)
            selection_primary_unset(seat);
        return;
    }

    LOG_DBG("decoded: %s", decoded);

    if (to_clipboard) {
        char *copy = xstrdup(decoded);
        if (!text_to_clipboard(seat, term, copy, seat->kbd.serial))
            free(copy);
    }

    if (to_primary) {
        char *copy = xstrdup(decoded);
        if (!text_to_primary(seat, term, copy, seat->kbd.serial))
            free(copy);
    }

    free(decoded);
}

struct clip_context {
    struct seat *seat;
    struct terminal *term;
    uint8_t buf[3];
    int idx;
};

static void
from_clipboard_cb(char *text, size_t size, void *user)
{
    struct clip_context *ctx = user;
    struct terminal *term = ctx->term;

    assert(ctx->idx >= 0 && ctx->idx <= 2);

    const char *t = text;
    size_t left = size;

    if (ctx->idx > 0) {
        for (size_t i = ctx->idx; i < 3 && left > 0; i++, t++, left--)
            ctx->buf[ctx->idx++] = *t;

        assert(ctx->idx <= 3);
        if (ctx->idx == 3) {
            char *chunk = base64_encode(ctx->buf, 3);
            assert(chunk != NULL);
            assert(strlen(chunk) == 4);

            term_to_slave(term, chunk, 4);
            free(chunk);

            ctx->idx = 0;
        }
    }

    if (left == 0)
        return;

    assert(ctx->idx == 0);

    int remaining = left % 3;
    for (int i = remaining; i > 0; i--)
        ctx->buf[ctx->idx++] = text[size - i];
    assert(ctx->idx == remaining);

    char *chunk = base64_encode((const uint8_t *)t, left / 3 * 3);
    assert(chunk != NULL);
    assert(strlen(chunk) % 4 == 0);
    term_to_slave(term, chunk, strlen(chunk));
    free(chunk);
}

static void
from_clipboard_done(void *user)
{
    struct clip_context *ctx = user;
    struct terminal *term = ctx->term;

    if (ctx->idx > 0) {
        char res[4];
        base64_encode_final(ctx->buf, ctx->idx, res);
        term_to_slave(term, res, 4);
    }

    term_to_slave(term, "\033\\", 2);
    free(ctx);
}

static void
osc_from_clipboard(struct terminal *term, const char *source)
{
    /* Find a seat in which the terminal has focus */
    struct seat *seat = NULL;
    tll_foreach(term->wl->seats, it) {
        if (it->item.kbd_focus == term) {
            seat = &it->item;
            break;
        }
    }

    if (seat == NULL) {
        LOG_WARN("OSC52: client tried to read clipboard data while window was unfocused");
        return;
    }

    /* Use clipboard if no source has been specified */
    char src = source[0] == '\0' ? 'c' : 0;
    bool from_clipboard = src == 'c';
    bool from_primary = false;

    for (const char *s = source;
         *s != '\0' && !from_clipboard && !from_primary;
         s++)
    {
        if (*s == 'c' || *s == 'p' || *s == 's') {
            src = *s;

            switch (src) {
            case 'c':
                from_clipboard = selection_clipboard_has_data(seat);
                break;

            case 's':
            case 'p':
                from_primary = selection_primary_has_data(seat);
                break;
            }
        } else
            LOG_WARN("unimplemented: clipboard source '%c'", *s);
    }

    if (!from_clipboard && !from_primary)
        return;

    term_to_slave(term, "\033]52;", 5);
    term_to_slave(term, &src, 1);
    term_to_slave(term, ";", 1);

    struct clip_context *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct clip_context) {.seat = seat, .term = term};

    if (from_clipboard) {
        text_from_clipboard(
            seat, term, &from_clipboard_cb, &from_clipboard_done, ctx);
    }

    if (from_primary) {
        text_from_primary(
            seat, term, &from_clipboard_cb, &from_clipboard_done, ctx);
    }
}

static void
osc_selection(struct terminal *term, char *string)
{
    char *p = string;
    bool clipboard_done = false;

    /* The first parameter is a string of clipbard sources/targets */
    while (*p != '\0' && !clipboard_done) {
        switch (*p) {
        case ';':
            clipboard_done = true;
            *p = '\0';
            break;
        }

        p++;
    }

    LOG_DBG("clipboard: target = %s data = %s", string, p);

    if (strlen(p) == 1 && p[0] == '?')
        osc_from_clipboard(term, string);
    else
        osc_to_clipboard(term, string, p);
}

static void
osc_flash(struct terminal *term)
{
    /* Our own private - flash */
    term_flash(term, 50);
}

static bool
parse_legacy_color(const char *string, uint32_t *color)
{
    if (string[0] != '#')
        return false;

    string++;
    const size_t len = strlen(string);

    if (len % 3 != 0)
        return false;

    const int digits = len / 3;

    int rgb[3];
    for (size_t i = 0; i < 3; i++) {
        rgb[i] = 0;
        for (size_t j = 0; j < digits; j++) {
            size_t idx = i * digits + j;
            char c = string[idx];
            rgb[i] <<= 4;

            if (!isxdigit(c))
                rgb[i] |= 0;
            else
                rgb[i] |= c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
        }

        /* Values with less than 16 bits represent the *most
         * significant bits*. I.e. the values are *not* scaled */
        rgb[i] <<= 16 - (4 * digits);
    }

    /* Re-scale to 8-bit */
    uint8_t r = 256 * (rgb[0] / 65536.);
    uint8_t g = 256 * (rgb[1] / 65536.);
    uint8_t b = 256 * (rgb[2] / 65536.);

    LOG_DBG("legacy: %02x%02x%02x", r, g, b);
    *color = r << 16 | g << 8 | b;
    return true;
}

static bool
parse_rgb(const char *string, uint32_t *color)
{
    size_t len = strlen(string);

    /* Verify we have the minimum required length (for "rgb:x/x/x") */
    if (len < 3 /* 'rgb' */ + 1 /* ':' */ + 2 /* '/' */ + 3 * 1 /* 3 * 'x' */)
        return false;

    /* Verify prefix is "rgb:" */
    if (string[0] != 'r' || string[1] != 'g' || string[2] != 'b' || string[3] != ':')
        return false;

    string += 4;
    len -= 4;

    int rgb[3];
    int digits[3];

    for (size_t i = 0; i < 3; i++) {
        for (rgb[i] = 0, digits[i] = 0;
             len > 0 && *string != '/';
             len--, string++, digits[i]++)
        {
            char c = *string;
            rgb[i] <<= 4;

            if (!isxdigit(c))
                rgb[i] |= 0;
            else
                rgb[i] |= c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
        }

        if (i >= 2)
            break;

        if (len == 0 || *string != '/')
            return false;
        string++; len--;
    }

    /* Re-scale to 8-bit */
    uint8_t r = 256 * (rgb[0] / (double)(1 << (4 * digits[0])));
    uint8_t g = 256 * (rgb[1] / (double)(1 << (4 * digits[1])));
    uint8_t b = 256 * (rgb[2] / (double)(1 << (4 * digits[2])));

    LOG_DBG("rgb: %02x%02x%02x", r, g, b);
    *color = r << 16 | g << 8 | b;
    return true;
}

static void
osc_set_pwd(struct terminal *term, char *string)
{
    LOG_DBG("PWD: URI: %s", string);

    char *scheme, *host, *path;
    if (!uri_parse(string, strlen(string), &scheme, NULL, NULL, &host, NULL, &path, NULL, NULL)) {
        LOG_ERR("OSC7: invalid URI: %s", string);
        return;
    }

    if (strcmp(scheme, "file") == 0 && hostname_is_localhost(host)) {
        LOG_DBG("OSC7: pwd: %s", path);
        free(term->cwd);
        term->cwd = path;
    } else
        free(path);

    free(scheme);
    free(host);
}

static void
osc_notify(struct terminal *term, char *string)
{
    /*
     * The 'notify' perl extension
     * (https://pub.phyks.me/scripts/urxvt/notify) is very simple:
     *
     * #!/usr/bin/perl
     * 
     * sub on_osc_seq_perl {
     *   my ($term, $osc, $resp) = @_;
     *   if ($osc =~ /^notify;(\S+);(.*)$/) {
     *     system("notify-send '$1' '$2'");
     *   }
     * }
     *
     * As can be seen, the notification text is not encoded in any
     * way. The regex does a greedy match of the ';' separator. Thus,
     * any extra ';' will end up being part of the title. There's no
     * way to have a ';' in the message body.
     *
     * I've changed that behavior slightly in; we split the title from
     * body on the *first* ';', allowing us to have semicolons in the
     * message body, but *not* in the title.
     */
    char *ctx = NULL;
    const char *title = strtok_r(string, ";", &ctx);
    const char *msg = strtok_r(NULL, "\x00", &ctx);

    LOG_DBG("notify: title=\"%s\", msg=\"%s\"", title, msg);

    /* TODO: move everything below to a separate function, to be able
     * to support multiple escape sequences */

    if (title == NULL || msg == NULL)
        return;

    if (term->conf->notify.argv == NULL)
        return;

    size_t argv_size = 0;
    for (; term->conf->notify.argv[argv_size] != NULL; argv_size++)
        ;

#define append(s, n)                                        \
    do {                                                    \
        expanded = xrealloc(expanded, len + (n) + 1);       \
        memcpy(&expanded[len], s, n);                       \
        len += n;                                           \
        expanded[len] = '\0';                               \
    } while (0)

    char **argv = malloc((argv_size + 1) * sizeof(argv[0]));

    /* Expand ${title} and ${body} */
    for (size_t i = 0; i < argv_size; i++) {
        size_t len = 0;
        char *expanded = NULL;

        char *start = NULL;
        char *last_end = term->conf->notify.argv[i];

        while ((start = strstr(last_end, "${")) != NULL) {
            /* Append everything from the last template's end to this
             * one's beginning */
            append(last_end, start - last_end);

            /* Find end of template */
            start += 2;
            char *end = strstr(start, "}");

            if (end == NULL) {
                /* Ensure final append() copies the unclosed '${' */
                last_end = start - 2;
                LOG_WARN("notify: unclosed template: %s", last_end);
                break;
            }

            /* Expand template */
            if (strncmp(start, "title", end - start) == 0)
                append(title, strlen(title));
            else if (strncmp(start, "body", end - start) == 0)
                append(msg, strlen(msg));
            else {
                /* Unrecognized template - append it as-is */
                start -= 2;
                append(start, end + 1 - start);
                LOG_WARN("notify: unrecognized template: %.*s",
                         (int)(end + 1 - start), start);
            }

            last_end = end + 1;;
        }

        append(last_end, term->conf->notify.argv[i] + strlen(term->conf->notify.argv[i]) - last_end);
        argv[i] = expanded;
    }
    argv[argv_size] = NULL;

#undef append

    LOG_DBG("notify command:");
    for (size_t i = 0; i < argv_size; i++)
        LOG_DBG("  argv[%zu] = \"%s\"", i, argv[i]);

    /* Redirect stdin to /dev/null, but ignore failure to open */
    int devnull = open("/dev/null", O_RDONLY);
    spawn(term->reaper, NULL, argv, devnull, -1, -1);

    if (devnull >= 0)
        close(devnull);

    for (size_t i = 0; i < argv_size; i++)
        free(argv[i]);
    free(argv);
}

static void
update_color_in_grids(struct terminal *term, uint32_t old_color,
                      uint32_t new_color)
{
    /*
     * Update color of already rendered cells.
     *
     * Note that we do *not* store the original palette
     * index. Therefore, the best we can do is compare colors - if
     * they match, assume "our" palette index was the one used to
     * render the cell.
     *
     * There are a couple of cases where this isn't necessarily true:
     * - user has configured the 16 base colors with non-unique
     * colors.  - the client has used 24-bit escapes for colors
     *
     * In general though, if the client configures the palette, it is
     * very likely only using index:ed coloring (i.e. not 24-bit
     * direct colors), and I hope that it is unusual with palettes
     * where all the colors aren't unique.
     *
     * TODO(?): for performance reasons, we only update the current
     * screen rows (of both grids). I.e. scrollback is *not* updated.
     */
    for (size_t i = 0; i < 2; i++) {
        struct grid *grid = i == 0 ? &term->normal : &term->alt;

        for (size_t r = 0; r < term->rows; r++) {
            struct row *row = grid_row(grid, r);
            assert(row != NULL);

            for (size_t c = 0; c < term->grid->num_cols; c++) {
                struct cell *cell = &row->cells[c];
                if (cell->attrs.have_fg &&
                    cell->attrs.fg == old_color)
                {
                    cell->attrs.fg = new_color;
                    cell->attrs.clean = 0;
                    row->dirty = true;
                }

                if ( cell->attrs.have_bg &&
                    cell->attrs.bg == old_color)
                {
                    cell->attrs.bg = new_color;
                    cell->attrs.clean = 0;
                    row->dirty = true;
                }
            }
        }
    }
}

void
osc_dispatch(struct terminal *term)
{
    unsigned param = 0;
    int data_ofs = 0;

    for (size_t i = 0; i < term->vt.osc.idx; i++, data_ofs++) {
        char c = term->vt.osc.data[i];

        if (c == ';') {
            data_ofs++;
            break;
        }

        if (!isdigit(c)) {
            UNHANDLED();
            return;
        }

        param *= 10;
        param += c - '0';
    }

    LOG_DBG("OCS: %.*s (param = %d)",
            (int)term->vt.osc.idx, term->vt.osc.data, param);

    char *string = (char *)&term->vt.osc.data[data_ofs];

    switch (param) {
    case 0: term_set_window_title(term, string); break;  /* icon + title */
    case 1: break;                                       /* icon */
    case 2: term_set_window_title(term, string); break;  /* title */

    case 4: {
        /* Set color<idx> */

        string--;
        if (*string != ';')
            break;

        assert(*string == ';');

        for (const char *s_idx = strtok(string, ";"), *s_color = strtok(NULL, ";");
             s_idx != NULL && s_color != NULL;
             s_idx = strtok(NULL, ";"), s_color = strtok(NULL, ";"))
        {
            /* Parse <idx> parameter */
            unsigned idx = 0;
            for (; *s_idx != '\0'; s_idx++) {
                char c = *s_idx;
                idx *= 10;
                idx += c - '0';
            }

            /* Client queried for current value */
            if (strlen(s_color) == 1 && s_color[0] == '?') {
                uint32_t color = term->colors.table[idx];
                uint8_t r = (color >> 16) & 0xff;
                uint8_t g = (color >>  8) & 0xff;
                uint8_t b = (color >>  0) & 0xff;

                char reply[32];
                snprintf(reply, sizeof(reply), "\033]4;%u;rgb:%02x/%02x/%02x\033\\",
                         idx, r, g, b);
                term_to_slave(term, reply, strlen(reply));
            }

            else {
                uint32_t color;
                bool color_is_valid = s_color[0] == '#'
                    ? parse_legacy_color(s_color, &color)
                    : parse_rgb(s_color, &color);

                if (!color_is_valid)
                    continue;

                LOG_DBG("change color definition for #%u from %06x to %06x",
                        idx, term->colors.table[idx], color);

                update_color_in_grids(term, term->colors.table[idx], color);
                term->colors.table[idx] = color;
            }
        }

        break;
    }

    case 7:
        /* Update terminal's understanding of PWD */
        osc_set_pwd(term, string);
        break;

    case 10:
    case 11: {
        /* Set default foreground/background color */

        /* Client queried for current value */
        if (strlen(string) == 1 && string[0] == '?') {
            uint32_t color = param == 10 ? term->colors.fg : term->colors.bg;
            uint8_t r = (color >> 16) & 0xff;
            uint8_t g = (color >>  8) & 0xff;
            uint8_t b = (color >>  0) & 0xff;

            /*
             * Reply in XParseColor format
             * E.g. for color 0xdcdccc we reply "\033]10;rgb:dc/dc/cc\033\\"
             */
            char reply[32];
            snprintf(
                reply, sizeof(reply), "\033]%u;rgb:%02x/%02x/%02x\033\\",
                param, r, g, b);

            term_to_slave(term, reply, strlen(reply));
            break;
        }

        uint32_t color;
        if (string[0] == '#' ? !parse_legacy_color(string, &color) : !parse_rgb(string, &color))
            break;

        LOG_DBG("change color definition for %s to %06x",
                param == 10 ? "foreground" : "background", color);

        switch (param) {
        case 10: term->colors.fg = color; break;
        case 11: term->colors.bg = color; break;
        }

        term_damage_view(term);
        term_damage_margins(term);
        break;
    }

    case 12: /* Set cursor color */

        /* Client queried for current value */
        if (strlen(string) == 1 && string[0] == '?') {
            uint8_t r = (term->cursor_color.cursor >> 16) & 0xff;
            uint8_t g = (term->cursor_color.cursor >>  8) & 0xff;
            uint8_t b = (term->cursor_color.cursor >>  0) & 0xff;

            char reply[32];
            snprintf(reply, sizeof(reply), "\033]12;rgb:%02x/%02x/%02x\033\\", r, g, b);
            term_to_slave(term, reply, strlen(reply));
            break;
        }

        uint32_t color;
        if (string[0] == '#' ? !parse_legacy_color(string, &color) : !parse_rgb(string, &color))
            break;

        LOG_DBG("change cursor color to %06x", color);

        if (color == 0)
            term->cursor_color.cursor = 0;  /* Invert fg/bg */
        else
            term->cursor_color.cursor = 1u << 31 | color;

        term_damage_cursor(term);
        break;

    case 30:  /* Set tab title */
        break;

    case 52:  /* Copy to/from clipboard/primary */
        osc_selection(term, string);
        break;

    case 104: {
        /* Reset Color Number 'c' (whole table if no parameter) */

        if (strlen(string) == 0) {
            LOG_DBG("resetting all colors");
            for (size_t i = 0; i < 256; i++) {
                update_color_in_grids(
                    term, term->colors.table[i], term->colors.default_table[i]);
                term->colors.table[i] = term->colors.default_table[i];
        }
        }

        else {
            for (const char *s_idx = strtok(string, ";");
                 s_idx != NULL;
                 s_idx = strtok(NULL, ";"))
            {
                unsigned idx = 0;
                for (; *s_idx != '\0'; s_idx++) {
                    char c = *s_idx;
                    idx *= 10;
                    idx += c - '0';
                }

                LOG_DBG("resetting color #%u", idx);
                update_color_in_grids(
                    term, term->colors.table[idx], term->colors.default_table[idx]);
                term->colors.table[idx] = term->colors.default_table[idx];
            }
        }

        break;
    }

    case 105: /* Reset Special Color Number 'c' */
        break;

    case 110: /* Reset default text foreground color */
        LOG_DBG("resetting foreground");
        term->colors.fg = term->colors.default_fg;
        term_damage_view(term);
        break;

    case 111: /* Reset default text background color */
        LOG_DBG("resetting background");
        term->colors.bg = term->colors.default_bg;
        term_damage_view(term);
        term_damage_margins(term);
        break;

    case 112:
        LOG_DBG("resetting cursor color");
        term->cursor_color.text = term->conf->cursor.color.text;
        term->cursor_color.cursor = term->conf->cursor.color.cursor;
        term_damage_cursor(term);
        break;

    case 555:
        osc_flash(term);
        break;

    case 777: {
        /*
         * OSC 777 is an URxvt generic escape used to send commands to
         * perl extensions. The generic syntax is: \E]777;<command>;<string>ST
         *
         * We only recognize the 'notify' command, which is, if not
         * well established, at least fairly well known.
         */

        char *param_brk = strchr(string, ';');
        if (param_brk == NULL) {
            UNHANDLED();
            return;
        }

        if (strncmp(string, "notify", param_brk - string) == 0)
            osc_notify(term, param_brk + 1);
        else
            UNHANDLED();
        break;
    }

    default:
        UNHANDLED();
        break;
    }
}

bool
osc_ensure_size(struct terminal *term, size_t required_size)
{
    if (required_size <= term->vt.osc.size)
        return true;

    size_t new_size = (required_size + 127) / 128 * 128;
    assert(new_size > 0);

    uint8_t *new_data = realloc(term->vt.osc.data, new_size);
    if (new_data == NULL) {
        LOG_ERRNO("failed to increase size of OSC buffer");
        return false;
    }

    term->vt.osc.data = new_data;
    term->vt.osc.size = new_size;
    return true;
}

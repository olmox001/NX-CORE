/*
 * kernel/lib/vsnprintf.c
 * Bounded formatted string output (vsnprintf)
 *
 * Purpose:
 *   Implements vsnprintf() — the single formatted-output primitive used by
 *   printk(), snprintf(), and (via the userland build path) the os1 user library.
 *   All other formatted-output functions in the kernel are thin wrappers over
 *   this one.
 *
 * Role:
 *   This file is compiled in both kernel and userland modes:
 *     - KERNEL defined: pulls in kernel/printk.h and kernel/types.h.
 *     - KERNEL not defined: pulls in os1.h (userland).
 *   The same translation unit therefore serves both contexts.
 *
 * Supported specifiers:
 *   %c  %s  %d/%i  %u  %x/%X  %p  %%
 *   Length modifiers: l, ll, z.
 *   Flags: - (left-align), + (force sign), ' ' (space sign), # (alternate),
 *          0 (zero-pad).
 *   Width: decimal field width, or '*' to take it from an int argument (C99);
 *          a negative '*' width means left-justify with its magnitude.
 *   Precision: decimal precision, or '.*' to take it from an int argument (C99);
 *          a negative '*' precision means "no precision".  For %s this bounds
 *          the copy, so '%.*s' is safe and does not read past the argument.
 *
 * Invariants:
 *   - Every write path guards `written < (int)size - 1` before writing.
 *   - buf[written] = '\0' is always written at exit (NUL termination).
 *   - No %n specifier is implemented (security: no arbitrary memory write).
 *
 * Known issues:
 *   LIB-VSNPRINTF-01  (W1 BUG)     Sign char emitted without decrementing width;
 *                                   %05d of -42 produces "-00042" (6 chars) not
 *                                   "-0042".  See print_num().
 *   LIB-VSNPRINTF-02  (W1 REFINE)  Returns chars written, not would-be length;
 *                                   callers cannot detect truncation. See vsnprintf.
 *   LIB-VSNPRINTF-03  (W0 MISSING) %o (octal) and %e/%f (float) are absent.
 *   LIB-VSNPRINTF-04  (W1 BAD-IMPL) %p hardcodes 16-digit width regardless of
 *                                   remaining buffer space.  See case 'p'.
 */
#ifdef KERNEL
#include <kernel/printk.h>
#include <kernel/types.h>
#else
#include <os1.h>
#endif
#include <stdarg.h>

/* Number conversion flags — bit-mask used by print_num() and the vsnprintf loop.
 * FLAG_ZEROPAD:   '0' flag: pad with '0' instead of ' ' when right-aligned.
 * FLAG_LEFT:      '-' flag: left-justify (overrides FLAG_ZEROPAD).
 * FLAG_PLUS:      '+' flag: always emit sign character even for non-negative.
 * FLAG_SPACE:     ' ' flag: prefix with a space for non-negative numbers.
 * FLAG_SPECIAL:   '#' flag: parsed but has no effect in the current implementation.
 * FLAG_UPPERCASE: uppercase hex digits (A-F); set for %X specifier.
 * FLAG_SIGN:      internal flag: set when the value is negative; causes '-' output.
 */
#define FLAG_ZEROPAD   0x01
#define FLAG_LEFT      0x02
#define FLAG_PLUS      0x04
#define FLAG_SPACE     0x08
#define FLAG_SPECIAL   0x10
#define FLAG_UPPERCASE 0x20
#define FLAG_SIGN      0x40

/*
 * print_num - format an unsigned integer into buf in the given base.
 *
 * Digits are accumulated in reverse into tmp[], then written forward.
 * Padding and sign/prefix characters are inserted between the width padding
 * and the digit string in the following order:
 *   [space padding] [sign or space] [zero padding] [digits] [left padding]
 *
 * Params:
 *   buf       - output buffer; write starts at buf[0].
 *   size      - remaining capacity of buf including NUL slot.
 *   num       - value to format (always unsigned at this point; sign already
 *               extracted by the caller and encoded in flags).
 *   base      - numeric base: 10 or 16.
 *   width     - minimum field width; 0 means no minimum.
 *   precision - minimum digit count (zero-pad with '0'); -1 means no minimum.
 *   flags     - bitwise OR of FLAG_* constants above.
 * Returns: number of characters written to buf (never writes NUL).
 * Locking: none (stateless aside from static digit tables).
 *
 * NOTE(LIB-VSNPRINTF-01): width is decremented by the digit count (i) before
 *   the sign character is emitted.  When FLAG_ZEROPAD is set, the sign is
 *   written first and then zero-padding fills width characters.  But width was
 *   already reduced by the digit count only — not by 1 for the sign char — so
 *   total output is (sign + width zeros + digits), which is one character more
 *   than the requested field width.  Example: print_num(..., -42, 10, 5, -1,
 *   FLAG_ZEROPAD|FLAG_SIGN) emits "-00042" (6 chars) instead of "-0042".
 */
static int print_num(char *buf, size_t size, uint64_t num, uint32_t base,
                     int width, int precision, int flags) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = (flags & FLAG_UPPERCASE) ? digits_upper : digits_lower;

    char tmp[66];
    int i = 0;
    size_t written = 0U;
    int needed = 0;

    if (num == 0U) {
        tmp[i++] = '0';
    } else {
        while (num != 0) {
            tmp[i++] = digits[(size_t)(num % (uint64_t)base)];
            num /= (uint64_t)base;
        }
    }

    while (i < precision && i < 64) {
        tmp[i++] = '0';
    }

    width -= i;
    if (flags & (FLAG_SIGN | FLAG_PLUS | FLAG_SPACE)) {
        width--;
    }

    if (!(flags & (FLAG_ZEROPAD | FLAG_LEFT))) {
        while (width > 0) {
            if (size > 0U && written < size - 1U) buf[written++] = ' ';
            needed++;
            width--;
        }
    }

    if (flags & FLAG_SIGN) {
        if (size > 0U && written < size - 1U) buf[written++] = '-';
        needed++;
    } else if (flags & FLAG_PLUS) {
        if (size > 0U && written < size - 1U) buf[written++] = '+';
        needed++;
    } else if (flags & FLAG_SPACE) {
        if (size > 0U && written < size - 1U) buf[written++] = ' ';
        needed++;
    }

    if (flags & FLAG_ZEROPAD) {
        while (width > 0) {
            if (size > 0U && written < size - 1U) buf[written++] = '0';
            needed++;
            width--;
        }
    }

    while (i > 0) {
        if (size > 0U && written < size - 1U) buf[written++] = tmp[--i];
        else i--;
        needed++;
    }

    if (flags & FLAG_LEFT) {
        while (width > 0) {
            if (size > 0U && written < size - 1U) buf[written++] = ' ';
            needed++;
            width--;
        }
    }

    return needed;
}

#ifndef KERNEL
/*
 * Floating-point conversion (%f/%F, %e/%E, %g/%G) — USERLAND ONLY.
 *
 * The kernel build (KERNEL defined) is compiled FPU-free and never formats
 * a double (LIB-VSNPRINTF-03), so this whole block is excluded there: no
 * floating-point code lands in the kernel image, no FPU save/restore is
 * implied.  The userland os1 library, by contrast, backs Lua's number->string
 * path (LUAI_NUMFFORMAT "%.14g", string.format("%g"/"%f"/"%e", ...)): without
 * this, every such conversion fell through to the default case and emitted the
 * literal spec ("%g"), corrupting all Lua numeric output.
 *
 * Digits come from double arithmetic only (no libm dependency): ~15-16
 * significant decimal digits are reliable, which covers Lua's 14-digit %g.
 */

/*
 * fp_gen - generate `ndigits` rounded significant decimal digits of value>0.
 *   digs[]    receives ndigits chars '0'..'9' (digs[0] most significant);
 *   *decexp   receives the base-10 exponent of the first digit, i.e.
 *             value ~= digs[0].digs[1..] x 10^(*decexp).
 * ndigits is clamped to [1,17].  Rounding is half-up on the guard digit and
 * carries all the way (all-nines rollover bumps *decexp).
 */
static void fp_gen(double value, int ndigits, char *digs, int *decexp) {
    if (ndigits < 1) ndigits = 1;
    if (ndigits > 17) ndigits = 17;

    int e = 0;
    double v = value;
    while (v >= 10.0) { v /= 10.0; e++; }
    while (v < 1.0)   { v *= 10.0; e--; }

    int d[19]; /* ndigits (<=17) + one guard digit */
    for (int i = 0; i <= ndigits; i++) {
        int digit = (int)v;
        if (digit < 0) digit = 0; else if (digit > 9) digit = 9;
        d[i] = digit;
        v = (v - (double)digit) * 10.0;
    }

    int carry = (d[ndigits] >= 5) ? 1 : 0;
    for (int i = ndigits - 1; i >= 0 && carry; i--) {
        d[i] += 1;
        if (d[i] == 10) { d[i] = 0; carry = 1; } else carry = 0;
    }
    if (carry) { /* 9.999.. rounded to 10.000..: shift and bump the exponent */
        for (int i = ndigits - 1; i > 0; i--) d[i] = d[i - 1];
        d[0] = 1;
        e++;
    }
    for (int i = 0; i < ndigits; i++) digs[i] = (char)('0' + d[i]);
    *decexp = e;
}

/* Append the exponent suffix (e/E +/- NN, at least two digits) to out[n..]. */
static int fp_exp(char *out, int n, int decexp, int upper) {
    out[n++] = upper ? 'E' : 'e';
    int ex = decexp;
    if (ex < 0) { out[n++] = '-'; ex = -ex; } else out[n++] = '+';
    char eb[8];
    int en = 0;
    if (ex == 0) eb[en++] = '0';
    while (ex > 0) { eb[en++] = (char)('0' + ex % 10); ex /= 10; }
    while (en < 2) eb[en++] = '0';
    while (en > 0) out[n++] = eb[--en];
    return n;
}

/*
 * fp_format - render `value` into out[] (NUL-terminated); returns length.
 *   spec  : 'f', 'e' or 'g' (lowercase); `upper` selects the E/G letter case.
 *   prec  : precision (<0 -> default 6); flags: FLAG_PLUS/SPACE/SPECIAL honored.
 * Sign and inf/nan are handled here; field WIDTH is applied by the caller.
 */
static int fp_format(char *out, double value, char spec, int prec, int flags,
                     int upper) {
    int n = 0;
    union { double d; uint64_t u; } bp;
    bp.d = value;
    int neg = (int)(bp.u >> 63);
    uint64_t expo = (bp.u >> 52) & 0x7ff;
    uint64_t mant = bp.u & 0xfffffffffffffULL;

    char sgn = neg ? '-' : (flags & FLAG_PLUS) ? '+'
                          : (flags & FLAG_SPACE) ? ' ' : 0;

    if (expo == 0x7ff) { /* inf / nan */
        if (mant) {
            const char *w = upper ? "NAN" : "nan";
            while (*w) out[n++] = *w++;
        } else {
            if (sgn) out[n++] = sgn;
            const char *w = upper ? "INF" : "inf";
            while (*w) out[n++] = *w++;
        }
        out[n] = '\0';
        return n;
    }

    if (neg) value = -value;
    if (prec < 0) prec = 6;
    if (sgn) out[n++] = sgn;

    char digs[20];
    int decexp;

    if (spec == 'e') {
        int ndig = prec + 1;
        if (value == 0.0) { for (int i = 0; i < 20; i++) digs[i] = '0'; decexp = 0; }
        else fp_gen(value, ndig, digs, &decexp);
        out[n++] = digs[0];
        if (prec > 0 || (flags & FLAG_SPECIAL)) out[n++] = '.';
        for (int i = 1; i <= prec; i++) out[n++] = (i < 20) ? digs[i] : '0';
        n = fp_exp(out, n, decexp, upper);
    } else if (spec == 'g') {
        int P = prec ? prec : 1;
        if (value == 0.0) { for (int i = 0; i < 20; i++) digs[i] = '0'; decexp = 0; }
        else fp_gen(value, P, digs, &decexp);
        int siglen = P;
        if (!(flags & FLAG_SPECIAL))
            while (siglen > 1 && digs[siglen - 1] == '0') siglen--;

        if (decexp < -4 || decexp >= P) { /* scientific */
            out[n++] = digs[0];
            if (siglen > 1 || (flags & FLAG_SPECIAL)) out[n++] = '.';
            for (int i = 1; i < siglen; i++) out[n++] = digs[i];
            n = fp_exp(out, n, decexp, upper);
        } else { /* fixed */
            int di = 0;
            if (decexp < 0) out[n++] = '0';
            else for (int p = decexp; p >= 0; p--)
                     out[n++] = (di < siglen) ? digs[di++] : '0';
            char frac[48];
            int fn = 0;
            if (decexp < 0) {
                for (int p = -1; p > decexp; p--) frac[fn++] = '0';
                for (int i = 0; i < siglen; i++) frac[fn++] = digs[i];
            } else {
                for (int i = decexp + 1; i < siglen; i++) frac[fn++] = digs[i];
            }
            if (fn > 0 || (flags & FLAG_SPECIAL)) {
                out[n++] = '.';
                for (int i = 0; i < fn; i++) out[n++] = frac[i];
            }
        }
    } else { /* 'f' */
        if (value == 0.0) { digs[0] = '0'; decexp = 0;
            for (int i = 1; i < 20; i++) digs[i] = '0'; }
        else {
            fp_gen(value, 1, digs, &decexp); /* locate the point first */
            int ndig = decexp + 1 + prec;
            if (ndig < 1) ndig = 1;
            if (ndig > 17) ndig = 17;
            fp_gen(value, ndig, digs, &decexp);
        }
        int ndig = decexp + 1 + prec;
        if (ndig < 1) ndig = 1;
        if (ndig > 17) ndig = 17;
        int di = 0;
        if (decexp < 0) out[n++] = '0';
        else for (int p = decexp; p >= 0; p--)
                 out[n++] = (di < ndig) ? digs[di++] : '0';
        if (prec > 0 || (flags & FLAG_SPECIAL)) out[n++] = '.';
        for (int p = -1; p >= -prec; p--) {
            int idx = decexp - p;
            out[n++] = (idx >= 0 && idx < ndig) ? digs[idx] : '0';
        }
    }
    out[n] = '\0';
    return n;
}
#endif /* !KERNEL */

/*
 * vsnprintf - format a string into buf using at most size bytes (including NUL).
 *
 * This is the kernel's bounded printf engine; all formatted output passes through
 * here.  The function is compiled for both kernel and userland (see file header).
 *
 * Params:
 *   buf  - destination buffer; must be at least 'size' bytes.
 *   size - total capacity of buf; if 0, returns 0 and writes nothing.
 *   fmt  - printf-style format string.
 *   args - va_list of format arguments; caller owns va_start/va_end.
 * Returns: number of characters written, NOT including the NUL terminator.
 *   NOTE(LIB-VSNPRINTF-02): returns chars written (< size), NOT the total chars
 *   needed if the buffer were unbounded.  Truncation is not detectable from the
 *   return value alone; this is not POSIX-conformant vsnprintf behaviour.
 * Locking: none (no global state modified).
 * Side effects: writes to buf; always NUL-terminates if size > 0.
 *
 * Format string parsing loop:
 *   For each '%' the loop parses:
 *     1. Zero or more flag characters (-, +, space, #, 0).
 *     2. Optional decimal field width.
 *     3. Optional '.' followed by decimal precision.
 *     4. Optional length modifier (l, ll, z).
 *     5. Conversion specifier character.
 *
 * NOTE(LIB-VSNPRINTF-03): %o (octal) is not implemented; %e/%f are absent
 *   (acceptable since the kernel does not use the FPU).
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    size_t written = 0U;
    int needed = 0;
    int width, precision;
    int flags;
    uint64_t num;
    const char *s;

    if (size == 0 && buf != NULL) return 0; /* POSIX allows buf=NULL, size=0 */

    while (*fmt) {
        if (*fmt != '%') {
            if (size > 0U && written < size - 1U) buf[written++] = *fmt;
            needed++;
            fmt++;
            continue;
        }

        fmt++; /* Skip '%' */

        /* Parse flags: accumulate all flag characters before width/precision */
        flags = 0;
        while (1) {
            if (*fmt == '-')      flags |= FLAG_LEFT;
            else if (*fmt == '+') flags |= FLAG_PLUS;
            else if (*fmt == ' ') flags |= FLAG_SPACE;
            else if (*fmt == '#') flags |= FLAG_SPECIAL;
            else if (*fmt == '0') flags |= FLAG_ZEROPAD;
            else break;
            fmt++;
        }

        /* Field width: a '*' takes the width from an int argument (C99); a
         * negative value means left-justify with its magnitude.  Otherwise a run
         * of decimal digits after flags, before '.' or the specifier.
         * NOTE: '*' MUST consume its int arg here — omitting it desynchronises
         * every following va_arg (a bogus pointer lands in the next %s and
         * faults on deref), which is exactly the '%.*s' crash class. */
        width = 0;
        if (*fmt == '*') {
            fmt++;
            width = va_arg(args, int);
            if (width < 0) {
                flags |= FLAG_LEFT;
                width = -width;
            }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Precision: optional '.' followed by a '*' (take from an int argument,
         * C99; a negative value means "no precision") or a run of decimal
         * digits.  -1 means no precision was specified. */
        precision = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                fmt++;
                precision = va_arg(args, int);
                if (precision < 0)
                    precision = -1;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Length modifiers:
         *   l  → long (is_long=1)
         *   ll → long long / int64_t (is_long=2)
         *   z  → size_t (treated as 32- or 64-bit per sizeof) */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                is_long = 2;
                fmt++;
            }
        } else if (*fmt == 'z') {
            is_long = (sizeof(size_t) == 8) ? 2 : 1;
            fmt++;
        }

        /* Conversion specifier */
        switch (*fmt) {
            case 'c':
                if (size > 0U && written < size - 1U) buf[written++] = (char)va_arg(args, int);
                needed++;
                break;

            case 's':
                s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len]) {
                    if (precision >= 0 && len >= precision) break;
                    len++;
                }
                if (!(flags & FLAG_LEFT)) {
                    while (len < width) {
                        if (size > 0U && written < size - 1U) buf[written++] = ' ';
                        needed++;
                        width--;
                    }
                }
                for (int j = 0; j < len; j++) {
                    if (size > 0U && written < size - 1U) buf[written++] = s[j];
                    needed++;
                }
                while (len < width) {
                    if (size > 0U && written < size - 1U) buf[written++] = ' ';
                    needed++;
                    width--;
                }
                break;

            case 'd':
            case 'i': {
                int64_t signed_num;
                if (is_long == 2)
                    signed_num = va_arg(args, int64_t);
                else if (is_long == 1)
                    signed_num = (int64_t)va_arg(args, long);
                else
                    signed_num = (int64_t)va_arg(args, int);

                if (signed_num < 0) {
                    flags |= FLAG_SIGN;
                    num = UINT64_C(0) - (uint64_t)signed_num;
                } else {
                    num = (uint64_t)signed_num;
                }

                size_t remaining = size > written ? size - written : 0U;
                int n_added =
                    print_num(remaining > 0U ? buf + written : NULL, remaining,
                              num, UINT32_C(10), width, precision, flags);
                needed += n_added;
                if (remaining > 0U) {
                    size_t capacity = remaining - 1U;
                    size_t committed = (size_t)n_added;
                    if (committed > capacity)
                        committed = capacity;
                    written += committed;
                }
                break;
            }

            case 'u':
            case 'x':
            case 'X': {
                if (is_long == 2)
                    num = va_arg(args, uint64_t);
                else if (is_long == 1)
                    num = (uint64_t)va_arg(args, unsigned long);
                else
                    num = (uint64_t)va_arg(args, unsigned int);

                if (*fmt == 'X')
                    flags |= FLAG_UPPERCASE;

                size_t remaining = size > written ? size - written : 0U;
                uint32_t base =
                    *fmt == 'u' ? UINT32_C(10) : UINT32_C(16);
                int n_added =
                    print_num(remaining > 0U ? buf + written : NULL, remaining,
                              num, base, width, precision, flags);
                needed += n_added;
                if (remaining > 0U) {
                    size_t capacity = remaining - 1U;
                    size_t committed = (size_t)n_added;
                    if (committed > capacity)
                        committed = capacity;
                    written += committed;
                }
                break;
            }

            case 'p': {
                /* Build the pointer token "0x" + 16 zero-padded hex digits in
                 * a scratch buffer (unchanged look for kernel logs), THEN apply
                 * the FIELD WIDTH to the whole token with space padding /
                 * left-justify.  The old code fed `width` straight into the
                 * hex digit count, so "%90p" produced 92 chars of zero-padded
                 * hex instead of a 90-wide field (LIB-VSNPRINTF-04) — which
                 * broke Lua's `#string.format("%90p", {}) == 90`. */
                num = (uint64_t)va_arg(args, void *);
                char pb[32];
                int pl = 0;
                pb[pl++] = '0';
                pb[pl++] = 'x';
                pl += print_num(pb + pl, sizeof(pb) - (size_t)pl, num,
                                UINT32_C(16), 16, -1, FLAG_ZEROPAD);
                int pad = width - pl;
                if (!(flags & FLAG_LEFT)) {
                    for (int k = 0; k < pad; k++) {
                        if (size > 0U && written < size - 1U) buf[written++] = ' ';
                        needed++;
                    }
                    for (int k = 0; k < pl; k++) {
                        if (size > 0U && written < size - 1U) buf[written++] = pb[k];
                        needed++;
                    }
                } else {
                    for (int k = 0; k < pl; k++) {
                        if (size > 0U && written < size - 1U) buf[written++] = pb[k];
                        needed++;
                    }
                    for (int k = 0; k < pad; k++) {
                        if (size > 0U && written < size - 1U) buf[written++] = ' ';
                        needed++;
                    }
                }
                break;
            }

            case '%':
                if (size > 0U && written < size - 1U) buf[written++] = '%';
                needed++;
                break;

#ifndef KERNEL
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                /* Floats are userland-only (see fp_format's header). */
                double dv = va_arg(args, double);
                char nb[512];
                int up = (*fmt >= 'A' && *fmt <= 'Z');
                char lspec = up ? (char)(*fmt + 32) : *fmt;
                int ln = fp_format(nb, dv, lspec, precision, flags, up);
                int pad = width - ln;
                if (!(flags & FLAG_LEFT)) {
                    if (flags & FLAG_ZEROPAD) {
                        /* zero-fill goes AFTER any sign/space prefix */
                        int sk = (nb[0] == '-' || nb[0] == '+' || nb[0] == ' ') ? 1 : 0;
                        for (int k = 0; k < sk; k++) {
                            if (size > 0U && written < size - 1U) buf[written++] = nb[k];
                            needed++;
                        }
                        for (int k = 0; k < pad; k++) {
                            if (size > 0U && written < size - 1U) buf[written++] = '0';
                            needed++;
                        }
                        for (int k = sk; k < ln; k++) {
                            if (size > 0U && written < size - 1U) buf[written++] = nb[k];
                            needed++;
                        }
                    } else {
                        for (int k = 0; k < pad; k++) {
                            if (size > 0U && written < size - 1U) buf[written++] = ' ';
                            needed++;
                        }
                        for (int k = 0; k < ln; k++) {
                            if (size > 0U && written < size - 1U) buf[written++] = nb[k];
                            needed++;
                        }
                    }
                } else {
                    for (int k = 0; k < ln; k++) {
                        if (size > 0U && written < size - 1U) buf[written++] = nb[k];
                        needed++;
                    }
                    for (int k = 0; k < pad; k++) {
                        if (size > 0U && written < size - 1U) buf[written++] = ' ';
                        needed++;
                    }
                }
                break;
            }
#endif /* !KERNEL */

            default:
                if (size > 0U && written < size - 1U) buf[written++] = '%';
                needed++;
                if (size > 0U && written < size - 1U) buf[written++] = *fmt;
                needed++;
                break;
        }
        fmt++;
    }

    if (size > 0U) buf[written] = '\0';
    return needed;
}

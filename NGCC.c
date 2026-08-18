/*
 * NGCC - NG's C Compiler
 *
 * A small C compiler written in C that compiles a subset of C directly
 * into native Windows x64 (PE32+) executables, gcc-style.
 *
 *   ngcc HelloWorld.c             -> HelloWorld.exe
 *   ngcc HelloWorld.c -o hi.exe   -> hi.exe
 *   ngcc -v / --version           -> show version
 *
 * Pipeline (fully self-contained, no external tools needed at runtime):
 *   source -> tokens -> AST -> x86-64 machine code -> PE32+ image
 *
 * Build with any MinGW-w64 gcc:
 *   gcc -O2 -Wall -Wextra -o ngcc.exe NGCC.c
 *
 * The generated .exe imports library functions from msvcrt.dll and
 * kernel32.dll through a hand-written import table, so it needs no CRT
 * startup code and no linker.
 *
 * See README.md for the full feature list and limitations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ============================================================ version */

/* NGCC version - change this when releasing a new version.
 * Query it with:  ngcc -v   or   ngcc --version */
#define NGCC_VERSION "1.0.0"

/* ============================================================ utilities */

static const char *curFile = NULL;
static char *curSrc = NULL;

static void error_at(int line, int col, const char *fmt, ...) {
    va_list ap;
    if (line > 0)
        fprintf(stderr, "NGCC: error: %s:%d:%d: ", curFile, line, col);
    else
        fprintf(stderr, "NGCC: error: %s: ", curFile ? curFile : "NGCC");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (line > 0 && curSrc) {
        int i, l = 1;
        const char *s = curSrc;
        while (*s && l < line) { if (*s == '\n') l++; s++; }
        const char *e = strchr(s, '\n');
        if (e) {
            int len = (int)(e - s);
            fprintf(stderr, "  ");
            for (i = 0; i < len && i < 160; i++)
                fputc(s[i] == '\t' ? ' ' : s[i], stderr);
            fprintf(stderr, "\n  ");
            for (i = 0; i < col - 1 && i < 160; i++) fputc(' ', stderr);
            fprintf(stderr, "^\n");
        }
    }
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "NGCC: error: out of memory\n"); exit(1); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "NGCC: error: out of memory\n"); exit(1); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ============================================================ type system */

typedef struct Node Node;
typedef struct Local Local;

typedef struct Type Type;
typedef struct Member Member;

typedef enum {
    TY_INT, TY_CHAR, TY_LONG, TY_ULONG, TY_VOID, TY_PTR,
    TY_ARRAY, TY_STRUCT, TY_FUNC, TY_DOUBLE, TY_FLOAT
} TyKind;

struct Member {
    const char *name;
    Type *ty;
    int offset;
};

struct Type {
    TyKind kind;
    int size;
    int align;
    int is_unsigned;
    Type *base;          /* TY_PTR / TY_ARRAY: element type; TY_FUNC: return type */
    int array_len;       /* TY_ARRAY */
    Type *ret;           /* TY_FUNC */
    Type **params;       /* TY_FUNC */
    int nparams;
    int variadic;
    const char *tag;     /* TY_STRUCT tag */
    Member *members;     /* TY_STRUCT */
    int nmembers;
    int is_defined;      /* TY_STRUCT: has a '{...}' definition */
};

static Type ty_int = { TY_INT, 4, 4, 0, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };
static Type ty_char = { TY_CHAR, 1, 1, 0, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };
static Type ty_uchar = { TY_CHAR, 1, 1, 1, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };
static Type ty_long = { TY_LONG, 8, 8, 0, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };
static Type ty_ulong = { TY_ULONG, 8, 8, 1, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };
static Type ty_void = { TY_VOID, 1, 1, 0, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };
static Type ty_double = { TY_DOUBLE, 8, 8, 0, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };
static Type ty_float = { TY_FLOAT, 4, 4, 0, NULL, 0, NULL, NULL, 0, 0, NULL, NULL, 0 };

static int ty_size(Type *t) {
    if (!t) return 8;
    if (t->kind == TY_ARRAY) return ty_size(t->base) * t->array_len;
    return t->size;
}
static int ty_align(Type *t) {
    if (!t) return 8;
    if (t->kind == TY_ARRAY) return ty_align(t->base);
    return t->align;
}
static int ty_elem_size(Type *t) {   /* size of the element a pointer/array points to */
    if (!t) return 8;
    if (t->kind == TY_PTR || t->kind == TY_ARRAY) return ty_size(t->base);
    return ty_size(t);
}
static int ty_is_ptr(Type *t) {
    if (!t) return 0;
    return t->kind == TY_PTR || t->kind == TY_ARRAY || t->kind == TY_FUNC;
}
static int ty_is_64(Type *t) {   /* needs 64-bit registers/ops */
    if (!t) return 1;
    return t->kind == TY_LONG || t->kind == TY_ULONG || t->kind == TY_PTR ||
           t->kind == TY_FUNC || (t->kind == TY_ARRAY && ty_size(t) > 4);
}

typedef struct { const char *name; Type *ty; } TypeDef;
static TypeDef *typedefs;
static int ntypedefs, captypedefs;
static void add_typedef(const char *name, Type *ty) {
    int i;
    for (i = 0; i < ntypedefs; i++)
        if (!strcmp(typedefs[i].name, name)) { typedefs[i].ty = ty; return; }
    if (ntypedefs == captypedefs) {
        captypedefs = captypedefs ? captypedefs * 2 : 16;
        typedefs = xrealloc(typedefs, (size_t)captypedefs * sizeof(TypeDef));
    }
    typedefs[ntypedefs].name = name;
    typedefs[ntypedefs].ty = ty;
    ntypedefs++;
}
static Type *find_typedef(const char *name) {
    int i;
    for (i = 0; i < ntypedefs; i++)
        if (!strcmp(typedefs[i].name, name)) return typedefs[i].ty;
    return NULL;
}

typedef struct { const char *tag; Type *ty; } StructDef;
static StructDef *structs;
static int nstructs, capstructs;
static Type *find_struct(const char *tag) {
    int i;
    for (i = 0; i < nstructs; i++)
        if (!strcmp(structs[i].tag, tag)) return structs[i].ty;
    return NULL;
}
static void add_struct(const char *tag, Type *ty) {
    if (nstructs == capstructs) {
        capstructs = capstructs ? capstructs * 2 : 16;
        structs = xrealloc(structs, (size_t)capstructs * sizeof(StructDef));
    }
    structs[nstructs].tag = tag;
    structs[nstructs].ty = ty;
    nstructs++;
}

static Type *new_type(TyKind kind, int size, int align) {
    Type *t = xmalloc(sizeof(Type));
    memset(t, 0, sizeof(Type));
    t->kind = kind;
    t->size = size;
    t->align = align;
    return t;
}static Type *new_ptr_type(Type *base) {
    Type *t = new_type(TY_PTR, 8, 8);
    t->base = base;
    return t;
}
static Type *new_array_type(Type *base, int len) {
    Type *t = new_type(TY_ARRAY, ty_size(base) * len, ty_align(base));
    t->base = base;
    t->array_len = len;
    return t;
}

/* opaque type for FILE etc. */
static Type ty_opaque = { TY_STRUCT, 8, 8, 0, NULL, 0, NULL, NULL, 0, 0, "opaque", NULL, 0 };

/* enumeration constants */
typedef struct { const char *name; long long val; } EnumConst;
static EnumConst *enums;
static int nenums, capenums;
static long long *find_enum_const(const char *name) {
    int i;
    for (i = 0; i < nenums; i++)
        if (!strcmp(enums[i].name, name)) return &enums[i].val;
    return NULL;
}
static void add_enum_const(const char *name, long long val) {
    if (nenums == capenums) {
        capenums = capenums ? capenums * 2 : 32;
        enums = xrealloc(enums, (size_t)capenums * sizeof(EnumConst));
    }
    enums[nenums].name = name;
    enums[nenums].val = val;
    nenums++;
}

/* global variables and function-local statics: storage in .data */
typedef struct {
    const char *name;
    Type *ty;
    int off;
    int size;
    Node *init;                 /* scalar initializer expression */
    Node **inits; int ninits;   /* array initializer list */
    int is_bss;                 /* no initializer: space is zero-filled */
    int is_extern;              /* declared 'extern' in some translation unit */
    Local *lcl;                 /* Local view for codegen (is_global=1) */
} Global;
static Global *globals;
static int nglobals, capglobals;
static Global *find_global(const char *name) {
    int i;
    for (i = 0; i < nglobals; i++)
        if (!strcmp(globals[i].name, name)) return &globals[i];
    return NULL;
}

/* ============================================================ lexer */

typedef enum { TK_IDENT, TK_NUM, TK_STR, TK_PUNCT, TK_EOF } TK;

typedef struct {
    TK kind;
    const char *text;          /* identifier name or punctuation string */
    long long num;             /* TK_NUM value */
    unsigned char *bytes;      /* TK_STR decoded bytes */
    int blen;
    int line, col;
    int ll;                    /* integer literal has an l/L suffix (64-bit) */
    int un;                    /* integer literal has a u/U suffix (unsigned) */
    double dval;               /* TK_NUM floating-point value */
    int is_dbl;                /* TK_NUM is a floating-point literal */
    int is_flt;                /* TK_NUM float literal (f/F suffix) */
} Token;

typedef struct {
    const char *name;
    Token *toks;
    int ntoks;
    int expanding;
} Macro;

static const char *KEYWORDS[] = {
    "int", "char", "void", "if", "else", "while", "for", "return",
    "break", "continue", "sizeof", NULL
};
static int is_keyword(const char *s) {
    int i;
    for (i = 0; KEYWORDS[i]; i++)
        if (!strcmp(s, KEYWORDS[i])) return 1;
    return 0;
}

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident_part(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_hex(int c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* token list builder */
typedef struct { Token *d; int len, cap; } TokBuf;
static void tokpush(TokBuf *b, Token t) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->d = xrealloc(b->d, (size_t)b->cap * sizeof(Token));
    }
    b->d[b->len++] = t;
}

/* local header include support: files already expanded in the current
 * translation unit (so include guards / repeated #include are harmless) */
static const char **includedFiles;
static int nincludedFiles, capIncludedFiles;
static int file_already_included(const char *path) {
    int i;
    for (i = 0; i < nincludedFiles; i++)
        if (!strcmp(includedFiles[i], path)) return 1;
    if (nincludedFiles == capIncludedFiles) {
        capIncludedFiles = capIncludedFiles ? capIncludedFiles * 2 : 16;
        includedFiles = xrealloc(includedFiles, (size_t)capIncludedFiles * sizeof(const char *));
    }
    includedFiles[nincludedFiles++] = xstrdup(path);
    return 0;
}

/* shared macro table (all tokenize calls in one translation unit see the
 * same #define set; recursive header expansion must not invalidate the
 * parent's pointer, so the table lives here instead of on the stack) */
static Macro *gMacros;
static int gNmacros;

static double pow10(double e);
/* parse a floating-point literal from src[i..]; advances *ip and *colp */
static double parse_float(const char *src, int *ip, int sl, int sc, int *colp) {
    int i = *ip, i_at_call = *ip;
    double v = 0, frac = 0, scale = 1, exp = 0, esign = 1;
    int have_digits = 0;
    while (i > 0 && is_digit((unsigned char)src[i - 1])) i--;
    while (i >= 0 && (src[i] == '-' || src[i] == '+')) i--;
    if (i < 0) i = 0;
    /* integer part */
    while (src[i] >= '0' && src[i] <= '9') { v = v * 10 + (src[i] - '0'); i++; have_digits = 1; }
    /* fraction */
    if (src[i] == '.') {
        i++;
        while (src[i] >= '0' && src[i] <= '9') {
            frac = frac * 10 + (src[i] - '0');
            scale *= 10;
            i++; have_digits = 1;
        }
    }
    /* exponent */
    if (src[i] == 'e' || src[i] == 'E') {
        i++;
        if (src[i] == '-' || src[i] == '+') { if (src[i] == '-') esign = -1; i++; }
        while (src[i] >= '0' && src[i] <= '9') { exp = exp * 10 + (src[i] - '0'); i++; }
    }
    if (!have_digits)
        error_at(sl, sc, "invalid floating-point literal");
    v = (v + frac / scale) * pow10(esign * exp);
    *colp += (i - i_at_call);
    *ip = i;
    return v;
}
static double pow10(double e) {
    double r = 1;
    int neg = e < 0;
    if (neg) e = -e;
    while (e-- > 0) r *= 10;
    return neg ? 1.0 / r : r;
}

static int macro_index(const char *name) {
    int i;
    for (i = 0; i < gNmacros; i++)
        if (!strcmp(gMacros[i].name, name)) return i;
    return -1;
}

/* recursively expand a macro; returns malloc'd token array (or NULL) */
static Token *expand_macro(const char *name, int depth, int *outN) {
    int mi, i;
    TokBuf out = { NULL, 0, 0 };
    if (depth > 64) return NULL;
    mi = macro_index(name);
    if (mi < 0 || gMacros[mi].expanding) return NULL;
    gMacros[mi].expanding = 1;
    for (i = 0; i < gMacros[mi].ntoks; i++) {
        Token *t = &gMacros[mi].toks[i];
        if (t->kind == TK_IDENT && strcmp(t->text, name) != 0 &&
            macro_index(t->text) >= 0) {
            int n2;
            Token *e = expand_macro(t->text, depth + 1, &n2);
            if (e) {
                int j;
                for (j = 0; j < n2; j++) tokpush(&out, e[j]);
                free(e);
            } else tokpush(&out, *t);
        } else tokpush(&out, *t);
    }
    gMacros[mi].expanding = 0;
    *outN = out.len;
    return out.d ? out.d : (Token *)xmalloc(sizeof(Token)); /* empty */
}

static Token *tokenize(const char *src, int plain, int *outN) {
    TokBuf toks = { NULL, 0, 0 };
    int i = 0, line = 1, col = 1;
    long n = (long)strlen(src);

    if (n >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) { i = 3; col = 4; } /* UTF-8 BOM */

    while (i < n) {
        int c = (unsigned char)src[i];
        if (c == '\n') { i++; line++; col = 1; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; col++; continue; }
        /* comments */
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') { i++; col++; }
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            int sl = line, sc = col;
            i += 2; col += 2;
            for (;;) {
                if (i + 1 >= n)
                    error_at(sl, sc, "unterminated block comment");
                if (src[i] == '*' && src[i + 1] == '/') { i += 2; col += 2; break; }
                if (src[i] == '\n') { i++; line++; col = 1; } else { i++; col++; }
            }
            continue;
        }
        /* preprocessor directives */
        if (!plain && c == '#') {
            int sl = line, sc = col;
            char word[32];
            int wlen = 0;
            i++; col++;
            while (i < n && (src[i] == ' ' || src[i] == '\t')) { i++; col++; }
            while (i < n && ((src[i] >= 'a' && src[i] <= 'z') ||
                             (src[i] >= 'A' && src[i] <= 'Z')) && wlen < 31)
                word[wlen++] = src[i++], col++;
            word[wlen] = 0;
            if (!strcmp(word, "include")) {
                /* #include "local.h"  ->  expand the header's tokens in place;
                 * #include <...>       ->  system header, ignored */
                while (i < n && (src[i] == ' ' || src[i] == '\t')) { i++; col++; }
                if (i < n && src[i] == '"') {
                    char hname[512];
                    int hlen = 0;
                    i++; col++;
                    while (i < n && src[i] != '"' && hlen < 510) {
                        hname[hlen++] = src[i++]; col++;
                    }
                    if (i < n && src[i] == '"') { i++; col++; }
                    while (i < n && src[i] != '\n') { i++; col++; }
                    hname[hlen] = 0;
                    if (hlen > 0) {
                        /* try the header next to the current source file,
                         * then relative to the working directory */
                        char path[600];
                        FILE *hf = NULL;
                        long hsz;
                        char *hbuf;
                        int nsub, j;
                        Token *sub;
                        const char *slash = strrchr(curFile, '\\');
                        const char *slash2 = strrchr(curFile, '/');
                        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
                        if (slash) {
                            int dlen = (int)(slash - curFile + 1);
                            if (dlen + hlen < (int)sizeof(path)) {
                                memcpy(path, curFile, (size_t)dlen);
                                memcpy(path + dlen, hname, (size_t)hlen + 1);
                                hf = fopen(path, "rb");
                            }
                        }
                        if (!hf) hf = fopen(hname, "rb");
                        if (hf) {
                            hsz = 0;
                            fseek(hf, 0, SEEK_END);
                            hsz = ftell(hf);
                            fseek(hf, 0, SEEK_SET);
                            hbuf = xmalloc((size_t)hsz + 1);
                            if (hsz > 0 && fread(hbuf, 1, (size_t)hsz, hf) != (size_t)hsz) {
                                fclose(hf);
                                free(hbuf);
                                error_at(sl, sc, "cannot read header '%s'", hname);
                            }
                            fclose(hf);
                            hbuf[hsz] = 0;
                            if (!file_already_included(hname)) {
                                sub = tokenize(hbuf, plain, &nsub);
                                for (j = 0; j < nsub; j++) {
                                    if (sub[j].kind == TK_EOF) continue;
                                    tokpush(&toks, sub[j]);
                                }
                                free(sub);
                            }
                            free(hbuf);
                        }
                    }
                } else {
                    while (i < n && src[i] != '\n') { i++; col++; }
                }
                continue;
            }
            if (!strcmp(word, "define")) {
                char name[256];
                int nlen = 0;
                char *rest;
                int restLen, j;
                Token *body;
                int nbody;
                Macro *macros2;
                while (i < n && (src[i] == ' ' || src[i] == '\t')) { i++; col++; }
                while (i < n && is_ident_part((unsigned char)src[i]) && nlen < 255)
                    name[nlen++] = src[i++], col++;
                name[nlen] = 0;
                if (!nlen) error_at(sl, sc, "expected a macro name after #define");
                j = i;
                while (j < n && src[j] != '\n') j++;
                restLen = j - i;
                rest = xmalloc((size_t)restLen + 1);
                memcpy(rest, src + i, (size_t)restLen);
                rest[restLen] = 0;
                body = tokenize(rest, 1, &nbody);
                if (nbody > 0 && body[nbody - 1].kind == TK_EOF) nbody--;  /* drop body EOF */
                macros2 = xrealloc(gMacros, (size_t)(gNmacros + 1) * sizeof(Macro));
                macros2[gNmacros].name = xstrdup(name);
                macros2[gNmacros].toks = body;
                macros2[gNmacros].ntoks = nbody;
                macros2[gNmacros].expanding = 0;
                gMacros = macros2;
                gNmacros++;
                free(rest);
                i = j; col += restLen;
                continue;
            }
            /* unknown directive: skip */
            while (i < n && src[i] != '\n') { i++; col++; }
            continue;
        }
        /* identifiers / keywords / macros */
        if (is_ident_start(c)) {
            int sl = line, sc = col;
            char *s;
            int len = 0;
            while (i + len < n && is_ident_part((unsigned char)src[i + len])) len++;
            s = xmalloc((size_t)len + 1);
            memcpy(s, src + i, (size_t)len);
            s[len] = 0;
            i += len; col += len;
            if (!plain && macro_index(s) >= 0) {
                int ne;
                Token *e = expand_macro(s, 0, &ne);
                if (ne > 0) {
                    int j;
                    for (j = 0; j < ne; j++) tokpush(&toks, e[j]);
                    free(e);
                    free(s);
                    continue;
                }
                free(e);
            }
            tokpush(&toks, (Token){ TK_IDENT, s, 0, NULL, 0, sl, sc });
            continue;
        }
        /* numbers */
        if (is_digit(c)) {
            int sl = line, sc = col;
            long long v = 0;
            int ll = 0, un = 0;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2; col += 2;
                while (i < n && is_hex((unsigned char)src[i])) {
                    int d = src[i];
                    if (d >= '0' && d <= '9') d -= '0';
                    else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                    else d = d - 'A' + 10;
                    v = (v << 4) | d;
                    i++; col++;
                }
            } else {
                while (i < n && is_digit((unsigned char)src[i])) {
                    v = v * 10 + (src[i] - '0');
                    i++; col++;
                }
            }
            /* floating-point literal: digits '.' digits, or exponent e/E */
            if ((i < n && src[i] == '.') || (i < n && (src[i] == 'e' || src[i] == 'E'))) {
                int start = i - (c == '0' && i >= 2 && (src[i-2] == 'x' || src[i-2] == 'X') ? 2 : 0);
                int isHexLit = (c == '0' && i - 2 >= 0 && (src[i-2] == 'x' || src[i-2] == 'X'));
                int isFlt = 0;
                if (isHexLit)
                    error_at(sl, sc, "hex floating-point literals are not supported");
                (void)start; (void)isHexLit;
                tokpush(&toks, (Token){ TK_NUM, NULL, 0, NULL, 0, sl, sc });
                toks.d[toks.len - 1].is_dbl = 1;
                toks.d[toks.len - 1].dval = parse_float(src, &i, sl, sc, &col);
                /* float suffix: 1.5f / 2.5F  (also .5f, 1e3f) */
                if (i < n && (src[i] == 'f' || src[i] == 'F')) {
                    isFlt = 1;
                    i++; col++;
                }
                toks.d[toks.len - 1].is_flt = isFlt;
                continue;
            }
            /* integer suffixes: u, l, ll, ul, ull (any case, any order) */
            for (;;) {
                if (i < n && (src[i] == 'u' || src[i] == 'U')) { un = 1; i++; col++; continue; }
                if (i < n && (src[i] == 'l' || src[i] == 'L')) {
                    ll = 1; i++; col++;
                    if (i < n && (src[i] == 'l' || src[i] == 'L')) { i++; col++; }
                    continue;
                }
                break;
            }
            tokpush(&toks, (Token){ TK_NUM, NULL, v, NULL, 0, sl, sc });
            toks.d[toks.len - 1].ll = ll;
            toks.d[toks.len - 1].un = un;
            continue;
        }
        /* character literal */
        if (c == '\'') {
            int sl = line, sc = col;
            int v;
            i++; col++;
            if (i >= n || src[i] == '\n' || src[i] == '\'')
                error_at(sl, sc, "invalid character literal");
            if (src[i] == '\\') {
                i++; col++;
                if (i >= n) error_at(sl, sc, "unterminated escape sequence");
                switch (src[i]) {
                    case 'n': v = 10; break;
                    case 't': v = 9; break;
                    case 'r': v = 13; break;
                    case 'a': v = 7; break;
                    case 'b': v = 8; break;
                    case 'f': v = 12; break;
                    case 'v': v = 11; break;
                    case '\\': v = '\\'; break;
                    case '\'': v = '\''; break;
                    case '"': v = '"'; break;
                    case '0': v = 0; break;
                    case 'x': {
                        v = 0;
                        i++; col++;
                        if (i < n && is_hex((unsigned char)src[i])) {
                            int d = src[i];
                            if (d >= '0' && d <= '9') d -= '0';
                            else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                            else d = d - 'A' + 10;
                            v |= d << 4;
                            i++; col++;
                        } else error_at(sl, sc, "invalid \\x escape");
                        if (i < n && is_hex((unsigned char)src[i])) {
                            int d = src[i];
                            if (d >= '0' && d <= '9') d -= '0';
                            else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                            else d = d - 'A' + 10;
                            v |= d;
                            i++; col++;
                        }
                        break;
                    }
                    default:
                        error_at(sl, sc, "unknown escape sequence '\\%c'", src[i]);
                }
                i++; col++;
            } else {
                v = (unsigned char)src[i];
                i++; col++;
            }
            if (i >= n || src[i] != '\'')
                error_at(sl, sc, "character literal must contain exactly one character");
            i++; col++;
            tokpush(&toks, (Token){ TK_NUM, NULL, v, NULL, 0, sl, sc });
            continue;
        }
        /* string literal */
        if (c == '"') {
            int sl = line, sc = col;
            unsigned char *bytes;
            int blen = 0, bcap = 16;
            i++; col++;
            bytes = xmalloc(bcap);
            while (i < n && src[i] != '"') {
                if (src[i] == '\\') {
                    int v;
                    i++; col++;
                    if (i >= n) error_at(sl, sc, "unterminated string literal");
                    switch (src[i]) {
                        case 'n': v = 10; break;
                        case 't': v = 9; break;
                        case 'r': v = 13; break;
                        case 'a': v = 7; break;
                        case 'b': v = 8; break;
                        case 'f': v = 12; break;
                        case 'v': v = 11; break;
                        case '\\': v = '\\'; break;
                        case '\'': v = '\''; break;
                        case '"': v = '"'; break;
                        case '0': v = 0; break;
                        case 'x': {
                            v = 0;
                            i++; col++;
                            if (i < n && is_hex((unsigned char)src[i])) {
                                int d = src[i];
                                if (d >= '0' && d <= '9') d -= '0';
                                else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                                else d = d - 'A' + 10;
                                v |= d << 4;
                                i++; col++;
                            } else error_at(sl, sc, "invalid \\x escape");
                            if (i < n && is_hex((unsigned char)src[i])) {
                                int d = src[i];
                                if (d >= '0' && d <= '9') d -= '0';
                                else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
                                else d = d - 'A' + 10;
                                v |= d;
                                i++; col++;
                            }
                            break;
                        }
                        default:
                            error_at(sl, sc, "unknown escape sequence '\\%c'", src[i]);
                    }
                    i++; col++;
                    if (blen == bcap) { bcap *= 2; bytes = xrealloc(bytes, bcap); }
                    bytes[blen++] = (unsigned char)v;
                } else {
                    unsigned char ch = (unsigned char)src[i];
                    if (ch == '\n') error_at(sl, sc, "unterminated string literal");
                    if (blen == bcap) { bcap *= 2; bytes = xrealloc(bytes, bcap); }
                    bytes[blen++] = ch;
                    i++; col++;
                }
            }
            if (i >= n) error_at(sl, sc, "unterminated string literal");
            i++; col++;
            tokpush(&toks, (Token){ TK_STR, NULL, 0, bytes, blen, sl, sc });
            continue;
        }
        /* punctuation */
        if (i + 2 < n && (!strncmp(src + i, "<<=", 3) || !strncmp(src + i, ">>=", 3) ||
                          !strncmp(src + i, "...", 3))) {
            char *s = xmalloc(4);
            memcpy(s, src + i, 3); s[3] = 0;
            tokpush(&toks, (Token){ TK_PUNCT, s, 0, NULL, 0, line, col });
            i += 3; col += 3;
            continue;
        }
        {
            static const char *two[] = { "->", "+=", "-=", "*=", "/=", "%=", "<<", ">>",
                "==", "!=", "<=", ">=", "&&", "||", "++", "--", "&=", "|=", "^=", NULL };
            int k;
            for (k = 0; two[k]; k++) {
                if (i + 1 < n && !strncmp(src + i, two[k], 2)) {
                    char *s = xmalloc(3);
                    memcpy(s, src + i, 2); s[2] = 0;
                    tokpush(&toks, (Token){ TK_PUNCT, s, 0, NULL, 0, line, col });
                    i += 2; col += 2;
                    break;
                }
            }
            if (two[k]) continue;
        }
        if (strchr("+-*/%<>=!(){};,~&|^[]?:.", c)) {
            char *s = xmalloc(2);
            s[0] = (char)c; s[1] = 0;
            tokpush(&toks, (Token){ TK_PUNCT, s, 0, NULL, 0, line, col });
            i++; col++;
            continue;
        }
        error_at(line, col, "unexpected character '%c'", c);
    }
    tokpush(&toks, (Token){ TK_EOF, NULL, 0, NULL, 0, line, col });
    *outN = toks.len;
    return toks.d ? toks.d : (Token *)xmalloc(sizeof(Token));
}

/* ============================================================ parser */

typedef enum {
    ND_NUM, ND_STR, ND_VAR, ND_ASSIGN,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
    ND_EQ, ND_NE, ND_LT, ND_LE, ND_GT, ND_GE,
    ND_LAND, ND_LOR, ND_AND, ND_OR, ND_XOR, ND_SHL, ND_SHR,
    ND_NEG, ND_NOT, ND_BITNOT, ND_ADDR, ND_DEREF, ND_CAST, ND_PREINC, ND_PREDEC, ND_POSTINC, ND_POSTDEC, ND_INDEX,
    ND_CALL, ND_IF, ND_WHILE, ND_FOR, ND_RETURN, ND_BLOCK, ND_DECL,
    ND_DECLVAR, ND_EXPR, ND_BREAK, ND_CONTINUE, ND_MEMBER, ND_COND, ND_COMPLIT,
    ND_SWITCH, ND_COMMA
} NK;

typedef struct SwitchCase { long long val; Node *body; int label; } SwitchCase;

typedef struct Node {
    NK k;
    int line, col;
    long long v;                 /* ND_NUM (integer) */
    double dval;                 /* ND_NUM (floating-point) */
    int is_dbl;                  /* ND_NUM is a double literal */
    int is_flt;                  /* ND_NUM is a float (f/F suffix) literal */
    unsigned char *bytes; int blen; /* ND_STR */
    const char *name;            /* ND_VAR / callee name */
    int li;                      /* ND_VAR: index into fn->locals (-1 = global/func) */
    int off;                     /* local slot offset / global data offset */
    char ty;                     /* legacy: 'i','c','p','a' (kept for old paths) */
    char elem;                   /* legacy element type */
    int arr_len;                 /* legacy array count */
    Type *tp;                    /* full expression/declaration type */
    int is_global;               /* storage lives in .data (global or static local) */
    struct Node **inits; int ninits;   /* array initializer list (ND_DECLVAR) */
    int ndim_init;                     /* # of sub-lists for a multi-dim array init */
    struct Node *l, *r, *o;
    struct Node *base, *idx;     /* ND_INDEX */
    struct Node *member;         /* ND_MEMBER: base expression */
    const char *mname;           /* ND_MEMBER: member name */
    int arrow;                   /* ND_MEMBER: 1 for ->, 0 for . */
    struct Node *fn;
    struct Node **args; int nargs;
    struct Node *cond, *then, *els, *body, *init, *inc, *expr;
    struct Node **stmts; int nstmts;
    struct Node **decls; int ndecls;
    SwitchCase *cases; int ncases;   /* ND_SWITCH */
    Node *defbody;                   /* ND_SWITCH default */
    long long caselabel;             /* unused */
} Node;

typedef struct Local {
    const char *name;
    int off;
    Type *ty;
    int is_param;
    int depth;          /* block nesting level (1 = function body top) */
    int is_static;      /* function-local static: storage in .data/.bss */
    int is_global;      /* file-scope variable: storage in .data/.bss */
} Local;

typedef struct {
    const char *name;
    Type *ret;          /* return type */
    Local *locals;
    int nlocals, caplocals;
    int maxOffset, offset;       /* offset = next free stack slot */
    int nparams;
    int variadic;       /* has a '...' parameter */
    Node *body;
    int codeOff;
    int emitted;        /* code has been generated for this function */
} Func;

typedef struct {
    Token *toks;
    int pos;
    Func **funcs;
    int nfuncs, capfuncs;
    Func *cur;
    int scope;              /* current block nesting level */
    int blockStarts[512];   /* nlocals value when each scope level opened */
} Parser;

static Node *new_node(NK k) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->k = k;
    return n;
}

static void nodeline(Node *n, int line, int col) { n->line = line; n->col = col; }

static Token *peek(Parser *p, int off) {
    int i = p->pos + off;
    return &p->toks[i < 0 ? 0 : i];
}
static Token *next(Parser *p) { return &p->toks[p->pos++]; }
static int at_punct(Parser *p, const char *s) {
    Token *t = peek(p, 0);
    return t->kind == TK_PUNCT && !strcmp(t->text, s);
}
static void expect_punct(Parser *p, const char *s) {
    Token *t = peek(p, 0);
    if (t->kind != TK_PUNCT || strcmp(t->text, s))
        error_at(t->line, t->col, "expected '%s' but found '%s'", s,
                 t->kind == TK_EOF ? "end of file" : t->text);
    p->pos++;
}
static Token *expect_ident(Parser *p) {
    Token *t = peek(p, 0);
    if (t->kind != TK_IDENT)
        error_at(t->line, t->col, "expected an identifier but found '%s'",
                 t->kind == TK_EOF ? "end of file" : t->text);
    p->pos++;
    return t;
}
static int at_kw(Parser *p, const char *s) {
    Token *t = peek(p, 0);
    return t->kind == TK_IDENT && !strcmp(t->text, s);
}

/* parse type; returns base char ('i','c','v') and sets *ptr */
static Type *parse_type(Parser *p);
static Type *parse_type_base(Parser *p);

static Type *parse_struct_type(Parser *p) {
    Token *t = next(p);   /* 'struct' */
    const char *tag = NULL;
    Token *tagTok = peek(p, 0);
    if (tagTok->kind == TK_IDENT && !is_keyword(tagTok->text)) {
        tag = tagTok->text;
        p->pos++;
    }
    if (at_punct(p, "{")) {
        int offset = 0;
        int align = 1;
        int cap = 8;
        Type *existing = tag ? find_struct(tag) : NULL;
        Type *ty = existing ? existing : new_type(TY_STRUCT, 0, 8);
        p->pos++;
        ty->tag = tag;
        if (tag) add_struct(tag, ty);   /* register early so self-references work */
        ty->members = xmalloc((size_t)cap * sizeof(Member));
        ty->nmembers = 0;
        while (!at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
            Type *mty = parse_type_base(p);   /* NO '*'s: each declarator adds its own */
            for (;;) {
                Type *mmty = mty;
                Token *nameTok;
                Member *m;
                /* per-declarator '*'s:  struct Node *l, *r, *o; */
                while (at_punct(p, "*")) { p->pos++; mmty = new_ptr_type(mmty); }
                nameTok = expect_ident(p);
                if (mmty->kind == TY_VOID)
                    error_at(nameTok->line, nameTok->col, "struct member cannot be void");
                /* array member:  int arr[512]; */
                if (at_punct(p, "[")) {
                    p->pos++;
                    if (at_punct(p, "]")) p->pos++;
                    else {
                        if (peek(p, 0)->kind != TK_NUM)
                            error_at(peek(p, 0)->line, peek(p, 0)->col,
                                     "array size must be a constant integer");
                        mmty = new_array_type(mmty, (int)peek(p, 0)->num);
                        p->pos++;
                        expect_punct(p, "]");
                    }
                }
                if (ty->nmembers == cap) {
                    cap *= 2;
                    ty->members = xrealloc(ty->members, (size_t)cap * sizeof(Member));
                }
                offset = (offset + ty_align(mmty) - 1) / ty_align(mmty) * ty_align(mmty);
                m = &ty->members[ty->nmembers++];
                m->name = nameTok->text;
                m->ty = mmty;
                m->offset = offset;
                offset += ty_size(mmty);
                if (ty_align(mmty) > align) align = ty_align(mmty);
                if (at_punct(p, ",")) { p->pos++; continue; }
                break;
            }
            expect_punct(p, ";");
        }
        expect_punct(p, "}");
        ty->size = (offset + align - 1) / align * align;
        ty->align = align;
        ty->is_defined = 1;
        return ty;    }
    if (!tag)
        error_at(t->line, t->col, "expected a struct tag or '{'");
    {
        Type *st = find_struct(tag);
        if (!st) {
            /* forward declaration: register an incomplete struct type */
            st = new_type(TY_STRUCT, 0, 8);
            st->tag = tag;
            add_struct(tag, st);
        }
        return st;
    }
}

/* simple constant expression evaluator (enum values, array sizes) */
static long long parse_const_add(Parser *p);
static long long parse_const_primary(Parser *p) {
    Token *t = peek(p, 0);
    if (t->kind == TK_NUM) { p->pos++; return t->num; }
    if (t->kind == TK_IDENT) {
        long long *ev;
        p->pos++;
        if (is_keyword(t->text))
            error_at(t->line, t->col, "expected a constant expression");
        ev = find_enum_const(t->text);
        if (!ev) error_at(t->line, t->col, "undeclared identifier in constant expression");
        return *ev;
    }
    if (at_punct(p, "(")) {
        long long v;
        p->pos++;
        v = parse_const_add(p);
        expect_punct(p, ")");
        return v;
    }
    if (at_punct(p, "-")) { p->pos++; return -parse_const_primary(p); }
    if (at_punct(p, "+")) { p->pos++; return parse_const_primary(p); }
    if (at_punct(p, "!")) { p->pos++; return !parse_const_primary(p); }
    error_at(t->line, t->col, "expected a constant expression");
    return 0;
}
static long long parse_const_mul(Parser *p) {
    long long v = parse_const_primary(p);
    for (;;) {
        if (at_punct(p, "*")) { p->pos++; v *= parse_const_primary(p); }
        else if (at_punct(p, "/")) { p->pos++; v /= parse_const_primary(p); }
        else if (at_punct(p, "%")) { p->pos++; v %= parse_const_primary(p); }
        else break;
    }
    return v;
}
static long long parse_const_add(Parser *p) {
    long long v = parse_const_mul(p);
    for (;;) {
        if (at_punct(p, "+")) { p->pos++; v += parse_const_mul(p); }
        else if (at_punct(p, "-")) { p->pos++; v -= parse_const_mul(p); }
        else break;
    }
    return v;
}
static long long parse_const_expr(Parser *p) { return parse_const_add(p); }

/* read a base type (no '*'s) */
static Type *parse_type_base(Parser *p) {
    Token *t = peek(p, 0);
    Type *ty;
    if (t->kind != TK_IDENT)
        error_at(t->line, t->col, "expected a type but found '%s'",
                 t->kind == TK_EOF ? "end of file" : t->text);
    if (!strcmp(t->text, "int") || !strcmp(t->text, "signed")) { p->pos++; ty = &ty_int; }
    else if (!strcmp(t->text, "double")) { p->pos++; ty = &ty_double; }
    else if (!strcmp(t->text, "float")) { p->pos++; ty = &ty_float; }
    else if (!strcmp(t->text, "char")) { p->pos++; ty = &ty_char; }
    else if (!strcmp(t->text, "long")) {
        p->pos++;
        if (at_kw(p, "long")) p->pos++;
        ty = &ty_long;
    }
    else if (!strcmp(t->text, "unsigned")) {
        p->pos++;
        if (at_kw(p, "char")) { p->pos++; ty = &ty_uchar; }
        else {
            if (at_kw(p, "long")) { p->pos++; if (at_kw(p, "long")) p->pos++; }
            if (at_kw(p, "int")) p->pos++;
            ty = &ty_ulong;
        }
    }
    else if (!strcmp(t->text, "signed")) {
        p->pos++;
        if (at_kw(p, "char")) { p->pos++; ty = &ty_char; }
        else { if (at_kw(p, "int")) p->pos++; ty = &ty_int; }
    }
    else if (!strcmp(t->text, "void")) { p->pos++; ty = &ty_void; }
    else if (!strcmp(t->text, "size_t")) { p->pos++; ty = &ty_ulong; }
    else if (!strcmp(t->text, "va_list")) { p->pos++; ty = new_ptr_type(&ty_char); }
    else if (!strcmp(t->text, "FILE")) { p->pos++; ty = &ty_opaque; }
    else if (!strcmp(t->text, "const") || !strcmp(t->text, "volatile")) {
        p->pos++;
        return parse_type_base(p);
    }
    else if (!strcmp(t->text, "struct")) { ty = parse_struct_type(p); }
    else if (!strcmp(t->text, "enum")) {
        p->pos++;
        if (peek(p, 0)->kind == TK_IDENT && !is_keyword(peek(p, 0)->text) &&
            !at_punct(p, "{")) p->pos++;   /* optional tag, ignored */
        if (at_punct(p, "{")) {
            long long val = 0;
            p->pos++;
            while (!at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
                Token *ct = expect_ident(p);
                if (at_punct(p, "=")) {
                    p->pos++;
                    val = parse_const_expr(p);
                }
                add_enum_const(ct->text, val);
                val++;
                if (at_punct(p, ",")) { p->pos++; continue; }
                break;
            }
            expect_punct(p, "}");
        }
        ty = &ty_int;
    }
    else {
        Type *td = find_typedef(t->text);
        if (td) { p->pos++; ty = td; }
        else if (find_struct(t->text)) { p->pos++; ty = find_struct(t->text); }
        else
            error_at(t->line, t->col,
                     "expected a type (int, char, long, unsigned, void, struct or a typedef) but found '%s'",
                     t->kind == TK_EOF ? "end of file" : t->text);
    }
    return ty;
}

/* base type + pointer stars; used for function returns and parameters */
static Type *parse_type(Parser *p) {
    Type *ty = parse_type_base(p);
    while (at_punct(p, "*")) { p->pos++; ty = new_ptr_type(ty); }
    return ty;
}

/* find the innermost visible local with this name (block-scope aware) */
static int find_local(Parser *p, const char *name) {
    int i;
    for (i = p->cur->nlocals - 1; i >= 0; i--)
        if (p->cur->locals[i].depth <= p->scope &&
            !strcmp(p->cur->locals[i].name, name)) return i;
    return -1;
}

/* is there a local with this name declared in the current block? */
static int local_in_block(Parser *p, const char *name) {
    int i;
    for (i = p->blockStarts[p->scope]; i < p->cur->nlocals; i++)
        if (!strcmp(p->cur->locals[i].name, name)) return i;
    return -1;
}

static int add_local(Func *fn, const char *name, Type *ty, int is_param, int depth) {
    int size;
    if (fn->nlocals == fn->caplocals) {
        fn->caplocals = fn->caplocals ? fn->caplocals * 2 : 16;
        fn->locals = xrealloc(fn->locals, (size_t)fn->caplocals * sizeof(Local));
    }
    if (is_param && !(ty->kind == TY_STRUCT && ty->size > 8)) size = 8;   /* scalar/pointer params in 8-byte slots */
    else {
        size = ty_size(ty);
        if (size == 0) size = 8;
        size = (size + 7) & ~7;
        if (size < 8) size = 8;
    }
    fn->offset += size;
    fn->locals[fn->nlocals].name = name;
    fn->locals[fn->nlocals].off = fn->offset;
    fn->locals[fn->nlocals].ty = ty;
    fn->locals[fn->nlocals].is_param = is_param;
    fn->locals[fn->nlocals].depth = depth;
    fn->locals[fn->nlocals].is_static = 0;
    fn->locals[fn->nlocals].is_global = 0;
    fn->nlocals++;
    if (fn->offset > fn->maxOffset) fn->maxOffset = fn->offset;
    return fn->offset;
}

/* function-local static: storage in .data at gloff, no stack slot */
static int add_local_static(Func *fn, const char *name, Type *ty, int gloff, int depth) {
    if (fn->nlocals == fn->caplocals) {
        fn->caplocals = fn->caplocals ? fn->caplocals * 2 : 16;
        fn->locals = xrealloc(fn->locals, (size_t)fn->caplocals * sizeof(Local));
    }
    fn->locals[fn->nlocals].name = name;
    fn->locals[fn->nlocals].off = gloff;
    fn->locals[fn->nlocals].ty = ty;
    fn->locals[fn->nlocals].is_param = 0;
    fn->locals[fn->nlocals].depth = depth;
    fn->locals[fn->nlocals].is_static = 1;
    fn->locals[fn->nlocals].is_global = 0;
    fn->nlocals++;
    return gloff;
}

/* type-kind for codegen: 'i' int32, 'c' char, 'l' long long, 'u' unsigned 64,
 * 'p' pointer, 's' struct value, 'v' void, 'd' double, 'f' float (32-bit) */
static char type_kind(Type *t) {
    if (!t) return 'p';
    switch (t->kind) {
    case TY_INT: return 'i';
    case TY_CHAR: return 'c';
    case TY_LONG: return 'l';
    case TY_ULONG: return 'u';
    case TY_PTR: case TY_FUNC: case TY_ARRAY: return 'p';
    case TY_STRUCT: return 's';
    case TY_VOID: return 'v';
    case TY_DOUBLE: return 'd';
    case TY_FLOAT: return 'f';
    }
    return 'i';
}

static int find_func(Parser *p, const char *name) {
    int i;
    for (i = 0; i < p->nfuncs; i++)
        if (!strcmp(p->funcs[i]->name, name)) return i;
    return -1;
}

/* ---- expressions ---- */

static Node *parse_expr(Parser *p);
static Node *parse_assign(Parser *p);
static Node *parse_stmt(Parser *p);
static Func *parse_function(Parser *p);

static Node *parse_primary(Parser *p) {
    Token *t = peek(p, 0);
    Node *n;
    if (t->kind == TK_NUM) {
        p->pos++;
        n = new_node(ND_NUM);
        n->v = t->num;
        n->dval = t->dval;
        n->is_dbl = t->is_dbl;
        n->is_flt = t->is_flt;
        n->tp = t->is_flt ? &ty_float :
                (t->is_dbl ? &ty_double :
                (t->ll ? (t->un ? &ty_ulong : &ty_long) : &ty_int));
        nodeline(n, t->line, t->col);
        return n;
    }
    if (t->kind == TK_STR) {
        /* adjacent string literals:  "a" "b"  ->  "ab" */
        int blen = t->blen, cap = blen + 1, off = 0;
        unsigned char *bytes = xmalloc((size_t)cap);
        p->pos++;
        memcpy(bytes, t->bytes, (size_t)t->blen);
        off = t->blen;
        while (peek(p, 0)->kind == TK_STR) {
            Token *s = peek(p, 0);
            if (off + s->blen + 1 > cap) {
                cap = (off + s->blen + 1) * 2;
                bytes = xrealloc(bytes, (size_t)cap);
            }
            memcpy(bytes + off, s->bytes, (size_t)s->blen);
            off += s->blen;
            p->pos++;
        }
        if (off + 1 > cap) bytes = xrealloc(bytes, (size_t)(off + 1));
        bytes[off] = 0;
        n = new_node(ND_STR);
        n->bytes = bytes;
        n->blen = off;
        nodeline(n, t->line, t->col);
        return n;
    }
    if (t->kind == TK_IDENT) {
        long long *ev;
        p->pos++;
        if (is_keyword(t->text))
            error_at(t->line, t->col, "'%s' is a keyword and cannot be used as a value", t->text);
        if (!strcmp(t->text, "NULL")) {
            n = new_node(ND_NUM);
            n->v = 0;
            nodeline(n, t->line, t->col);
            return n;
        }
        if (!strcmp(t->text, "SEEK_SET") || !strcmp(t->text, "SEEK_CUR") ||
            !strcmp(t->text, "SEEK_END")) {
            n = new_node(ND_NUM);
            n->v = !strcmp(t->text, "SEEK_SET") ? 0 : (!strcmp(t->text, "SEEK_CUR") ? 1 : 2);
            nodeline(n, t->line, t->col);
            return n;
        }
        ev = find_enum_const(t->text);
        if (ev) {
            n = new_node(ND_NUM);
            n->v = *ev;
            nodeline(n, t->line, t->col);
            return n;
        }
        n = new_node(ND_VAR);
        n->name = t->text;
        n->li = -1;
        if (p->cur) n->li = find_local(p, t->text);
        nodeline(n, t->line, t->col);
        return n;
    }
    if (t->kind == TK_PUNCT && !strcmp(t->text, "(")) {
        p->pos++;
        n = parse_expr(p);
        expect_punct(p, ")");
        return n;
    }
    error_at(t->line, t->col, "expected an expression but found '%s'",
             t->kind == TK_EOF ? "end of file" : t->text);
    return NULL;
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    for (;;) {
        Token *t = peek(p, 0);
        if (at_punct(p, "(")) {
            Node *call = new_node(ND_CALL);
            p->pos++;
            call->fn = n;
            if (!at_punct(p, ")")) {
                int cap = 8;
                call->args = xmalloc((size_t)cap * sizeof(Node *));
                for (;;) {
                    if (call->nargs == cap) { cap *= 2; call->args = xrealloc(call->args, (size_t)cap * sizeof(Node *)); }
                    call->args[call->nargs++] = parse_assign(p);
                    if (at_punct(p, ",")) { p->pos++; continue; }
                    break;
                }
            }
            expect_punct(p, ")");
            nodeline(call, n->line, n->col);
            n = call;
            continue;
        }
        if (at_punct(p, "[")) {
            Node *ix = new_node(ND_INDEX);
            p->pos++;
            ix->base = n;
            ix->idx = parse_expr(p);
            expect_punct(p, "]");
            nodeline(ix, n->line, n->col);
            n = ix;
            continue;
        }
        if (at_punct(p, ".") || at_punct(p, "->")) {
            int arrow = at_punct(p, "->");
            Node *m = new_node(ND_MEMBER);
            p->pos++;
            m->member = n;
            m->mname = expect_ident(p)->text;
            m->arrow = arrow;
            nodeline(m, t->line, t->col);
            n = m;
            continue;
        }
        if (t->kind == TK_PUNCT && (!strcmp(t->text, "++") || !strcmp(t->text, "--"))) {
            Node *u = new_node(!strcmp(t->text, "++") ? ND_POSTINC : ND_POSTDEC);
            p->pos++;
            u->o = n;
            nodeline(u, t->line, t->col);
            n = u;
            continue;
        }
        break;
    }
    return n;
}

/* best-effort sizeof of an expression (evaluated at compile time) */
static long long sizeof_expr(Parser *p, Node *e) {
    int li;
    switch (e->k) {
    case ND_VAR:
        li = find_local(p, e->name);
        if (li < 0) {
            Global *g = find_global(e->name);
            if (g) return ty_size(g->ty);
            error_at(e->line, e->col, "undeclared variable '%s'", e->name);
        }
        return ty_size(p->cur->locals[li].ty);
    case ND_STR: return e->blen + 1;
    case ND_ADDR: return 8;
    case ND_INDEX: return ty_size(&ty_int);
    default: return 4;   /* int-sized */
    }
}

/* is token at index j (from stream start) a type name? */
static int is_type_name(const char *s);
static int peek2is_type(Parser *p, int j) {
    Token *tk = &p->toks[j];
    if (tk->kind == TK_EOF) return 0;
    if (tk->kind != TK_IDENT) return 0;
    return is_type_name(tk->text);
}

/* is this identifier a type name (keyword, builtin, typedef or struct tag)? */
static int is_type_name(const char *s) {
    if (!strcmp(s, "int") || !strcmp(s, "char") || !strcmp(s, "long") ||
        !strcmp(s, "unsigned") || !strcmp(s, "signed") || !strcmp(s, "void") ||
        !strcmp(s, "double") || !strcmp(s, "float") ||
        !strcmp(s, "const") || !strcmp(s, "volatile") || !strcmp(s, "size_t") ||
        !strcmp(s, "va_list") || !strcmp(s, "FILE") || !strcmp(s, "struct") ||
        !strcmp(s, "enum"))
        return 1;
    if (find_typedef(s) || find_struct(s)) return 1;
    return 0;
}

static Node *parse_unary(Parser *p) {
    Token *t = peek(p, 0);
    Node *n;
    /* sizeof: constant-folded at compile time */
    if (t->kind == TK_IDENT && !strcmp(t->text, "sizeof")) {
        int paren = 0;
        p->pos++;
        if (at_punct(p, "(")) { paren = 1; p->pos++; }
        t = peek(p, 0);
        if (t->kind == TK_IDENT &&
            (!strcmp(t->text, "int") || !strcmp(t->text, "char") || !strcmp(t->text, "void") ||
             !strcmp(t->text, "double") || !strcmp(t->text, "float") ||
             !strcmp(t->text, "long") || !strcmp(t->text, "unsigned") ||
             !strcmp(t->text, "size_t") || !strcmp(t->text, "va_list") || !strcmp(t->text, "FILE") ||
             !strcmp(t->text, "struct") || !strcmp(t->text, "enum") ||
             !strcmp(t->text, "const") || !strcmp(t->text, "volatile") ||
             find_typedef(t->text) || find_struct(t->text))) {
            int ptr = 0;
            long long sz;
            Type *st = NULL;
            while (at_kw(p, "const") || at_kw(p, "volatile")) p->pos++;
            t = peek(p, 0);
            if (!strcmp(t->text, "int")) { p->pos++; st = &ty_int; }
            else if (!strcmp(t->text, "double")) { p->pos++; st = &ty_double; }
            else if (!strcmp(t->text, "float")) { p->pos++; st = &ty_float; }
            else if (!strcmp(t->text, "char")) { p->pos++; st = &ty_char; }
            else if (!strcmp(t->text, "long")) { p->pos++; st = &ty_long; }
            else if (!strcmp(t->text, "unsigned")) { p->pos++; st = &ty_ulong; }
            else if (!strcmp(t->text, "size_t")) { p->pos++; st = &ty_ulong; }
            else if (!strcmp(t->text, "va_list")) { p->pos++; st = new_ptr_type(&ty_char); }
            else if (!strcmp(t->text, "FILE")) { p->pos++; st = &ty_opaque; }
            else if (!strcmp(t->text, "void"))
                error_at(t->line, t->col, "sizeof(void) is not supported");
            else if (!strcmp(t->text, "struct") || !strcmp(t->text, "enum")) {
                st = parse_type_base(p);   /* consumes struct/enum + tag */
                if (st->kind == TY_STRUCT && !st->is_defined)
                    error_at(t->line, t->col, "sizeof incomplete struct type");
            } else if (find_typedef(t->text)) { p->pos++; st = find_typedef(t->text); }
            else { p->pos++; st = find_struct(t->text); }
            while (at_punct(p, "*")) { p->pos++; ptr++; }
            if (!paren)
                error_at(t->line, t->col, "sizeof(type) requires parentheses");
            expect_punct(p, ")");
            sz = ptr ? 8 : ty_size(st);
            n = new_node(ND_NUM);
            n->v = sz;
            nodeline(n, t->line, t->col);
            return n;
        }
        n = new_node(ND_NUM);
        if (paren) {
            n->v = sizeof_expr(p, parse_expr(p));
            expect_punct(p, ")");
        } else {
            n->v = sizeof_expr(p, parse_unary(p));
        }
        nodeline(n, t->line, t->col);
        return n;
    }
    /* type cast: (int)expr, (int*)expr, (unsigned char)expr;
     * compound literal: (Type){...} */
    if (t->kind == TK_PUNCT && !strcmp(t->text, "(")) {
        int j = p->pos + 1;
        /* look ahead: a run of type names followed by '*'s and then ')' or '{' */
        while (peek2is_type(p, j)) j++;
        while (p->toks[j].kind != TK_EOF && p->toks[j].kind == TK_PUNCT &&
               !strcmp(p->toks[j].text, "*")) j++;
        if (p->toks[j].kind != TK_EOF && p->toks[j].kind == TK_PUNCT &&
            (!strcmp(p->toks[j].text, ")") || !strcmp(p->toks[j].text, "{"))) {
            Type *cty;
            p->pos++;               /* '(' */
            cty = parse_type(p);    /* base type + stars */
            expect_punct(p, ")");
            if (at_punct(p, "{")) {
                /* compound literal:  (Token){ a, b, c } */
                Node *cl = new_node(ND_COMPLIT);
                int cap = 8;
                p->pos++;
                cl->tp = cty;
                cl->inits = xmalloc((size_t)cap * sizeof(Node *));
                while (!at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
                    if (cl->ninits == cap) { cap *= 2; cl->inits = xrealloc(cl->inits, (size_t)cap * sizeof(Node *)); }
                    cl->inits[cl->ninits++] = parse_assign(p);
                    if (at_punct(p, ",")) { p->pos++; continue; }
                    break;
                }
                expect_punct(p, "}");
                nodeline(cl, t->line, t->col);
                return cl;
            }
            n = new_node(ND_CAST);
            n->tp = cty;
            n->o = parse_unary(p);
            nodeline(n, t->line, t->col);
            return n;
        }
    }
    if (t->kind == TK_PUNCT) {
        if (!strcmp(t->text, "-") || !strcmp(t->text, "!") ||
            !strcmp(t->text, "~") || !strcmp(t->text, "&") || !strcmp(t->text, "*") ||
            !strcmp(t->text, "++") || !strcmp(t->text, "--")) {
            NK k;
            p->pos++;
            if (!strcmp(t->text, "-")) k = ND_NEG;
            else if (!strcmp(t->text, "!")) k = ND_NOT;
            else if (!strcmp(t->text, "~")) k = ND_BITNOT;
            else if (!strcmp(t->text, "&")) k = ND_ADDR;
            else if (!strcmp(t->text, "*")) k = ND_DEREF;
            else if (!strcmp(t->text, "++")) k = ND_PREINC;
            else k = ND_PREDEC;
            n = new_node(k);
            n->o = parse_unary(p);
            nodeline(n, t->line, t->col);
            return n;
        }
        if (!strcmp(t->text, "+")) { p->pos++; return parse_unary(p); }
    }
    return parse_postfix(p);
}

static NK binop_kind(const char *s) {
    if (!strcmp(s, "+")) return ND_ADD;
    if (!strcmp(s, "-")) return ND_SUB;
    if (!strcmp(s, "*")) return ND_MUL;
    if (!strcmp(s, "/")) return ND_DIV;
    if (!strcmp(s, "%")) return ND_MOD;
    if (!strcmp(s, "==")) return ND_EQ;
    if (!strcmp(s, "!=")) return ND_NE;
    if (!strcmp(s, "<")) return ND_LT;
    if (!strcmp(s, "<=")) return ND_LE;
    if (!strcmp(s, ">")) return ND_GT;
    if (!strcmp(s, ">=")) return ND_GE;
    if (!strcmp(s, "<<")) return ND_SHL;
    if (!strcmp(s, ">>")) return ND_SHR;
    if (!strcmp(s, "&")) return ND_AND;
    if (!strcmp(s, "|")) return ND_OR;
    if (!strcmp(s, "^")) return ND_XOR;
    if (!strcmp(s, "&&")) return ND_LAND;
    if (!strcmp(s, "||")) return ND_LOR;
    return (NK)-1;
}

static NK compound_kind(const char *s) {
    if (!strcmp(s, "+=")) return ND_ADD;
    if (!strcmp(s, "-=")) return ND_SUB;
    if (!strcmp(s, "*=")) return ND_MUL;
    if (!strcmp(s, "/=")) return ND_DIV;
    if (!strcmp(s, "%=")) return ND_MOD;
    if (!strcmp(s, "<<=")) return ND_SHL;
    if (!strcmp(s, ">>=")) return ND_SHR;
    if (!strcmp(s, "&=")) return ND_AND;
    if (!strcmp(s, "|=")) return ND_OR;
    if (!strcmp(s, "^=")) return ND_XOR;
    return (NK)-1;
}

static int is_compound(const char *s) { return compound_kind(s) != (NK)-1; }

static Node *parse_binary(Parser *p, Node *(*next_level)(Parser *), const char **ops, int nops) {
    Node *n = next_level(p);
    for (;;) {
        Token *t = peek(p, 0);
        int i;
        if (t->kind != TK_PUNCT) return n;
        for (i = 0; i < nops; i++)
            if (!strcmp(t->text, ops[i])) break;
        if (i == nops) return n;
        p->pos++;
        {
            Node *b = new_node(binop_kind(t->text));
            b->l = n;
            b->r = next_level(p);
            nodeline(b, t->line, t->col);
            n = b;
        }
    }
}

static Node *parse_lor(Parser *p);
static Node *parse_land(Parser *p);
static Node *parse_bitor(Parser *p);
static Node *parse_bitxor(Parser *p);
static Node *parse_bitand(Parser *p);
static Node *parse_equality(Parser *p);
static Node *parse_relational(Parser *p);
static Node *parse_shift(Parser *p);
static Node *parse_additive(Parser *p);
static Node *parse_multiplicative(Parser *p);

static Node *parse_lor(Parser *p) {
    static const char *ops[] = { "||" };
    return parse_binary(p, parse_land, ops, 1);
}
static Node *parse_land(Parser *p) {
    static const char *ops[] = { "&&" };
    return parse_binary(p, parse_bitor, ops, 1);
}
static Node *parse_bitor(Parser *p) {
    static const char *ops[] = { "|" };
    return parse_binary(p, parse_bitxor, ops, 1);
}
static Node *parse_bitxor(Parser *p) {
    static const char *ops[] = { "^" };
    return parse_binary(p, parse_bitand, ops, 1);
}
static Node *parse_bitand(Parser *p) {
    static const char *ops[] = { "&" };
    return parse_binary(p, parse_equality, ops, 1);
}
static Node *parse_equality(Parser *p) {
    static const char *ops[] = { "==", "!=" };
    return parse_binary(p, parse_relational, ops, 2);
}
static Node *parse_relational(Parser *p) {
    static const char *ops[] = { "<", "<=", ">", ">=" };
    return parse_binary(p, parse_shift, ops, 4);
}
static Node *parse_shift(Parser *p) {
    static const char *ops[] = { "<<", ">>" };
    return parse_binary(p, parse_additive, ops, 2);
}
static Node *parse_additive(Parser *p) {
    static const char *ops[] = { "+", "-" };
    return parse_binary(p, parse_multiplicative, ops, 2);
}
static Node *parse_multiplicative(Parser *p) {
    static const char *ops[] = { "*", "/", "%" };
    return parse_binary(p, parse_unary, ops, 3);
}

static Node *parse_assign(Parser *p) {
    Node *lhs = parse_lor(p);
    Token *t = peek(p, 0);
    if (t->kind == TK_PUNCT && !strcmp(t->text, "?")) {
        Node *n = new_node(ND_COND);
        p->pos++;
        n->cond = lhs;
        n->then = parse_expr(p);
        expect_punct(p, ":");
        n->els = parse_assign(p);
        nodeline(n, t->line, t->col);
        return n;
    }
    if (t->kind == TK_PUNCT && (!strcmp(t->text, "=") || is_compound(t->text))) {
        Node *n = new_node(ND_ASSIGN);
        p->pos++;
        n->r = parse_assign(p);
        if (strcmp(t->text, "=")) {
            /* x += e  =>  x = (x + e) */
            Node *b = new_node(compound_kind(t->text));
            b->l = lhs;
            b->r = n->r;
            nodeline(b, t->line, t->col);
            n->l = lhs;
            n->r = b;
        } else {
            n->l = lhs;
        }
        nodeline(n, t->line, t->col);
        return n;
    }
    return lhs;
}

static Node *parse_expr(Parser *p) {
    Node *n = parse_assign(p);
    while (at_punct(p, ",")) {
        Node *c = new_node(ND_COMMA);
        Token *ct = peek(p, 0);
        p->pos++;
        c->l = n;
        c->r = parse_assign(p);
        nodeline(c, ct->line, ct->col);
        n = c;
    }
    return n;
}

/* ---- statements ---- */

static Member *find_member(Type *st, const char *name);
static int add_global(const char *name, Type *ty, Node **inits, int ninits, Node *init, int is_extern);
static Local *resolve_var(Node *node);

/* is the next top-level item a function definition (as opposed to a global
 * variable/typedef declaration)?  Look-ahead without consuming tokens. */
static int is_type_start(Parser *p);
static int is_function_decl(Parser *p) {
    int save = p->pos;
    int r = 0;
    while (at_kw(p, "static") || at_kw(p, "extern") || at_kw(p, "typedef") ||
           at_kw(p, "const") || at_kw(p, "volatile") || at_kw(p, "signed"))
        p->pos++;
    if (!is_type_start(p)) { p->pos = save; return 0; }
    while (is_type_start(p)) p->pos++;
    while (at_punct(p, "*")) p->pos++;
    if (peek(p, 0)->kind != TK_IDENT) { p->pos = save; return 0; }
    p->pos++;
    r = at_punct(p, "(");
    p->pos = save;
    return r;
}

/* parse '{...}' for type ty; flatten nested lists into *out, padding each
 * inner row to ty->base->array_len with zero nodes so the flat list exactly
 * matches the array layout.  Returns the number of sub-lists when ty is an
 * array of arrays (used to deduce the outer dimension of a[][N]). */
static int parse_init_list(Parser *p, Type *ty, Node ***out, int *nout, int *cap) {
    int subcount = 0;
    p->pos++;   /* consume '{' */
    if (ty->kind == TY_ARRAY && ty->base->kind == TY_ARRAY) {
        Type *inner = ty->base;
        while (!at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
            int rowStart, k;
            subcount++;
            rowStart = *nout;
            parse_init_list(p, inner, out, nout, cap);
            /* pad this row to the inner length with zero nodes */
            for (k = *nout - rowStart; k < inner->array_len; k++) {
                Node *z;
                if (*nout == *cap) { *cap *= 2; *out = xrealloc(*out, (size_t)*cap * sizeof(Node *)); }
                z = new_node(ND_NUM);
                z->v = 0;
                (*out)[(*nout)++] = z;
            }
            if (at_punct(p, ",")) { p->pos++; continue; }
            break;
        }
        expect_punct(p, "}");
        return subcount;
    }
    while (!at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
        if (*nout == *cap) { *cap *= 2; *out = xrealloc(*out, (size_t)*cap * sizeof(Node *)); }
        (*out)[(*nout)++] = parse_assign(p);
        if (at_punct(p, ",")) { p->pos++; continue; }
        break;
    }
    expect_punct(p, "}");
    return 0;
}

static Node *parse_decl(Parser *p) {
    int is_typedef = 0, is_static = 0, is_extern = 0;
    Type *base;
    Node *n = new_node(ND_DECL);
    int cap = 8;
    n->decls = xmalloc((size_t)cap * sizeof(Node *));
    if (at_kw(p, "typedef")) { p->pos++; is_typedef = 1; }
    if (at_kw(p, "static")) { p->pos++; is_static = 1; }
    if (at_kw(p, "extern")) { p->pos++; is_extern = 1; }
    base = parse_type_base(p);
    for (;;) {
        Type *ty = base;
        int arr_len = -1;               /* -1 = not an array */
        Token *nameTok;
        Node *d = new_node(ND_DECLVAR);
        /* bare type definition:  struct Foo { ... };  or  enum { ... }; */
        if (at_punct(p, ";")) break;
        /* pointer declarators: 'int *a, *b;' - each declarator carries its own '*'s */
        while (at_punct(p, "*")) { p->pos++; ty = new_ptr_type(ty); }
        /* function pointer declarator:  Ret (*name)(params) */
        if (at_punct(p, "(") && peek(p, 1)->kind == TK_PUNCT && !strcmp(peek(p, 1)->text, "*")) {
            Type **fparams = NULL;
            int fnp = 0, fcap = 0;
            p->pos++;   /* '(' */
            p->pos++;   /* '*' */
            nameTok = expect_ident(p);
            expect_punct(p, ")");
            expect_punct(p, "(");
            if (!at_punct(p, ")")) {
                while (1) {
                    if (fnp == fcap) { fcap = fcap ? fcap * 2 : 4; fparams = xrealloc(fparams, (size_t)fcap * sizeof(Type *)); }
                    fparams[fnp++] = parse_type(p);
                    if (at_punct(p, ",")) { p->pos++; continue; }
                    break;
                }
            }
            expect_punct(p, ")");
            {
                Type *ft = new_type(TY_FUNC, 0, 0);
                ft->ret = ty;
                ft->params = fparams;
                ft->nparams = fnp;
                ty = new_ptr_type(ft);
            }
        } else {
            nameTok = expect_ident(p);
        }
        /* array declarator: name[ N ]  or  name[ ] (size deduced from initializer).
         * Multiple dims:  int a[2][3]  ->  array[2] of array[3] of int.
         * Declarator order is outer-first, so build inner-first. */
        if (at_punct(p, "[")) {
            int dims[16], ndims = 0;
            while (at_punct(p, "[")) {
                Token *st;
                p->pos++;
                if (at_punct(p, "]")) { p->pos++; dims[ndims++] = 0; }
                else {
                    st = peek(p, 0);
                    dims[ndims] = (int)parse_const_expr(p);
                    if (dims[ndims] <= 0 || dims[ndims] > 1000000)
                        error_at(st->line, st->col, "invalid array size %d", dims[ndims]);
                    ndims++;
                    expect_punct(p, "]");
                }
            }
            while (ndims > 0) {
                ndims--;
                ty = new_array_type(ty, dims[ndims]);
            }
            arr_len = dims[0];
        }
        if (is_typedef) {
            add_typedef(nameTok->text, ty);
            if (at_punct(p, ",")) { p->pos++; continue; }
            break;
        }
        if (ty->kind == TY_VOID)
            error_at(nameTok->line, nameTok->col, "variable cannot have type 'void'");
        d->name = nameTok->text;
        d->tp = ty;
        nodeline(d, nameTok->line, nameTok->col);
        /* initializer */
        if (at_punct(p, "=")) {
            p->pos++;
            if (at_punct(p, "{")) {
                int icap = 8;
                if (ty->kind != TY_ARRAY && ty->kind != TY_STRUCT)
                    error_at(peek(p, 0)->line, peek(p, 0)->col,
                             "'{...}' initializer requires an array or struct");
                d->inits = xmalloc((size_t)icap * sizeof(Node *));
                if (ty->kind == TY_ARRAY && ty->base->kind == TY_ARRAY) {
                    /* multi-dim:  int a[][3] = { {...}, {...} }  (flattened) */
                    int sub = parse_init_list(p, ty, &d->inits, &d->ninits, &icap);
                    d->ndim_init = sub;   /* # of sub-lists = outer dim */
                    if (ty->array_len == 0)
                        ty = new_array_type(ty->base, sub);
                } else {
                    p->pos++;   /* consume '{' */
                    while (!at_punct(p, "}")) {
                        if (d->ninits == icap) {
                            icap *= 2;
                            d->inits = xrealloc(d->inits, (size_t)icap * sizeof(Node *));
                        }
                        d->inits[d->ninits++] = parse_assign(p);
                        if (at_punct(p, ",")) { p->pos++; continue; }
                        break;
                    }
                    expect_punct(p, "}");
                }
            } else {
                d->init = parse_assign(p);
                if (ty->kind == TY_ARRAY) {
                    /* char s[] = "..."  or  char t[10] = "abc" */
                    if (ty->base->kind == TY_CHAR && d->init->k == ND_STR) {
                        int blen = d->init->blen, cnt = blen + 1, b;
                        d->inits = xmalloc((size_t)cnt * sizeof(Node *));
                        for (b = 0; b < blen; b++) {
                            Node *num = new_node(ND_NUM);
                            num->v = d->init->bytes[b];
                            d->inits[d->ninits++] = num;
                        }
                        { Node *num = new_node(ND_NUM); num->v = 0; d->inits[d->ninits++] = num; }
                        d->init = NULL;
                        if (ty->array_len == 0) ty = new_array_type(ty->base, cnt);
                    } else {
                        error_at(d->init->line, d->init->col,
                                 "array initializer must be '{...}' or a string literal");
                    }
                }
            }
        }
        /* int a[] = {1,2,3}  ->  size deduced from the initializer count */
        if (ty->kind == TY_ARRAY && ty->array_len == 0 && d->ninits > 0) {
            if (ty->base->kind == TY_ARRAY)
                ty = new_array_type(ty->base, d->ninits);   /* outer dim = # of sub-lists */
            else
                ty = new_array_type(ty->base, d->ninits);
        }
        if (ty->kind == TY_ARRAY && ty->array_len == 0)
            error_at(nameTok->line, nameTok->col,
                     "array size missing (only function parameters may use [])");
        if (d->ninits > 0 && ty->kind == TY_ARRAY && ty->base->kind != TY_ARRAY &&
            d->ninits > ty->array_len)
            error_at(nameTok->line, nameTok->col, "too many initializers for '%s' (%d, max %d)",
                     d->name, d->ninits, ty->array_len);
        if (d->ninits > 0 && ty->kind == TY_STRUCT && d->ninits > ty->nmembers)
            error_at(nameTok->line, nameTok->col, "too many initializers for '%s' (%d, max %d)",
                     d->name, d->ninits, ty->nmembers);
        d->tp = ty;
        d->arr_len = (ty->kind == TY_ARRAY) ? ty->array_len : 0;
        /* storage: global -> .data;  function-local static -> .data under a
         * mangled name (so the same name can be static in several functions) */
        if (is_static && p->cur) {
            char mangled[512];
            int gloff;
            int mlen = 0, nlen = (int)strlen(p->cur->name), vlen = (int)strlen(nameTok->text);
            if (mlen + 4 + nlen + 1 + vlen < (int)sizeof(mangled)) {
                memcpy(mangled + mlen, "__s_", 4); mlen += 4;
                memcpy(mangled + mlen, p->cur->name, (size_t)nlen); mlen += nlen;
                mangled[mlen++] = '_';
                memcpy(mangled + mlen, nameTok->text, (size_t)vlen); mlen += vlen;
                mangled[mlen] = 0;
            } else {
                mangled[0] = 0;
            }
            gloff = add_global(xstrdup(mangled), ty, d->inits, d->ninits, d->init, 0);
            if (local_in_block(p, nameTok->text) >= 0)
                error_at(nameTok->line, nameTok->col, "redefinition of '%s'", nameTok->text);
            d->off = add_local_static(p->cur, nameTok->text, ty, gloff, p->scope);
            d->is_global = 1;      /* storage is in .data (initialized at startup) */
        } else if (is_static || !p->cur) {
            d->off = add_global(nameTok->text, ty, d->inits, d->ninits, d->init, is_extern);
            d->is_global = 1;
        } else {
            if (local_in_block(p, nameTok->text) >= 0)
                error_at(nameTok->line, nameTok->col, "redefinition of '%s'", nameTok->text);
            d->off = add_local(p->cur, nameTok->text, ty, 0, p->scope);
        }
        if (n->ndecls == cap) { cap *= 2; n->decls = xrealloc(n->decls, (size_t)cap * sizeof(Node *)); }
        n->decls[n->ndecls++] = d;
        if (at_punct(p, ",")) { p->pos++; continue; }
        break;
    }
    expect_punct(p, ";");
    return n;
}

/* is the next token the start of a declaration? */
static int is_type_start(Parser *p) {
    Token *t = peek(p, 0);
    if (t->kind != TK_IDENT) return 0;
    if (!strcmp(t->text, "int") || !strcmp(t->text, "char") || !strcmp(t->text, "long") ||
        !strcmp(t->text, "unsigned") || !strcmp(t->text, "void") || !strcmp(t->text, "static") ||
        !strcmp(t->text, "typedef") || !strcmp(t->text, "const") || !strcmp(t->text, "struct") ||
        !strcmp(t->text, "enum") || !strcmp(t->text, "size_t") || !strcmp(t->text, "va_list") ||
        !strcmp(t->text, "FILE") || !strcmp(t->text, "signed") || !strcmp(t->text, "volatile") ||
        !strcmp(t->text, "double") || !strcmp(t->text, "float") || !strcmp(t->text, "extern"))
        return 1;
    if (find_typedef(t->text)) return 1;
    if (find_struct(t->text)) return 1;
    return 0;
}

static Node *parse_switch(Parser *p) {
    Token *t = next(p);
    Node *n = new_node(ND_SWITCH);
    int cap = 8;
    expect_punct(p, "(");
    n->cond = parse_expr(p);
    expect_punct(p, ")");
    expect_punct(p, "{");
    n->cases = xmalloc((size_t)cap * sizeof(SwitchCase));
    while (!at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
        if (at_kw(p, "case")) {
            SwitchCase *c;
            p->pos++;
            if (n->ncases == cap) {
                cap *= 2;
                n->cases = xrealloc(n->cases, (size_t)cap * sizeof(SwitchCase));
            }
            c = &n->cases[n->ncases++];
            c->val = parse_const_expr(p);
            c->label = 0;
            expect_punct(p, ":");
            c->body = new_node(ND_BLOCK);
            {
                int bcap = 8;
                c->body->stmts = xmalloc((size_t)bcap * sizeof(Node *));
                while (!at_kw(p, "case") && !at_kw(p, "default") &&
                       !at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
                    if (c->body->nstmts == bcap) {
                        bcap *= 2;
                        c->body->stmts = xrealloc(c->body->stmts, (size_t)bcap * sizeof(Node *));
                    }
                    c->body->stmts[c->body->nstmts++] = parse_stmt(p);
                }
            }
        } else if (at_kw(p, "default")) {
            p->pos++;
            expect_punct(p, ":");
            n->defbody = new_node(ND_BLOCK);
            {
                int bcap = 8;
                n->defbody->stmts = xmalloc((size_t)bcap * sizeof(Node *));
                while (!at_kw(p, "case") && !at_kw(p, "default") &&
                       !at_punct(p, "}") && peek(p, 0)->kind != TK_EOF) {
                    if (n->defbody->nstmts == bcap) {
                        bcap *= 2;
                        n->defbody->stmts = xrealloc(n->defbody->stmts, (size_t)bcap * sizeof(Node *));
                    }
                    n->defbody->stmts[n->defbody->nstmts++] = parse_stmt(p);
                }
            }
        } else {
            error_at(peek(p, 0)->line, peek(p, 0)->col,
                     "expected 'case' or 'default' inside switch");
        }
    }
    expect_punct(p, "}");
    nodeline(n, t->line, t->col);
    return n;
}

static Node *parse_if(Parser *p) {
    Token *t = next(p);
    Node *n = new_node(ND_IF);
    expect_punct(p, "(");
    n->cond = parse_expr(p);
    expect_punct(p, ")");
    n->then = parse_stmt(p);
    if (at_kw(p, "else")) { p->pos++; n->els = parse_stmt(p); }
    nodeline(n, t->line, t->col);
    return n;
}

static Node *parse_while(Parser *p) {
    Token *t = next(p);
    Node *n = new_node(ND_WHILE);
    expect_punct(p, "(");
    n->cond = parse_expr(p);
    expect_punct(p, ")");
    n->body = parse_stmt(p);
    nodeline(n, t->line, t->col);
    return n;
}

static Node *parse_for(Parser *p) {
    Token *t = next(p);
    Node *n = new_node(ND_FOR);
    expect_punct(p, "(");
    if (at_punct(p, ";")) p->pos++;
    else if (is_type_start(p)) n->init = parse_decl(p);
    else { n->init = new_node(ND_EXPR); n->init->expr = parse_expr(p); expect_punct(p, ";"); }
    if (!at_punct(p, ";")) n->cond = parse_expr(p);
    expect_punct(p, ";");
    if (!at_punct(p, ")")) n->inc = parse_expr(p);
    expect_punct(p, ")");
    n->body = parse_stmt(p);
    nodeline(n, t->line, t->col);
    return n;
}

static Node *parse_return(Parser *p) {
    Token *t = next(p);
    Node *n = new_node(ND_RETURN);
    if (!at_punct(p, ";")) n->expr = parse_expr(p);
    expect_punct(p, ";");
    nodeline(n, t->line, t->col);
    return n;
}

static Node *parse_stmt(Parser *p) {
    Token *t = peek(p, 0);
    Node *n;
    if (t->kind == TK_PUNCT && !strcmp(t->text, "{")) {
        int cap = 8;
        n = new_node(ND_BLOCK);
        p->pos++;
        if (p->scope < 511) {
            p->scope++;
            p->blockStarts[p->scope] = p->cur ? p->cur->nlocals : 0;
        }
        n->stmts = xmalloc((size_t)cap * sizeof(Node *));
        while (!at_punct(p, "}") && t->kind != TK_EOF) {
            if (n->nstmts == cap) { cap *= 2; n->stmts = xrealloc(n->stmts, (size_t)cap * sizeof(Node *)); }
            n->stmts[n->nstmts++] = parse_stmt(p);
            t = peek(p, 0);
        }
        expect_punct(p, "}");
        if (p->scope > 0) p->scope--;
        return n;
    }
    if (t->kind == TK_IDENT) {
        if (is_type_start(p)) {
            /* a function prototype inside a function body:  void f(int *s,int n);
             * parse_function registers it (body == NULL) but resets p->cur,
             * so save/restore the enclosing function around the call */
            if (is_function_decl(p)) {
                Func *saveCur = p->cur;
                int saveScope = p->scope;
                parse_function(p);
                p->cur = saveCur;
                p->scope = saveScope;
                /* empty statement: the prototype itself is not code */
                n = new_node(ND_BLOCK);
                n->stmts = NULL;
                n->nstmts = 0;
                nodeline(n, t->line, t->col);
                return n;
            }
            return parse_decl(p);
        }
        if (!strcmp(t->text, "if")) return parse_if(p);
        if (!strcmp(t->text, "switch")) return parse_switch(p);
        if (!strcmp(t->text, "while")) return parse_while(p);
        if (!strcmp(t->text, "for")) return parse_for(p);
        if (!strcmp(t->text, "return")) return parse_return(p);
        if (!strcmp(t->text, "break")) {
            p->pos++;
            expect_punct(p, ";");
            n = new_node(ND_BREAK);
            nodeline(n, t->line, t->col);
            return n;
        }
        if (!strcmp(t->text, "continue")) {
            p->pos++;
            expect_punct(p, ";");
            n = new_node(ND_CONTINUE);
            nodeline(n, t->line, t->col);
            return n;
        }
    }
    n = new_node(ND_EXPR);
    n->expr = parse_expr(p);
    expect_punct(p, ";");
    nodeline(n, n->expr->line, n->expr->col);
    return n;
}

static Func *parse_function(Parser *p) {
    Type *ret;
    Token *nameTok;
    Func *fn;
    while (at_kw(p, "static") || at_kw(p, "extern") || at_kw(p, "const") ||
           at_kw(p, "volatile") || at_kw(p, "signed"))
        p->pos++;
    ret = parse_type(p);
    nameTok = expect_ident(p);
    {
        int fi = find_func(p, nameTok->text);
        if (fi >= 0 && p->funcs[fi]->body != NULL)
            error_at(nameTok->line, nameTok->col, "duplicate function '%s'", nameTok->text);
    }
    if (!at_punct(p, "("))
        error_at(nameTok->line, nameTok->col, "global variables are not supported yet (found '%s')",
                 peek(p, 0)->text);
    p->pos++;
    fn = xmalloc(sizeof(Func));
    memset(fn, 0, sizeof(Func));
    fn->name = nameTok->text;
    fn->ret = ret;
    p->cur = fn;
    p->scope = 1;               /* parameters live in the function-body scope */
    p->blockStarts[1] = 0;
    /* parameters */    if (!at_punct(p, ")")) {
        if (at_kw(p, "void") && peek(p, 1)->kind == TK_PUNCT && !strcmp(peek(p, 1)->text, ")")) {
            p->pos++;
        } else {
            for (;;) {
                Type *pty;
                Token *pt;
                /* variadic parameter: ... */
                if (at_punct(p, "...")) {
                    p->pos++;
                    fn->variadic = 1;
                    break;
                }
                pty = parse_type(p);
                if (pty->kind == TY_VOID)
                    error_at(peek(p, 0)->line, peek(p, 0)->col, "parameter cannot have type 'void'");
                /* function pointer parameter:  Ret (*name)(params) */
                if (at_punct(p, "(") && peek(p, 1)->kind == TK_PUNCT && !strcmp(peek(p, 1)->text, "*")) {
                    Type **fparams = NULL;
                    int fnp = 0, fcap = 0;
                    p->pos++;   /* '(' */
                    p->pos++;   /* '*' */
                    pt = expect_ident(p);
                    expect_punct(p, ")");
                    expect_punct(p, "(");
                    if (!at_punct(p, ")")) {
                        while (1) {
                            if (fnp == fcap) { fcap = fcap ? fcap * 2 : 4; fparams = xrealloc(fparams, (size_t)fcap * sizeof(Type *)); }
                            fparams[fnp++] = parse_type(p);
                            if (at_punct(p, ",")) { p->pos++; continue; }
                            break;
                        }
                    }
                    expect_punct(p, ")");
                    {
                        Type *ft = new_type(TY_FUNC, 0, 0);
                        ft->ret = pty;
                        ft->params = fparams;
                        ft->nparams = fnp;
                        pty = new_ptr_type(ft);
                    }
                } else {
                    pt = expect_ident(p);
                }
                /* array parameter: name[]  or  name[N]  decays to a pointer */
                if (at_punct(p, "[")) {
                    p->pos++;
                    if (at_punct(p, "]")) p->pos++;
                    else {
                        if (peek(p, 0)->kind != TK_NUM)
                            error_at(peek(p, 0)->line, peek(p, 0)->col,
                                     "array size must be a constant integer");
                        p->pos++;
                        expect_punct(p, "]");
                    }
                    if (pty->kind == TY_PTR)
                        error_at(peek(p, 0)->line, peek(p, 0)->col,
                                 "array of pointers is not supported");
                    pty = new_ptr_type(pty);
                }
                if (find_local(p, pt->text) >= 0)
                    error_at(pt->line, pt->col, "duplicate parameter '%s'", pt->text);
                add_local(fn, pt->text, pty, 1, 1);
                fn->nparams++;
                if (at_punct(p, ",")) { p->pos++; continue; }
                break;
            }
        }
    }
    expect_punct(p, ")");
    /* function prototype (declaration without a body):  Ret name(params);
     * registered into the parser's function list (body == NULL) so calls can
     * be resolved across translation units (multi-file compilation). */
    if (at_punct(p, ";")) {
        p->pos++;
        fn->body = NULL;
        p->cur = NULL;
        p->scope = 0;
        if (p->nfuncs == p->capfuncs) {
            p->capfuncs = p->capfuncs ? p->capfuncs * 2 : 16;
            p->funcs = xrealloc(p->funcs, (size_t)p->capfuncs * sizeof(Func *));
        }
        p->funcs[p->nfuncs++] = fn;
        return fn;
    }
    fn->body = parse_stmt(p);
    if (fn->body->k != ND_BLOCK)
        error_at(nameTok->line, nameTok->col, "function body must be a { ... } block");
    p->cur = NULL;
    p->scope = 0;
    if (p->nfuncs == p->capfuncs) {
        p->capfuncs = p->capfuncs ? p->capfuncs * 2 : 16;
        p->funcs = xrealloc(p->funcs, (size_t)p->capfuncs * sizeof(Func *));
    }
    p->funcs[p->nfuncs++] = fn;
    return fn;
}

/* ============================================================ codegen */

typedef enum { FX_FUNC, FX_LABEL, FX_DATA, FX_IAT } FXT;
typedef struct { int pos, size; FXT t; const char *name; int id; } Fixup;
typedef struct { int id; int off; } Label;

static unsigned char *code;
static int codeLen, codeCap;
static Fixup *fixups;
static int nfixups, capfixups;
static Label *labels;
static int nlabels, caplabels;
static int labelCounter;
static int epiLabel;
static int brkStack[256], contStack[256], loopDepth;
static int switchBrk[256], switchDepth;
static int stackDepth;   /* pending expression pushes on the stack */
static unsigned char *dataBuf;
static int dataLen, dataCap;
static Func **gfuncs;
static int ngfuncs, capgfuncs;
static Local *curLocals;
static Func *curGenFn;   /* function being code-generated */
static int curNLocals;
static int iatEntryOffsets[38];
static int iatBaseOff, iatTotalSize;

#define IMAGE_BASE 0x140000000LL
#define RVA_TEXT 0x1000
static int dataBaseRVA;      /* .data RVA: right after .text, page-aligned */
static int idataBaseRVA;     /* .idata RVA: right after .data, page-aligned */

#define R_RAX 0
#define R_RCX 1
#define R_RDX 2
#define R_RBX 3
#define R_RSP 4
#define R_RBP 5
#define R_RDI 7

static void emit1(int b) {
    if (codeLen == codeCap) {
        codeCap = codeCap ? codeCap * 2 : 4096;
        code = xrealloc(code, (size_t)codeCap);
    }
    code[codeLen++] = (unsigned char)b;
}
/* expression-level stack pushes; the count is tracked so that call sites can
 * keep pending values out of the callee's 32-byte shadow space */
static void push_rax(void) { emit1(0x50); stackDepth++; }
static void pop_rdi(void) { emit1(0x5F); stackDepth--; }
static void pop_rax(void) { emit1(0x58); stackDepth--; }
static void emit(int a, int b) { emit1(a); emit1(b); }
static void emit3(int a, int b, int c) { emit1(a); emit1(b); emit1(c); }
static void emit4(int a, int b, int c, int d) { emit1(a); emit1(b); emit1(c); emit1(d); }
static void emit5(int a, int b, int c, int d, int e) { emit1(a); emit1(b); emit1(c); emit1(d); emit1(e); }
static void emit6(int a, int b, int c, int d, int e, int f) { emit1(a); emit1(b); emit1(c); emit1(d); emit1(e); emit1(f); }
static void emit32(unsigned v) {
    emit1(v & 0xFF); emit1((v >> 8) & 0xFF); emit1((v >> 16) & 0xFF); emit1((v >> 24) & 0xFF);
}
static void emit64(unsigned long long v) {
    emit1((int)(v & 0xFF)); emit1((int)((v >> 8) & 0xFF)); emit1((int)((v >> 16) & 0xFF));
    emit1((int)((v >> 24) & 0xFF)); emit1((int)((v >> 32) & 0xFF)); emit1((int)((v >> 40) & 0xFF));
    emit1((int)((v >> 48) & 0xFF)); emit1((int)((v >> 56) & 0xFF));
}
static void patch32(int pos, unsigned v) {
    code[pos] = v & 0xFF; code[pos + 1] = (v >> 8) & 0xFF;
    code[pos + 2] = (v >> 16) & 0xFF; code[pos + 3] = (v >> 24) & 0xFF;
}
static int modrm(int mod, int reg, int rm) {
    return ((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7);
}

static int new_label(void) { return labelCounter++; }
static void define_label(int id) {
    if (nlabels == caplabels) {
        caplabels = caplabels ? caplabels * 2 : 64;
        labels = xrealloc(labels, (size_t)caplabels * sizeof(Label));
    }
    labels[nlabels].id = id;
    labels[nlabels].off = codeLen;
    nlabels++;
}
static int find_label(int id) {
    int i;
    for (i = 0; i < nlabels; i++)
        if (labels[i].id == id) return labels[i].off;
    return -1;
}

static void add_fixup(FXT t, const char *name, int id, int size) {
    Fixup f;
    if (nfixups == capfixups) {
        capfixups = capfixups ? capfixups * 2 : 128;
        fixups = xrealloc(fixups, (size_t)capfixups * sizeof(Fixup));
    }
    f.pos = codeLen;
    f.size = size;
    f.t = t;
    f.name = name;
    f.id = id;
    fixups[nfixups++] = f;
    emit32(0);
}

static int find_gfunc(const char *name) {
    int i;
    for (i = 0; i < ngfuncs; i++)
        if (!strcmp(gfuncs[i]->name, name)) return i;
    return -1;
}

static const char *IMPORT_FUNCS[] = {
    "printf", "scanf", "puts", "putchar", "getchar", "exit",
    "malloc", "free", "calloc", "rand", "srand", "time",
    "__iob_func", "fopen", "fread", "fwrite", "fclose", "fseek", "ftell",
    "fprintf", "vfprintf", "fputc", "memcpy", "memset", "memcmp",
    "strcmp", "strlen", "strncmp", "strchr", "strrchr", "getenv", "realloc",
    "pow",
    "GetCommandLineA", "ExitProcess", "FindFirstFileA", "FindNextFileA", "FindClose"
};
#define NIMPORTS 38
static int import_index(const char *name) {
    int i;
    for (i = 0; i < NIMPORTS; i++)
        if (!strcmp(IMPORT_FUNCS[i], name)) return i;
    return -1;
}

/* string literal interning into .data */
static int intern_string(const unsigned char *bytes, int blen) {
    int i, off = dataLen;
    for (i = 0; i < dataLen - blen; i++)
        if (!memcmp(dataBuf + i, bytes, (size_t)blen) && dataBuf[i + blen] == 0)
            return i;
    if (dataLen + blen + 1 > dataCap) {
        dataCap = (dataLen + blen + 1) * 2;
        dataBuf = xrealloc(dataBuf, (size_t)dataCap);
    }
    memcpy(dataBuf + dataLen, bytes, (size_t)blen);
    dataLen += blen;
    dataBuf[dataLen++] = 0;
    return off;
}

/* allocate .data space for a global or function-local static variable */
static int add_global(const char *name, Type *ty, Node **inits, int ninits, Node *init, int is_extern) {
    int size, off, i;
    Global *g = find_global(name);
    if (g) {
        /* 'extern' declaration: reuse the existing slot (or the definition
         * that will be seen in another translation unit).  A real definition
         * that already exists is still a redefinition error. */
        if (is_extern) return g->off;
        if (!g->is_extern && (g->init || g->inits || init || inits))
            error_at(0, 0, "redefinition of global '%s'", name);
        /* tentative definition (int x;) merging with an earlier 'extern'
         * declaration: reuse the slot and upgrade it to a definition */
        g->is_extern = 0;
        if (init) g->init = init;
        if (inits) { g->inits = inits; g->ninits = ninits; }
        g->is_bss = (g->init == NULL && g->ninits == 0);
        return g->off;
    }
    if (nglobals == capglobals) {
        capglobals = capglobals ? capglobals * 2 : 32;
        globals = xrealloc(globals, (size_t)capglobals * sizeof(Global));
    }
    size = ty_size(ty);
    if (size == 0) size = 8;
    /* align to 8 */
    while (dataLen % 8) {
        if (dataLen == dataCap) { dataCap = dataCap ? dataCap * 2 : 4096; dataBuf = xrealloc(dataBuf, (size_t)dataCap); }
        dataBuf[dataLen++] = 0;
    }
    off = dataLen;
    for (i = 0; i < size; i++) {
        if (dataLen == dataCap) { dataCap = dataCap ? dataCap * 2 : 4096; dataBuf = xrealloc(dataBuf, (size_t)dataCap); }
        dataBuf[dataLen++] = 0;
    }
    globals[nglobals].name = name;
    globals[nglobals].ty = ty;
    globals[nglobals].off = off;
    globals[nglobals].size = size;
    globals[nglobals].init = init;
    globals[nglobals].inits = inits;
    globals[nglobals].ninits = ninits;
    globals[nglobals].is_bss = (init == NULL && ninits == 0);
    globals[nglobals].is_extern = is_extern;
    globals[nglobals].lcl = xmalloc(sizeof(Local));
    globals[nglobals].lcl->name = name;
    globals[nglobals].lcl->off = off;
    globals[nglobals].lcl->ty = ty;
    globals[nglobals].lcl->is_param = 0;
    globals[nglobals].lcl->is_global = 1;
    globals[nglobals].lcl->is_static = 0;
    nglobals++;
    return off;
}

/* ---- instruction helpers ---- */

static void sub_rsp(int v) {
    if (v <= 127) emit4(0x48, 0x83, 0xEC, v);
    else { emit3(0x48, 0x81, 0xEC); emit32((unsigned)v); }
}
static void add_rsp(int v) {
    if (v <= 127) emit4(0x48, 0x83, 0xC4, v);
    else { emit3(0x48, 0x81, 0xC4); emit32((unsigned)v); }
}
static void call_rel_func(const char *name) { emit1(0xE8); add_fixup(FX_FUNC, name, 0, 5); }
static void jmp_rel_label(int id) { emit1(0xE9); add_fixup(FX_LABEL, NULL, id, 5); }
static void jcc_rel_label(int cc, int id) { emit(0x0F, cc); add_fixup(FX_LABEL, NULL, id, 6); }
static void call_iat(int idx) { emit(0xFF, 0x15); add_fixup(FX_IAT, NULL, idx, 6); }

/* rbp-relative memory access helpers: disp8 when the offset fits, disp32 otherwise */
static void emit_lea_rbp(int reg, int off) {
    if (off <= 127) emit4(0x48, 0x8D, modrm(1, reg, R_RBP), (unsigned char)(-off));
    else { emit3(0x48, 0x8D, modrm(2, reg, R_RBP)); emit32((unsigned)(-off)); }
}
static void emit_mov_load_rbp32(int reg, int off) {
    if (off <= 127) emit3(0x8B, modrm(1, reg, R_RBP), (unsigned char)(-off));
    else { emit(0x8B, modrm(2, reg, R_RBP)); emit32((unsigned)(-off)); }
}
static void emit_mov_store_rbp32(int reg, int off) {
    if (off <= 127) emit3(0x89, modrm(1, reg, R_RBP), (unsigned char)(-off));
    else { emit(0x89, modrm(2, reg, R_RBP)); emit32((unsigned)(-off)); }
}
static void emit_mov_load_rbp64(int reg, int off) {
    if (off <= 127) emit4(0x48, 0x8B, modrm(1, reg, R_RBP), (unsigned char)(-off));
    else { emit3(0x48, 0x8B, modrm(2, reg, R_RBP)); emit32((unsigned)(-off)); }
}
static void emit_mov_store_rbp64(int reg, int off) {
    if (off <= 127) emit4(0x48, 0x89, modrm(1, reg, R_RBP), (unsigned char)(-off));
    else { emit3(0x48, 0x89, modrm(2, reg, R_RBP)); emit32((unsigned)(-off)); }
}
static void emit_movsx_load_rbp8(int reg, int off) {
    if (off <= 127) emit4(0x0F, 0xBE, modrm(1, reg, R_RBP), (unsigned char)(-off));
    else { emit3(0x0F, 0xBE, modrm(2, reg, R_RBP)); emit32((unsigned)(-off)); }
}
static void emit_mov_store_rbp8(int reg, int off) {
    if (off <= 127) emit3(0x88, modrm(1, reg, R_RBP), (unsigned char)(-off));
    else { emit(0x88, modrm(2, reg, R_RBP)); emit32((unsigned)(-off)); }
}

static Member *find_member(Type *st, const char *name) {
    int i;
    for (i = 0; i < st->nmembers; i++)
        if (!strcmp(st->members[i].name, name)) return &st->members[i];
    return NULL;
}

/* ---- typed value helpers ---- */

/* SSE2: load double from [rax] into xmm0 */
static void load_dbl_from_addr(void) {
    emit4(0xF2, 0x0F, 0x10, 0x00);   /* movsd xmm0, [rax] */
}
/* SSE2: store xmm0 to [rdi] */
static void store_dbl_to_addr(void) {
    emit4(0xF2, 0x0F, 0x11, 0x07);   /* movsd [rdi], xmm0 */
}
/* SSE2: load double from [rip+off] into xmm0 */
static void load_dbl_rip(int off) {
    emit4(0xF2, 0x0F, 0x10, 0x05); add_fixup(FX_DATA, NULL, off, 6);  /* movsd xmm0, [rip+off] */
}
/* SSE2: store xmm0 to [rip+off] */
static void store_dbl_rip(int off) {
    emit4(0xF2, 0x0F, 0x11, 0x05); add_fixup(FX_DATA, NULL, off, 6);  /* movsd [rip+off], xmm0 */
}
/* SSE2: load double from [rbp-off] into xmm0 */
static void load_dbl_rbp(int off) {
    if (off <= 127) emit5(0xF2, 0x0F, 0x10, 0x45, (unsigned char)(-off));          /* movsd xmm0, [rbp-d] */
    else { emit4(0xF2, 0x0F, 0x10, 0x85); emit32((unsigned)(-off)); }
}
/* SSE2: store xmm0 to [rbp-off] */
static void store_dbl_rbp(int off) {
    if (off <= 127) emit5(0xF2, 0x0F, 0x11, 0x45, (unsigned char)(-off));          /* movsd [rbp-d], xmm0 */
    else { emit4(0xF2, 0x0F, 0x11, 0x85); emit32((unsigned)(-off)); }
}
/* SSE2: convert signed int/long (in rax/eax, width by t) to double in xmm0 */
static void cvtsi2sd_typed(Type *t) {
    if (ty_is_64(t)) emit5(0xF2, 0x48, 0x0F, 0x2A, 0xC0);   /* cvtsi2sd xmm0, rax */
    else emit4(0xF2, 0x0F, 0x2A, 0xC0);                      /* cvtsi2sd xmm0, eax */
}
/* SSE2: convert double in xmm0 to signed int/long (in rax/eax, width by t) */
static void cvttsd2si_typed(Type *t) {
    if (ty_is_64(t)) emit5(0xF2, 0x48, 0x0F, 0x2C, 0xC0);   /* cvttsd2si rax, xmm0 */
    else emit4(0xF2, 0x0F, 0x2C, 0xC0);                      /* cvttsd2si eax, xmm0 */
}
/* ============ SSE 32-bit (float) helpers ============ */
/* load float from [rax] into xmm0 (low 32 bits) */
static void load_flt_from_addr(void) {
    emit4(0xF3, 0x0F, 0x10, 0x00);   /* movss xmm0, [rax] */
}
/* store xmm0 (low 32 bits) to [rdi] */
static void store_flt_to_addr(void) {
    emit4(0xF3, 0x0F, 0x11, 0x07);   /* movss [rdi], xmm0 */
}
/* load float from [rip+off] into xmm0 */
static void load_flt_rip(int off) {
    emit4(0xF3, 0x0F, 0x10, 0x05); add_fixup(FX_DATA, NULL, off, 6);  /* movss xmm0, [rip+off] */
}
/* store xmm0 to [rip+off] */
static void store_flt_rip(int off) {
    emit4(0xF3, 0x0F, 0x11, 0x05); add_fixup(FX_DATA, NULL, off, 6);  /* movss [rip+off], xmm0 */
}
/* load float from [rbp-off] into xmm0 */
static void load_flt_rbp(int off) {
    if (off <= 127) emit5(0xF3, 0x0F, 0x10, 0x45, (unsigned char)(-off));          /* movss xmm0, [rbp-d] */
    else { emit4(0xF3, 0x0F, 0x10, 0x85); emit32((unsigned)(-off)); }
}
/* store xmm0 to [rbp-off] */
static void store_flt_rbp(int off) {
    if (off <= 127) emit5(0xF3, 0x0F, 0x11, 0x45, (unsigned char)(-off));          /* movss [rbp-d], xmm0 */
    else { emit4(0xF3, 0x0F, 0x11, 0x85); emit32((unsigned)(-off)); }
}
/* convert signed int/long (rax/eax, width by t) to float in xmm0 */
static void cvtsi2ss_typed(Type *t) {
    if (ty_is_64(t)) emit5(0xF3, 0x48, 0x0F, 0x2A, 0xC0);   /* cvtsi2ss xmm0, rax */
    else emit4(0xF3, 0x0F, 0x2A, 0xC0);                      /* cvtsi2ss xmm0, eax */
}
/* convert float in xmm0 to signed int/long (rax/eax, width by t) */
static void cvttss2si_typed(Type *t) {
    if (ty_is_64(t)) emit5(0xF3, 0x48, 0x0F, 0x2C, 0xC0);   /* cvttss2si rax, xmm0 */
    else emit4(0xF3, 0x0F, 0x2C, 0xC0);                      /* cvttss2si eax, xmm0 */
}
/* convert float (xmm0 low 32) to double in xmm0 (F3 0F 5A C0) */
static void cvtss2sd(void) {
    emit4(0xF3, 0x0F, 0x5A, 0xC0);
}
/* convert double (xmm0) to float (xmm0 low 32) (F2 0F 5A C0) */
static void cvtsd2ss(void) {
    emit4(0xF2, 0x0F, 0x5A, 0xC0);
}
/* xorps xmm0, xmm0 (0F 57 C0) — zero a float */
static void xorps_xmm0(void) {
    emit3(0x0F, 0x57, 0xC0);
}
/* float truthiness: value in xmm0 -> int 0/1 in eax */
static void flt_to_bool_eax(void) {
    emit3(0x0F, 0x57, 0xC9);   /* xorps xmm1, xmm1 (0.0f) */
    emit3(0x0F, 0x2E, 0xC8);   /* ucomiss xmm1, xmm0 (0.0f vs f) */
    emit3(0x0F, 0x95, 0xC0);   /* setne al */
    emit3(0x0F, 0xB6, 0xC0);   /* movzx eax, al */
}
/* truthiness conversion for a value of type t currently in xmm0 (float/double) */
static void dbl_to_bool_eax(void);
static void gen_truth(Type *t) {
    if (!t) return;
    if (t->kind == TY_FLOAT) flt_to_bool_eax();
    else if (t->kind == TY_DOUBLE) dbl_to_bool_eax();
}
/* SSE2: xorpd xmm0, xmm0  (66 0F 57 C0) — zero a double */
static void xorpd_xmm0(void) {
    emit4(0x66, 0x0F, 0x57, 0xC0);
}
/* SSE2: convert double truthiness: value in xmm0 -> int 0/1 in eax */
static void dbl_to_bool_eax(void) {
    emit4(0x66, 0x0F, 0x57, 0xC9);   /* xorpd xmm1, xmm1 (0.0) */
    emit4(0x66, 0x0F, 0x2E, 0xC8);   /* ucomisd xmm1, xmm0  (0.0 vs d) */
    emit3(0x0F, 0x95, 0xC0);         /* setne al */
    emit3(0x0F, 0xB6, 0xC0);         /* movzx eax, al */
}
/* convert the value just produced by gen_expr(e) into type want
 * (float/double/int implicit conversions; other kinds are no-ops) */
static void gen_conv(Type *want, Type *got) {
    if (!want || !got) return;
    if (want->kind == TY_FLOAT) {
        if (got->kind == TY_DOUBLE) cvtsd2ss();
        else if (got->kind == TY_INT || got->kind == TY_CHAR ||
                 got->kind == TY_LONG || got->kind == TY_ULONG) cvtsi2ss_typed(got);
        /* float->float: no-op */
    } else if (want->kind == TY_DOUBLE) {
        if (got->kind == TY_FLOAT) cvtss2sd();
        else if (got->kind != TY_DOUBLE) cvtsi2sd_typed(got);
    } else {
        /* int/long/char targets */
        if (got->kind == TY_FLOAT) cvttss2si_typed(want);
        else if (got->kind == TY_DOUBLE) cvttsd2si_typed(want);
    }
}

static void load_from_addr(Type *t) {   /* value at [rax] -> rax/eax (or xmm0) by type */
    char k = type_kind(t);
    if (k == 'f') { load_flt_from_addr(); return; }
    if (k == 'd') { load_dbl_from_addr(); return; }
    if (k == 'p' || k == 'l' || k == 'u') emit3(0x48, 0x8B, 0x00);   /* mov rax, [rax] */
    else if (k == 'c') emit3(0x0F, 0xBE, 0x00);                      /* movsx eax, byte [rax] */
    else emit(0x8B, 0x00);                                           /* mov eax, [rax] */
}
static void store_to_addr(Type *t) {    /* value in rax/eax (or xmm0) -> [rdi] by type */
    char k = type_kind(t);
    if (k == 'f') { store_flt_to_addr(); return; }
    if (k == 'd') { store_dbl_to_addr(); return; }
    if (k == 'p' || k == 'l' || k == 'u') emit3(0x48, 0x89, 0x07);   /* mov [rdi], rax */
    else if (k == 'c') emit(0x88, 0x07);                             /* mov [rdi], al */
    else emit(0x89, 0x07);                                           /* mov [rdi], eax */
}
static void load_rip(Type *t, int off) {   /* variable in .data -> rax/eax (or xmm0) */
    char k = type_kind(t);
    if (k == 'f') { load_flt_rip(off); return; }
    if (k == 'd') { load_dbl_rip(off); return; }
    if (k == 'p' || k == 'l' || k == 'u') { emit3(0x48, 0x8B, 0x05); add_fixup(FX_DATA, NULL, off, 6); }
    else if (k == 'c') { emit3(0x0F, 0xBE, 0x05); add_fixup(FX_DATA, NULL, off, 6); }
    else { emit(0x8B, 0x05); add_fixup(FX_DATA, NULL, off, 5); }
}
static void store_rip(Type *t, int off) {  /* rax/eax (or xmm0) -> variable in .data */
    char k = type_kind(t);
    if (k == 'f') { store_flt_rip(off); return; }
    if (k == 'd') { store_dbl_rip(off); return; }
    if (k == 'p' || k == 'l' || k == 'u') { emit3(0x48, 0x89, 0x05); add_fixup(FX_DATA, NULL, off, 6); }
    else if (k == 'c') { emit(0x88, 0x05); add_fixup(FX_DATA, NULL, off, 5); }
    else { emit(0x89, 0x05); add_fixup(FX_DATA, NULL, off, 5); }
}

static void load_local(Local *l) {
    char k = type_kind(l->ty);
    if (k == 's') {                      /* struct value: yield its address */
        if (l->is_global || l->is_static) { emit3(0x48, 0x8D, 0x05); add_fixup(FX_DATA, NULL, l->off, 7); }
        else emit_lea_rbp(R_RAX, l->off);
        return;
    }
    if (l->is_global || l->is_static) { load_rip(l->ty, l->off); return; }
    if (k == 'f') { load_flt_rbp(l->off); return; }
    if (k == 'd') { load_dbl_rbp(l->off); return; }
    if (k == 'p' || k == 'l' || k == 'u') emit_mov_load_rbp64(R_RAX, l->off);
    else if (k == 'c') emit_movsx_load_rbp8(R_RAX, l->off);
    else emit_mov_load_rbp32(R_RAX, l->off);
}
static void store_local(Local *l) {
    char k = type_kind(l->ty);
    if (l->is_global || l->is_static) { store_rip(l->ty, l->off); return; }
    if (k == 'f') { store_flt_rbp(l->off); return; }
    if (k == 'd') { store_dbl_rbp(l->off); return; }
    if (k == 'p' || k == 'l' || k == 'u') emit_mov_store_rbp64(R_RAX, l->off);
    else if (k == 'c') emit_mov_store_rbp8(R_RAX, l->off);
    else emit_mov_store_rbp32(R_RAX, l->off);
}

static Local *resolve_var(Node *node) {
    /* use the local index bound during parsing (block-scope correct) */
    if (node->li >= 0 && node->li < curNLocals)
        return &curLocals[node->li];
    {
        Global *g = find_global(node->name);
        if (g) return g->lcl;
    }
    if (find_gfunc(node->name) >= 0)
        error_at(node->line, node->col,
                 "'%s' is a function and cannot be used as a value (did you mean to call it?)",
                 node->name);
    error_at(node->line, node->col, "undeclared variable '%s'", node->name);
    return NULL;
}

static Type *node_type(Node *n);

/* type of an lvalue (variable, array element, struct member, deref) */
static Type *lvalue_type(Node *node) {
    if (node->k == ND_VAR) {
        Local *l = resolve_var(node);
        if (l->ty->kind == TY_ARRAY && !l->is_param)
            error_at(node->line, node->col, "cannot assign to an array");
        return l->ty;
    }
    if (node->k == ND_INDEX) {
        Type *bt = node_type(node->base);
        if (!ty_is_ptr(bt))
            error_at(node->line, node->col, "cannot subscript a non-array value");
        return bt->base;
    }
    if (node->k == ND_MEMBER) {
        Type *bt = node_type(node->member);
        if (node->arrow) {
            if (bt->kind != TY_PTR)
                error_at(node->line, node->col, "'->' used on a non-pointer value");
            bt = bt->base;
        }
        if (bt->kind != TY_STRUCT)
            error_at(node->line, node->col, "'%s' is not a struct", node->mname);
        if (!find_member(bt, node->mname))
            error_at(node->line, node->col, "no member named '%s'", node->mname);
        return find_member(bt, node->mname)->ty;
    }
    if (node->k == ND_DEREF) {
        Type *bt = node_type(node->o);
        if (!ty_is_ptr(bt))
            error_at(node->line, node->col, "cannot dereference a non-pointer value");
        return bt->base;
    }
    error_at(node->line, node->col,
             "invalid lvalue (NGCC supports variables, array elements, *pointer and struct members)");
    return NULL;
}

static void gen_expr(Node *node);
static void gen_stmt(Node *node);
static void gen_array_init_flat(Type *t, int absOff, int isData, Node **inits, int ninits, int *pi);

static void scale_rax_by(int es) { if (es == 2) { emit3(0x48, 0xC1, 0xE0); emit1(1); }
    else if (es == 4) { emit3(0x48, 0xC1, 0xE0); emit1(2); }
    else if (es == 8) { emit3(0x48, 0xC1, 0xE0); emit1(3); }
    else if (es == 16) { emit3(0x48, 0xC1, 0xE0); emit1(4); }
    else if (es == 32) { emit3(0x48, 0xC1, 0xE0); emit1(5); }
    else if (es == 64) { emit3(0x48, 0xC1, 0xE0); emit1(6); }
    else { emit3(0x48, 0x69, 0xC0); emit32((unsigned)es); } }  /* imul rax, rax, es */
static void scale_rdi_by(int es) { if (es == 2) { emit3(0x48, 0xC1, 0xE7); emit1(1); }
    else if (es == 4) { emit3(0x48, 0xC1, 0xE7); emit1(2); }
    else if (es == 8) { emit3(0x48, 0xC1, 0xE7); emit1(3); }
    else if (es == 16) { emit3(0x48, 0xC1, 0xE7); emit1(4); }
    else if (es == 32) { emit3(0x48, 0xC1, 0xE7); emit1(5); }
    else if (es == 64) { emit3(0x48, 0xC1, 0xE7); emit1(6); }
    else { emit3(0x48, 0x69, 0xFF); emit32((unsigned)es); } }  /* imul rdi, rdi, es */
static void add_rax_imm(int v) {   /* add rax, imm */
    if (v <= 127) emit4(0x48, 0x83, 0xC0, v);
    else { emit3(0x48, 0x81, 0xC0); emit32((unsigned)v); }
}

/* store value in rax/eax (or xmm0) to [rdi+disp] by type */
static void store_to_addr_disp(Type *t, int disp) {
    char k = type_kind(t);
    if (k == 'd') {
        if (disp <= 127) emit5(0xF2, 0x0F, 0x11, modrm(1, 0, 7), disp);  /* movsd [rdi+d], xmm0 */
        else { emit4(0xF2, 0x0F, 0x11, modrm(2, 0, 7)); emit32((unsigned)disp); }
        return;
    }
    if (disp <= 127) {
        if (k == 'p' || k == 'l' || k == 'u') emit4(0x48, 0x89, modrm(1, 0, 7), disp);  /* mov [rdi+d], rax */
        else if (k == 'c') emit3(0x88, modrm(1, 0, 7), disp);                            /* mov [rdi+d], al */
        else emit3(0x89, modrm(1, 0, 7), disp);                                          /* mov [rdi+d], eax */
    } else {
        if (k == 'p' || k == 'l' || k == 'u') { emit3(0x48, 0x89, modrm(2, 0, 7)); emit32((unsigned)disp); }
        else if (k == 'c') { emit(0x88, modrm(2, 0, 7)); emit32((unsigned)disp); }
        else { emit(0x89, modrm(2, 0, 7)); emit32((unsigned)disp); }
    }
}

/* lea rdi, [rsp + disp] */
static void lea_rdi_rsp(int disp) {
    if (disp <= 127) emit5(0x48, 0x8D, 0x7C, 0x24, disp);          /* lea rdi, [rsp+d] */
    else { emit5(0x48, 0x8D, 0xBC, 0x24, 0); emit32((unsigned)disp); }
}

/* copy 'size' bytes from the address in rax to the address in rdi */
static void copy_mem(int size) {
    int n8 = size / 8, rem = size % 8, k;
    emit1(0x56);                    /* push rsi (callee-saved) */
    emit3(0x48, 0x89, 0xC6);        /* mov rsi, rax */
    for (k = 0; k < n8; k++) {
        if (k * 8 <= 127) {
            emit4(0x48, 0x8B, 0x46, k * 8);        /* mov rax, [rsi+8k] */
            emit4(0x48, 0x89, 0x47, k * 8);        /* mov [rdi+8k], rax */
        } else {
            emit3(0x48, 0x8B, 0x86); emit32((unsigned)(k * 8));
            emit3(0x48, 0x89, 0x87); emit32((unsigned)(k * 8));
        }
    }
    if (rem == 4) {
        emit3(0x8B, 0x46, n8 * 8);                  /* mov eax, [rsi+8n] */
        emit3(0x89, 0x47, n8 * 8);                  /* mov [rdi+8n], eax */
    } else {
        for (k = 0; k < rem; k++) {
            emit4(0x0F, 0xB6, 0x46, n8 * 8 + k);    /* movzx eax, byte [rsi+..] */
            emit3(0x88, 0x47, n8 * 8 + k);          /* mov [rdi+..], al */
        }
    }
    emit1(0x5E);                    /* pop rsi */
}

/* base address of an array/pointer expression (used by subscripting) */
static void gen_pointer_base(Node *node) {
    if (node->k == ND_VAR) {
        Local *l = resolve_var(node);
        if (l->ty->kind == TY_ARRAY || l->ty->kind == TY_PTR || l->ty->kind == TY_FUNC) {
            if (l->ty->kind == TY_ARRAY && !l->is_param && !l->is_global && !l->is_static)
                emit_lea_rbp(R_RAX, l->off);                          /* local array data */
            else if (l->ty->kind == TY_ARRAY)
                { emit3(0x48, 0x8D, 0x05); add_fixup(FX_DATA, NULL, l->off, 7); }  /* global array */
            else if (l->is_global || l->is_static)
                load_rip(l->ty, l->off);                              /* global pointer */
            else
                emit_mov_load_rbp64(R_RAX, l->off);                   /* pointer value in slot */
            return;
        }
    }
    /* any other expression whose value is a pointer: ops[0][0], (p+1)[i], fnptr[i] */
    if (node->k != ND_VAR && ty_is_ptr(node_type(node))) {
        gen_expr(node);
        return;
    }
    error_at(node->line, node->col, "invalid array/pointer expression");
}

/* address of the storage of an lvalue */
static void gen_addr(Node *node) {
    if (node->k == ND_VAR) {
        Local *l = resolve_var(node);
        if (l->ty->kind == TY_ARRAY)
            error_at(node->line, node->col, "cannot take the address of an array (use an index)");
        if (l->is_global || l->is_static) { emit3(0x48, 0x8D, 0x05); add_fixup(FX_DATA, NULL, l->off, 7); }
        else emit_lea_rbp(R_RAX, l->off);
        return;
    }
    if (node->k == ND_INDEX) {
        gen_expr(node->idx);            /* index in eax */
        push_rax();
        gen_pointer_base(node->base);   /* base address in rax */
        pop_rdi();                      /* index */
        emit3(0x48, 0x63, 0xFF);        /* movsxd rdi, edi */
        scale_rdi_by(ty_elem_size(node_type(node->base)));
        emit3(0x48, 0x01, 0xF8);        /* add rax, rdi */
        return;
    }
    if (node->k == ND_MEMBER) {
        Type *bt = node_type(node->member);
        int off;
        if (node->arrow) {
            if (bt->kind != TY_PTR)
                error_at(node->line, node->col, "'->' used on a non-pointer value");
            gen_expr(node->member);     /* pointer value in rax */
            bt = bt->base;
        } else {
            gen_addr(node->member);     /* struct address in rax */
        }
        if (bt->kind != TY_STRUCT)
            error_at(node->line, node->col, "'%s' is not a struct", node->mname);
        {
            Member *m = find_member(bt, node->mname);
            if (!m) error_at(node->line, node->col, "no member named '%s'", node->mname);
            off = m->offset;
        }
        if (off) add_rax_imm(off);
        return;
    }
    if (node->k == ND_DEREF) {
        lvalue_type(node);              /* validate it is a pointer */
        gen_expr(node->o);              /* the pointer value in rax */
        return;
    }
    error_at(node->line, node->col, "invalid lvalue");
}

/* type of an expression node (used by codegen) */
static Type *node_type(Node *n) {
    switch (n->k) {
    case ND_NUM: return n->tp ? n->tp : &ty_int;
    case ND_STR: return new_ptr_type(&ty_char);
    case ND_VAR: {
        int gi;
        if (!strcmp(n->name, "stderr") || !strcmp(n->name, "stdout") || !strcmp(n->name, "stdin"))
            return new_ptr_type(&ty_opaque);
        gi = find_gfunc(n->name);
        if (gi >= 0) {
            Type *ft = new_type(TY_FUNC, 0, 0);
            ft->ret = gfuncs[gi]->ret;
            return new_ptr_type(ft);
        }
        return resolve_var(n)->ty;
    }
    case ND_INDEX: {
        Type *bt = node_type(n->base);
        return ty_is_ptr(bt) ? bt->base : &ty_int;
    }
    case ND_MEMBER: return lvalue_type(n);
    case ND_DEREF: {
        Type *bt = node_type(n->o);
        return ty_is_ptr(bt) ? bt->base : &ty_int;
    }
    case ND_ADDR: return new_ptr_type(node_type(n->o));
    case ND_CAST: return n->tp;
    case ND_COMPLIT: return n->tp;
    case ND_ASSIGN: return node_type(n->l);
    case ND_COMMA: return node_type(n->r);
    case ND_COND: {
        Type *a = node_type(n->then), *b = node_type(n->els);
        if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) return &ty_double;
        if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) return &ty_float;
        if (ty_is_64(a) || ty_is_64(b) || ty_is_ptr(a) || ty_is_ptr(b)) return &ty_long;
        return &ty_int;
    }
    case ND_NEG: case ND_NOT: case ND_BITNOT:
    case ND_PREINC: case ND_PREDEC: case ND_POSTINC: case ND_POSTDEC:
        return node_type(n->o);
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_AND: case ND_OR: case ND_XOR: case ND_SHL: case ND_SHR: {
        Type *a = node_type(n->l), *b = node_type(n->r);
        if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) return &ty_double;
        if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) return &ty_float;
        if (ty_is_64(a) || ty_is_64(b) || ty_is_ptr(a) || ty_is_ptr(b)) return &ty_long;
        return &ty_int;
    }
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
    case ND_LAND: case ND_LOR:
        return &ty_int;
    case ND_CALL: {
        if (n->fn->k == ND_VAR) {
            int gi = find_gfunc(n->fn->name);
            if (gi >= 0) return gfuncs[gi]->ret;
            if (!strcmp(n->fn->name, "malloc") || !strcmp(n->fn->name, "calloc") ||
                !strcmp(n->fn->name, "memcpy") || !strcmp(n->fn->name, "memset"))
                return new_ptr_type(&ty_void);
            if (!strcmp(n->fn->name, "fopen")) return new_ptr_type(&ty_opaque);
            if (!strcmp(n->fn->name, "getenv") || !strcmp(n->fn->name, "strchr") ||
                !strcmp(n->fn->name, "strrchr"))
                return new_ptr_type(&ty_char);
            if (!strcmp(n->fn->name, "ftell")) return &ty_long;
            if (!strcmp(n->fn->name, "pow")) return &ty_double;
            return &ty_int;
        }
        {
            Type *ft = node_type(n->fn);
            if (ft->kind == TY_PTR && ft->base && ft->base->kind == TY_FUNC)
                return ft->base->ret;
            return &ty_int;
        }
    }
    default: return &ty_int;
    }
}

static void gen_cmp(int cc, int is64) {
    if (is64) emit3(0x48, 0x3B, 0xF8); else emit(0x3B, 0xF8);  /* cmp rdi, rax (left-right) */
    emit3(0x0F, cc, 0xC0);
    emit3(0x0F, 0xB6, 0xC0);
}

static void gen_binary(Node *node) {
    int is64, uns;
    Type *lt = node_type(node->l), *rt = node_type(node->r);
    if (lt->kind == TY_DOUBLE || rt->kind == TY_DOUBLE) {
        /* double arithmetic: left operand saved on the stack (a call in the
         * right operand would clobber XMM1), then restored into xmm1;
         * the right operand ends up in xmm0. */
        int cc;
        gen_expr(node->l);
        if (lt->kind == TY_FLOAT) cvtss2sd();
        else if (lt->kind != TY_DOUBLE) cvtsi2sd_typed(lt);
        emit5(0x66, 0x48, 0x0F, 0x7E, 0xC0);   /* movq rax, xmm0 */
        push_rax();
        gen_expr(node->r);
        if (rt->kind == TY_FLOAT) cvtss2sd();
        else if (rt->kind != TY_DOUBLE) cvtsi2sd_typed(rt);
        pop_rdi();                             /* restore left bits into rdi */
        emit5(0x66, 0x48, 0x0F, 0x6E, 0xCF);   /* movq xmm1, rdi */
        switch (node->k) {
        case ND_ADD: emit4(0xF2, 0x0F, 0x58, 0xC1); break;  /* addsd xmm0, xmm1 */
        case ND_MUL: emit4(0xF2, 0x0F, 0x59, 0xC1); break;  /* mulsd xmm0, xmm1 */
        case ND_SUB:
            /* result = left - right; left in xmm1, right in xmm0 */
            emit4(0xF2, 0x0F, 0x10, 0xD0);   /* movsd xmm2, xmm0 (save right) */
            emit4(0xF2, 0x0F, 0x10, 0xC1);   /* movsd xmm0, xmm1 (left) */
            emit4(0xF2, 0x0F, 0x5C, 0xC2);   /* subsd xmm0, xmm2 */
            break;
        case ND_DIV:
            emit4(0xF2, 0x0F, 0x10, 0xD0);   /* movsd xmm2, xmm0 (save right) */
            emit4(0xF2, 0x0F, 0x10, 0xC1);   /* movsd xmm0, xmm1 (left) */
            emit4(0xF2, 0x0F, 0x5E, 0xC2);   /* divsd xmm0, xmm2 */
            break;
        case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
            emit4(0x66, 0x0F, 0x2E, 0xC8);   /* ucomisd xmm1, xmm0 (left vs right) */
            if (node->k == ND_EQ) cc = 0x94;          /* sete */
            else if (node->k == ND_NE) cc = 0x95;     /* setne */
            else if (node->k == ND_LT) cc = 0x92;     /* setb  (CF) */
            else if (node->k == ND_LE) cc = 0x96;     /* setbe (CF or ZF) */
            else if (node->k == ND_GT) cc = 0x97;     /* seta  (CF=0 & ZF=0) */
            else cc = 0x93;                           /* setae (CF=0) */
            emit3(0x0F, cc, 0xC0);
            emit3(0x0F, 0xB6, 0xC0);                  /* movzx eax, al */
            break;
        default:
            error_at(node->line, node->col, "invalid double binary operator");
        }
        return;
    }
    if (lt->kind == TY_FLOAT || rt->kind == TY_FLOAT) {
        /* float arithmetic (32-bit SSE): left in xmm1, right in xmm0 */
        int cc;
        gen_expr(node->l);
        if (lt->kind != TY_FLOAT) cvtsi2ss_typed(lt);
        emit5(0x66, 0x48, 0x0F, 0x7E, 0xC0);   /* movq rax, xmm0 (save left bits) */
        push_rax();
        gen_expr(node->r);
        if (rt->kind != TY_FLOAT) cvtsi2ss_typed(rt);
        pop_rdi();
        emit5(0x66, 0x48, 0x0F, 0x6E, 0xCF);   /* movq xmm1, rdi */
        switch (node->k) {
        case ND_ADD: emit4(0xF3, 0x0F, 0x58, 0xC1); break;  /* addss xmm0, xmm1 */
        case ND_MUL: emit4(0xF3, 0x0F, 0x59, 0xC1); break;  /* mulss xmm0, xmm1 */
        case ND_SUB:
            emit4(0xF3, 0x0F, 0x10, 0xD0);   /* movss xmm2, xmm0 */
            emit4(0xF3, 0x0F, 0x10, 0xC1);   /* movss xmm0, xmm1 */
            emit4(0xF3, 0x0F, 0x5C, 0xC2);   /* subss xmm0, xmm2 */
            break;
        case ND_DIV:
            emit4(0xF3, 0x0F, 0x10, 0xD0);   /* movss xmm2, xmm0 */
            emit4(0xF3, 0x0F, 0x10, 0xC1);   /* movss xmm0, xmm1 */
            emit4(0xF3, 0x0F, 0x5E, 0xC2);   /* divss xmm0, xmm2 */
            break;
        case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
            emit3(0x0F, 0x2E, 0xC8);   /* ucomiss xmm1, xmm0 (left vs right) */
            if (node->k == ND_EQ) cc = 0x94;
            else if (node->k == ND_NE) cc = 0x95;
            else if (node->k == ND_LT) cc = 0x92;
            else if (node->k == ND_LE) cc = 0x96;
            else if (node->k == ND_GT) cc = 0x97;
            else cc = 0x93;
            emit3(0x0F, cc, 0xC0);
            emit3(0x0F, 0xB6, 0xC0);
            break;
        default:
            error_at(node->line, node->col, "invalid float binary operator");
        }
        return;
    }
    gen_expr(node->l);
    push_rax();
    gen_expr(node->r);
    pop_rdi();
    is64 = ty_is_64(lt) || ty_is_64(rt) || ty_is_ptr(lt) || ty_is_ptr(rt);
    uns = (lt && lt->is_unsigned) || (rt && rt->is_unsigned) ||
          ty_is_ptr(lt) || ty_is_ptr(rt);
    switch (node->k) {
    case ND_ADD:
        if (ty_is_ptr(lt) && !ty_is_ptr(rt)) {
            emit3(0x48, 0x63, 0xC0);          /* movsxd rax, eax */
            scale_rax_by(ty_elem_size(lt));
            emit3(0x48, 0x01, 0xF8);           /* add rax, rdi */
        } else if (ty_is_ptr(rt) && !ty_is_ptr(lt)) {
            emit3(0x48, 0x63, 0xFF);          /* movsxd rdi, edi */
            scale_rdi_by(ty_elem_size(rt));
            emit3(0x48, 0x01, 0xF8);
        } else if (is64) emit3(0x48, 0x01, 0xF8);
        else emit(0x01, 0xF8);
        break;
    case ND_SUB:
        if (ty_is_ptr(lt) && !ty_is_ptr(rt)) {
            emit3(0x48, 0x63, 0xC0);
            scale_rax_by(ty_elem_size(lt));
            emit3(0x48, 0x29, 0xC7);           /* sub rdi, rax */
            emit3(0x48, 0x89, 0xF8);
        } else if (ty_is_ptr(lt) && ty_is_ptr(rt)) {
            emit3(0x48, 0x29, 0xC7);           /* sub rdi, rax (p - q) */
            emit3(0x48, 0x89, 0xF8);           /* mov rax, rdi */
            emit(0x48, 0x99);                 /* cqo */
            emit3(0x48, 0xC7, 0xC3); emit32((unsigned)ty_elem_size(lt));  /* mov rbx, elem */
            emit3(0x48, 0xF7, 0xFB);           /* idiv rbx */
            break;
        } else if (is64) { emit3(0x48, 0x29, 0xC7); emit3(0x48, 0x89, 0xF8); }
        else { emit(0x29, 0xC7); emit(0x89, 0xF8); }
        break;
    case ND_MUL:
        if (is64) emit4(0x48, 0x0F, 0xAF, 0xC7); else emit3(0x0F, 0xAF, 0xC7);
        break;
    case ND_DIV:
    case ND_MOD:
        if (is64) {
            emit3(0x48, 0x89, 0xC3);           /* mov rbx, rax (right) */
            emit3(0x48, 0x89, 0xF8);           /* mov rax, rdi (left) */
            if (uns) { emit3(0x48, 0x31, 0xD2); emit3(0x48, 0xF7, 0xF3); }  /* xor edx,edx; div rbx */
            else { emit(0x48, 0x99); emit3(0x48, 0xF7, 0xFB); }            /* cqo; idiv rbx */
            if (node->k == ND_MOD) emit3(0x48, 0x89, 0xD0);   /* mov rax, rdx */
        } else {
            emit(0x89, 0xC3);
            emit(0x89, 0xF8);
            if (uns) { emit(0x31, 0xD2); emit(0xF7, 0xF3); }
            else { emit1(0x99); emit(0xF7, 0xFB); }
            if (node->k == ND_MOD) emit(0x89, 0xD0);
        }
        break;
    case ND_AND: if (is64) emit3(0x48, 0x21, 0xF8); else emit(0x21, 0xF8); break;
    case ND_OR:  if (is64) emit3(0x48, 0x09, 0xF8); else emit(0x09, 0xF8); break;
    case ND_XOR: if (is64) emit3(0x48, 0x31, 0xF8); else emit(0x31, 0xF8); break;
    case ND_SHL:
    case ND_SHR:
        emit(0x89, 0xC1);                     /* mov ecx, eax (count) */
        if (is64) {
            if (node->k == ND_SHL) emit3(0x48, 0xD3, 0xE7);
            else if (uns) emit3(0x48, 0xD3, 0xEF);
            else emit3(0x48, 0xD3, 0xFF);
            emit3(0x48, 0x89, 0xF8);
        } else {
            if (node->k == ND_SHL) emit(0xD3, 0xE7);
            else if (uns) emit(0xD3, 0xEF);
            else emit(0xD3, 0xFF);
            emit(0x89, 0xF8);
        }
        break;
    case ND_EQ: gen_cmp(0x94, is64); break;
    case ND_NE: gen_cmp(0x95, is64); break;
    case ND_LT: gen_cmp(uns ? 0x92 : 0x9C, is64); break;
    case ND_LE: gen_cmp(uns ? 0x96 : 0x9E, is64); break;
    case ND_GT: gen_cmp(uns ? 0x97 : 0x9F, is64); break;
    case ND_GE: gen_cmp(uns ? 0x93 : 0x9D, is64); break;
    default:
        error_at(node->line, node->col, "internal error: unknown binary operator");
    }
}

static void gen_logical(Node *node, int is_and) {
    int lSkip = new_label(), lEnd = new_label();
    gen_expr(node->l);
    gen_truth(node_type(node->l));
    emit(0x85, 0xC0);                    /* test eax, eax */
    jcc_rel_label(is_and ? 0x84 : 0x85, lSkip);  /* jz / jnz */
    gen_expr(node->r);
    gen_truth(node_type(node->r));
    emit(0x85, 0xC0);
    jcc_rel_label(is_and ? 0x84 : 0x85, lSkip);
    emit1(0xB8); emit32((unsigned)(is_and ? 1 : 0));   /* mov eax, 1/0 */
    jmp_rel_label(lEnd);
    define_label(lSkip);
    emit1(0xB8); emit32((unsigned)(is_and ? 0 : 1));
    define_label(lEnd);
}

static void gen_call(Node *node) {
    const char *name;
    int n, i, S;
    int indirect = 0;
    if (node->fn->k != ND_VAR)
        indirect = 1;
    name = node->fn->k == ND_VAR ? node->fn->name : NULL;
    /* builtins: va_start / va_end */
    if (!strcmp(name, "va_start")) {
        Local *ap;
        int np;
        if (node->nargs != 2 || node->args[0]->k != ND_VAR)
            error_at(node->line, node->col, "va_start expects (va_list, last_named)");
        ap = resolve_var(node->args[0]);
        np = curGenFn ? curGenFn->nparams : 0;
        /* the first variadic argument: for nparams<=3 it is r9 (saved at
         * [rbp+40]); for nparams==4 it is the first stack argument at
         * [rbp+56].  vfprintf reads a contiguous array from va_list, so we
         * copy r9 into the gap at [rbp+48] first. */
        if (np <= 3) {
            emit4(0x48, 0x8B, 0x45, 0x28);   /* mov rax, [rbp+40]  (r9) */
            emit4(0x48, 0x89, 0x45, 0x30);   /* mov [rbp+48], rax */
            emit4(0x48, 0x8D, 0x45, 0x30);   /* lea rax, [rbp+48] */
        } else {
            emit4(0x48, 0x8D, 0x45, 0x38);   /* lea rax, [rbp+56] */
        }
        store_local(ap);                     /* ap = rax */
        return;
    }
    if (!strcmp(name, "va_end")) {
        return;   /* no-op */
    }
    n = node->nargs;
    {
        /* variadic callees (user '...' functions and printf-family imports)
         * may receive more than 4 arguments; the extras go on the stack */
        int variadic = 0;
        int gi = name ? find_gfunc(name) : -1;
        if (gi >= 0) variadic = gfuncs[gi]->variadic;
        else if (name && (!strcmp(name, "printf") || !strcmp(name, "fprintf") ||
                          !strcmp(name, "vfprintf")))
            variadic = 1;
    }
    {
        /* classify arguments: register args (first 4 scalars/pointers),
         * stack scalar args (5th+ for variadic), struct args.
         * regXmm[i]: 0 = integer reg, 1 = double (64-bit XMM), 2 = float (32-bit XMM) */
        int nreg = 0, extraStack = 0, nstruct = 0, nvec = 0;
        int regXmm[4] = { 0, 0, 0, 0 };
        Type *ptypes[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
        int stkOff[8], stkTotal = 0;
        int ri = 0, si = 0, ei = 0;
        int gi = name ? find_gfunc(name) : -1;
        int calleeVariadic = 0;
        if (gi >= 0) calleeVariadic = gfuncs[gi]->variadic;
        else if (name && (!strcmp(name, "printf") || !strcmp(name, "fprintf") ||
                          !strcmp(name, "vfprintf") || !strcmp(name, "scanf") ||
                          !strcmp(name, "fscanf")))
            calleeVariadic = 1;
        for (i = 0; i < n; i++) {
            Type *at = node_type(node->args[i]);
            Type *pt = NULL;
            if (gi >= 0 && i < gfuncs[gi]->nparams) pt = gfuncs[gi]->locals[i].ty;
            else if (name && !strcmp(name, "pow")) pt = &ty_double;
            ptypes[i] = pt;
            if (at && at->kind == TY_STRUCT && at->size > 8) {
                stkOff[nstruct++] = stkTotal;
                stkTotal += (at->size + 7) & ~7;
            } else if (nreg < 4) {
                int eff = pt ? pt->kind : at->kind;
                /* variadic callees promote float to double (C default argument
                 * promotion); non-variadic float parameters pass 32-bit */
                if (eff == TY_DOUBLE) { regXmm[nreg] = 1; nvec++; }
                else if (eff == TY_FLOAT) { regXmm[nreg] = calleeVariadic ? 1 : 2; nvec++; }
                nreg++;
            } else {
                extraStack++;
            }
        }
        if (nreg > 4)
            error_at(node->line, node->col, "too many register arguments (NGCC supports at most 4)");
        if (extraStack > 0 && nstruct > 0)
            error_at(node->line, node->col,
                     "mixing struct and stack arguments is not supported");
        {
            int regSlotBase = 32 + 8 * extraStack + stkTotal;
            S = (n == 0 && stackDepth == 0) ? 0 :
                ((regSlotBase + 8 * nreg + 15) & ~15) + 8 * stackDepth;
            if (S) sub_rsp(S);
            for (i = 0; i < n; i++) {
                Type *at = node_type(node->args[i]);
                Type *pt = ptypes[i];
                if (at && at->kind == TY_STRUCT && at->size > 8) {
                    int so = 32 + stkOff[si++];
                    if (node->args[i]->k == ND_COMPLIT) {
                        int k;
                        /* write the provided initializers, then zero-fill the
                         * remaining members (C compound-literal semantics) */
                        for (k = 0; k < at->nmembers; k++) {
                            lea_rdi_rsp(so + at->members[k].offset);
                            emit1(0x57); stackDepth++;        /* push rdi */
                            if (k < node->args[i]->ninits) {
                                gen_expr(node->args[i]->inits[k]);
                                gen_conv(at->members[k].ty, node_type(node->args[i]->inits[k]));
                            } else {
                                emit1(0xB8), emit32(0);       /* mov eax, 0 */
                            }
                            emit1(0x5F); stackDepth--;        /* pop rdi */
                            store_to_addr_disp(at->members[k].ty, 0);
                        }
                    } else {
                        gen_expr(node->args[i]);              /* src address in rax */
                        lea_rdi_rsp(so);                      /* dst address in rdi */
                        copy_mem(at->size);
                    }
                } else if (ri < 4) {
                    /* convert the argument to the callee's declared parameter
                     * type when we know it (user functions, pow).  Variadic
                     * callees promote float to double (C default promotion). */
                    if (pt && at && pt->kind != at->kind) {
                        gen_expr(node->args[i]);
                        gen_conv(pt, at);
                        at = pt;
                    } else if (calleeVariadic && at && at->kind == TY_FLOAT) {
                        gen_expr(node->args[i]);
                        cvtss2sd();          /* float -> double for varargs */
                        at = &ty_double;
                    } else {
                        gen_expr(node->args[i]);
                    }
                    if (regXmm[ri] == 1)
                        emit6(0xF2, 0x0F, 0x11, 0x44, 0x24, regSlotBase + 8 * ri);  /* movsd [rsp+off], xmm0 */
                    else if (regXmm[ri] == 2)
                        emit6(0xF3, 0x0F, 0x11, 0x44, 0x24, regSlotBase + 8 * ri);  /* movss [rsp+off], xmm0 */
                    else
                        emit5(0x48, 0x89, 0x44, 0x24, regSlotBase + 8 * ri);  /* reg temp slot */
                    ri++;
                } else {
                    gen_expr(node->args[i]);
                    if (pt && at && pt->kind != at->kind) gen_conv(pt, at);
                    else if (calleeVariadic && at && at->kind == TY_FLOAT) { cvtss2sd(); at = &ty_double; }
                    if ((pt ? pt->kind == TY_DOUBLE : (at && at->kind == TY_DOUBLE)) ||
                        (pt ? pt->kind == TY_FLOAT : (at && at->kind == TY_FLOAT)))
                        emit6(0xF2, 0x0F, 0x11, 0x44, 0x24, 32 + 8 * ei);  /* movsd [rsp+32+8ei], xmm0 */
                    else
                        emit5(0x48, 0x89, 0x44, 0x24, 32 + 8 * ei);   /* ABI stack slot [rsp+32] */
                    ei++;
                }
            }
            /* load register arguments.  For variadic callees (printf & co.)
             * the callee copies RCX/RDX/R8/R9 into its shadow space and reads
             * floating-point arguments from there, so a double argument must
             * be placed in BOTH the XMM register and the matching GP register
             * (this mirrors what gcc/MSVC emit for varargs calls). */
            if (nreg >= 1) {
                if (regXmm[0] == 1) {
                    emit6(0xF2, 0x0F, 0x10, 0x44, 0x24, regSlotBase + 0);  /* movsd xmm0, [rsp+off] */
                    emit5(0x48, 0x8B, 0x4C, 0x24, regSlotBase + 0);   /* mov rcx, [rsp+off] (bits) */
                } else if (regXmm[0] == 2) {
                    emit6(0xF3, 0x0F, 0x10, 0x44, 0x24, regSlotBase + 0);  /* movss xmm0, [rsp+off] */
                    emit5(0x48, 0x8B, 0x4C, 0x24, regSlotBase + 0);   /* mov rcx, [rsp+off] (bits) */
                } else emit5(0x48, 0x8B, 0x4C, 0x24, regSlotBase + 0);   /* rcx */
            }
            if (nreg >= 2) {
                if (regXmm[1] == 1) {
                    emit6(0xF2, 0x0F, 0x10, 0x4C, 0x24, regSlotBase + 8);  /* movsd xmm1, [rsp+off] */
                    emit5(0x48, 0x8B, 0x54, 0x24, regSlotBase + 8);   /* mov rdx, [rsp+off] (bits) */
                } else if (regXmm[1] == 2) {
                    emit6(0xF3, 0x0F, 0x10, 0x4C, 0x24, regSlotBase + 8);  /* movss xmm1, [rsp+off] */
                    emit5(0x48, 0x8B, 0x54, 0x24, regSlotBase + 8);   /* mov rdx, [rsp+off] (bits) */
                } else emit5(0x48, 0x8B, 0x54, 0x24, regSlotBase + 8);   /* rdx */
            }
            if (nreg >= 3) {
                if (regXmm[2] == 1) {
                    emit6(0xF2, 0x0F, 0x10, 0x54, 0x24, regSlotBase + 16); /* movsd xmm2, [rsp+off] */
                    emit5(0x4C, 0x8B, 0x44, 0x24, regSlotBase + 16);  /* mov r8, [rsp+off] (bits) */
                } else if (regXmm[2] == 2) {
                    emit6(0xF3, 0x0F, 0x10, 0x54, 0x24, regSlotBase + 16); /* movss xmm2, [rsp+off] */
                    emit5(0x4C, 0x8B, 0x44, 0x24, regSlotBase + 16);  /* mov r8, [rsp+off] (bits) */
                } else emit5(0x4C, 0x8B, 0x44, 0x24, regSlotBase + 16);  /* r8 */
            }
            if (nreg >= 4) {
                if (regXmm[3] == 1) {
                    emit6(0xF2, 0x0F, 0x10, 0x5C, 0x24, regSlotBase + 24); /* movsd xmm3, [rsp+off] */
                    emit5(0x4C, 0x8B, 0x4C, 0x24, regSlotBase + 24);  /* mov r9, [rsp+off] (bits) */
                } else if (regXmm[3] == 2) {
                    emit6(0xF3, 0x0F, 0x10, 0x5C, 0x24, regSlotBase + 24); /* movss xmm3, [rsp+off] */
                    emit5(0x4C, 0x8B, 0x4C, 0x24, regSlotBase + 24);  /* mov r9, [rsp+off] (bits) */
                } else emit5(0x4C, 0x8B, 0x4C, 0x24, regSlotBase + 24);  /* r9 */
            }
            if (nvec > 0) { emit1(0xB0); emit1((unsigned char)nvec); }  /* mov al, nvec (vector-arg count) */
            {
                int gi = name ? find_gfunc(name) : -1;
                if (!indirect && gi >= 0 && gfuncs[gi]->body != NULL) call_rel_func(name);
                else if (!indirect && name && import_index(name) >= 0) call_iat(import_index(name));
                else {
                    /* indirect call: a function-pointer expression, or an
                     * ND_VAR that is not a known function (a fn-pointer variable) */
                    gen_expr(node->fn);             /* function address in rax */
                    emit(0xFF, 0xD0);               /* call rax */
                }
            }
            if (S) add_rsp(S);
        }
    }
}

static void gen_expr(Node *node) {
    switch (node->k) {
    case ND_NUM: {
        Type *nt = node_type(node);
        if (nt->kind == TY_FLOAT) {
            /* float literal: eax = float bits, movd xmm0, eax */
            float fv = (float)node->dval;
            unsigned int u = 0;
            memcpy(&u, &fv, 4);
            emit1(0xB8); emit32(u);                    /* mov eax, imm32 (float bits) */
            emit4(0x66, 0x0F, 0x6E, 0xC0);             /* movd xmm0, eax */
            break;
        }
        if (nt->kind == TY_DOUBLE) {
            /* move the double bits into u (memcpy: exact and self-hostable) */
            unsigned long long u = 0;
            memcpy(&u, &node->dval, 8);
            emit1(0x48); emit1(0xB8);          /* movabs rax, imm64 (double bits) */
            emit32((unsigned)(u & 0xFFFFFFFF));
            emit32((unsigned)((u >> 32) & 0xFFFFFFFF));
            emit1(0x66); emit1(0x48); emit1(0x0F); emit1(0x6E); emit1(0xC0);  /* movq xmm0, rax */
            break;
        }
        if (ty_is_64(nt) && (node->v < 0 || node->v > 0x7FFFFFFFLL)) {
            emit1(0x48); emit1(0xB8);          /* movabs rax, imm64 */
            emit32((unsigned)(node->v & 0xFFFFFFFF));
            emit32((unsigned)((node->v >> 32) & 0xFFFFFFFF));
        } else {
            emit1(0xB8); emit32((unsigned)node->v);   /* mov eax, imm32 */
        }
        break;
    }
    case ND_STR: {
        int off = intern_string(node->bytes, node->blen);
        emit3(0x48, 0x8D, 0x05);                                  /* lea rax, [rip+disp32] */
        add_fixup(FX_DATA, NULL, off, 7);
        break;
    }
    case ND_VAR: {
        Local *v;
        /* function name used as a value: its address */
        if (find_gfunc(node->name) >= 0) {
            emit3(0x48, 0x8D, 0x05);                    /* lea rax, [rip+func] */
            add_fixup(FX_FUNC, node->name, 0, 7);
            break;
        }
        /* builtin stdio streams: stdin/stdout/stderr via __iob_func(),
         * which returns FILE* pointing to the _iob array of FILE structs
         * (each FILE is 48 bytes on msvcrt). */
        if (!strcmp(node->name, "stderr") || !strcmp(node->name, "stdout") ||
            !strcmp(node->name, "stdin")) {
            int idx = !strcmp(node->name, "stdout") ? 1 : (!strcmp(node->name, "stderr") ? 2 : 0);
            int k = stackDepth;
            int S = (k == 0) ? 0 : 32 + 8 * k;
            if (S) sub_rsp(S);
            call_iat(import_index("__iob_func"));
            if (S) add_rsp(S);
            add_rax_imm(idx * 48);
            break;
        }
        v = resolve_var(node);
        if (v->ty->kind == TY_ARRAY) gen_pointer_base(node);   /* array decays to a pointer */
        else load_local(v);
        break;
    }
    case ND_ASSIGN: {
        Type *t = lvalue_type(node->l);
        Type *rt = node_type(node->r);
        gen_addr(node->l);              /* address in rax */
        push_rax();
        gen_expr(node->r);
        gen_conv(t, rt);                /* int<->double implicit conversion */
        pop_rdi();
        if (t->kind == TY_STRUCT) {
            copy_mem(t->size);          /* struct assignment copies the whole value */
        } else {
            store_to_addr(t);
        }
        break;
    }
    case ND_INDEX:
    case ND_DEREF:
        gen_addr(node);                 /* address in rax */
        if (lvalue_type(node)->kind != TY_ARRAY && lvalue_type(node)->kind != TY_STRUCT)
            load_from_addr(lvalue_type(node));
        break;
    case ND_MEMBER:
        gen_addr(node);                 /* member address in rax */
        if (lvalue_type(node)->kind != TY_ARRAY && lvalue_type(node)->kind != TY_STRUCT)
            load_from_addr(lvalue_type(node));
        break;
    case ND_COMPLIT:
        error_at(node->line, node->col,
                 "compound literals are only supported as function arguments");
        break;
    case ND_CAST: {
        Type *ft = node->tp, *ot = node_type(node->o);
        gen_expr(node->o);
        gen_conv(ft, ot);                /* float/double/int conversions */
        break;
    }
    case ND_ADDR:
        gen_addr(node->o);              /* address in rax */
        break;
    case ND_COMMA:
        gen_expr(node->l);
        gen_expr(node->r);
        break;
    case ND_COND: {
        int lElse = new_label(), lEnd = new_label();
        gen_expr(node->cond);
        gen_truth(node_type(node->cond));
        emit(0x85, 0xC0);
        jcc_rel_label(0x84, lElse);     /* jz else */
        gen_expr(node->then);
        jmp_rel_label(lEnd);
        define_label(lElse);
        gen_expr(node->els);
        define_label(lEnd);
        break;
    }
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_AND: case ND_OR: case ND_XOR: case ND_SHL: case ND_SHR:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
        gen_binary(node);
        break;
    case ND_LAND: gen_logical(node, 1); break;
    case ND_LOR:  gen_logical(node, 0); break;
    case ND_NEG:
        gen_expr(node->o);
        if (node_type(node->o)->kind == TY_FLOAT) {
            /* -f in xmm0: xor the sign bit via xmm1 */
            emit1(0xB8); emit32(0x80000000);           /* mov eax, 0x80000000 */
            emit4(0x66, 0x0F, 0x6E, 0xC8);             /* movd xmm1, eax */
            emit3(0x0F, 0x57, 0xC1);                   /* xorps xmm0, xmm1 */
        } else if (node_type(node->o)->kind == TY_DOUBLE) {
            /* -d in xmm0: xor the sign bit via xmm1 */
            emit4(0x66, 0x0F, 0x57, 0xC9);          /* xorpd xmm1, xmm1 */
            emit1(0x48); emit1(0xB8); emit64(0x8000000000000000ULL);  /* movabs rax, sign */
            emit5(0x66, 0x48, 0x0F, 0x6E, 0xC8);    /* movq xmm1, rax */
            emit4(0x66, 0x0F, 0x57, 0xC1);          /* xorpd xmm0, xmm1 */
        } else if (ty_is_64(node_type(node->o))) emit3(0x48, 0xF7, 0xD8);  /* neg rax */
        else emit(0xF7, 0xD8);                                     /* neg eax */
        break;
    case ND_NOT:
        gen_expr(node->o);
        if (node_type(node->o)->kind == TY_FLOAT) {
            /* !f: f==0.0f? -> 0/1 in eax */
            emit3(0x0F, 0x57, 0xC9);                /* xorps xmm1, xmm1 */
            emit3(0x0F, 0x2E, 0xC8);                /* ucomiss xmm1, xmm0 (0.0f vs f) */
            emit3(0x0F, 0x94, 0xC0);                /* sete al */
            emit3(0x0F, 0xB6, 0xC0);                /* movzx eax, al */
        } else if (node_type(node->o)->kind == TY_DOUBLE) {
            /* !d: d==0.0? -> 0/1 in eax */
            emit4(0x66, 0x0F, 0x57, 0xC9);          /* xorpd xmm1, xmm1 */
            emit4(0x66, 0x0F, 0x2E, 0xC8);          /* ucomisd xmm1, xmm0 (0.0 vs d) */
            emit3(0x0F, 0x94, 0xC0);                /* sete al */
            emit3(0x0F, 0xB6, 0xC0);                /* movzx eax, al */
        } else {
            emit(0x85, 0xC0);                       /* test eax, eax */
            emit3(0x0F, 0x94, 0xC0);                /* sete al */
            emit3(0x0F, 0xB6, 0xC0);                /* movzx eax, al */
        }
        break;
    case ND_BITNOT:
        gen_expr(node->o);
        if (ty_is_64(node_type(node->o))) emit3(0x48, 0xF7, 0xD0);  /* not rax */
        else emit(0xF7, 0xD0);                                     /* not eax */
        break;
    case ND_PREINC:
    case ND_PREDEC: {
        Type *t = lvalue_type(node->o);
        char k = type_kind(t);
        gen_addr(node->o);
        push_rax();                     /* push addr */
        if (k == 's') {
            error_at(node->line, node->col, "cannot increment a struct value");
        }
        load_from_addr(t);
        if (k == 'f') {
            /* f += 1.0f / f -= 1.0f in xmm0 */
            emit1(0xB8); emit32(0x3F800000);           /* mov eax, 1.0f */
            emit4(0x66, 0x0F, 0x6E, 0xC8);             /* movd xmm1, eax */
            if (node->k == ND_PREINC) emit4(0xF3, 0x0F, 0x58, 0xC1);  /* addss xmm0, xmm1 */
            else emit4(0xF3, 0x0F, 0x5C, 0xC1);                      /* subss xmm0, xmm1 */
        } else if (k == 'd') {
            /* d += 1.0 / d -= 1.0 in xmm0 */
            emit1(0x48); emit1(0xB8); emit64(0x3FF0000000000000ULL);  /* movabs rax, 1.0 */
            emit5(0x66, 0x48, 0x0F, 0x6E, 0xC8);    /* movq xmm1, rax */
            if (node->k == ND_PREINC) emit4(0xF2, 0x0F, 0x58, 0xC1);  /* addsd xmm0, xmm1 */
            else emit4(0xF2, 0x0F, 0x5C, 0xC1);                      /* subsd xmm0, xmm1 */
        } else if (k == 'p' || k == 'l' || k == 'u') {
            int es = (k == 'p') ? ty_elem_size(t) : 1;
            if (node->k == ND_PREINC) emit4(0x48, 0x83, 0xC0, es);
            else emit4(0x48, 0x83, 0xE8, es);
        } else {
            if (node->k == ND_PREINC) emit3(0x83, 0xC0, 1);
            else emit3(0x83, 0xE8, 1);
        }
        pop_rdi();                      /* pop rdi (addr) */
        store_to_addr(t);
        break;
    }
    case ND_POSTINC:
    case ND_POSTDEC: {
        Type *t = lvalue_type(node->o);
        char k = type_kind(t);
        gen_addr(node->o);
        emit3(0x48, 0x89, 0xC3);        /* mov rbx, rax  (keep the address) */
        load_from_addr(t);              /* old value in rax / xmm0 */
        if (k == 'f') {
            emit4(0x66, 0x0F, 0x7E, 0xC0);  /* movd eax, xmm0 (save old) */
            push_rax();                    /* push old value */
            emit1(0xB8); emit32(0x3F800000);           /* mov eax, 1.0f */
            emit4(0x66, 0x0F, 0x6E, 0xC8);             /* movd xmm1, eax */
            if (node->k == ND_POSTINC) emit4(0xF3, 0x0F, 0x58, 0xC1);  /* addss xmm0, xmm1 */
            else emit4(0xF3, 0x0F, 0x5C, 0xC1);                       /* subss xmm0, xmm1 */
            emit4(0xF3, 0x0F, 0x11, 0x03);         /* movss [rbx], xmm0 */
            pop_rax();
            emit4(0x66, 0x0F, 0x6E, 0xC0);          /* movd xmm0, eax (restore old) */
        } else if (k == 'd') {
            emit5(0x66, 0x48, 0x0F, 0x7E, 0xC0);  /* movq rax, xmm0 (save old) */
            push_rax();                            /* push old value */
            emit1(0x48); emit1(0xB8); emit64(0x3FF0000000000000ULL);  /* movabs rax, 1.0 */
            emit5(0x66, 0x48, 0x0F, 0x6E, 0xC8);   /* movq xmm1, rax */
            if (node->k == ND_POSTINC) emit4(0xF2, 0x0F, 0x58, 0xC1);  /* addsd xmm0, xmm1 */
            else emit4(0xF2, 0x0F, 0x5C, 0xC1);                       /* subsd xmm0, xmm1 */
            emit4(0xF2, 0x0F, 0x11, 0x03);         /* movsd [rbx], xmm0 */
            pop_rax();
            emit5(0x66, 0x48, 0x0F, 0x6E, 0xC0);   /* movq xmm0, rax (restore old) */
        } else {
        push_rax();                     /* push old value */
        if (k == 'p' || k == 'l' || k == 'u') {
            int es = (k == 'p') ? ty_elem_size(t) : 1;
            if (node->k == ND_POSTINC) emit4(0x48, 0x83, 0xC0, es);
            else emit4(0x48, 0x83, 0xE8, es);
        } else {
            if (node->k == ND_POSTINC) emit3(0x83, 0xC0, 1);
            else emit3(0x83, 0xE8, 1);
        }
        if (k == 'p' || k == 'l' || k == 'u') emit3(0x48, 0x89, 0x03);  /* mov [rbx], rax */
        else if (k == 'c') emit(0x88, 0x03);                           /* mov [rbx], al */
        else emit(0x89, 0x03);                                         /* mov [rbx], eax */
        pop_rax();                      /* pop rax (old value) */
        }
        break;
    }
    case ND_CALL: gen_call(node); break;
    default:
        error_at(node->line, node->col, "internal error: unknown expression node");
    }
}

static void gen_stmt(Node *node) {
    int i;
    switch (node->k) {
    case ND_BLOCK:
        for (i = 0; i < node->nstmts; i++) gen_stmt(node->stmts[i]);
        break;
    case ND_DECL:
        for (i = 0; i < node->ndecls; i++) {
            Node *d = node->decls[i];
            if (d->tp->kind == TY_ARRAY) {
                int pi = 0;
                gen_array_init_flat(d->tp, d->off, d->is_global, d->inits, d->ninits, &pi);
            } else if (d->tp->kind == TY_STRUCT && d->ninits > 0) {
                /* struct { ... } initializer: write each member by its offset */
                int k;
                for (k = 0; k < d->ninits && k < d->tp->nmembers; k++) {
                    if (d->is_global) { emit3(0x48, 0x8D, 0x05); add_fixup(FX_DATA, NULL, d->off, 7); }
                    else emit_lea_rbp(R_RAX, d->off);
                    if (d->tp->members[k].offset) add_rax_imm(d->tp->members[k].offset);
                    push_rax();
                    gen_expr(d->inits[k]);
                    gen_conv(d->tp->members[k].ty, node_type(d->inits[k]));
                    pop_rdi();
                    store_to_addr(d->tp->members[k].ty);
                }
            } else if (d->init) {
                gen_expr(d->init);
                gen_conv(d->tp, node_type(d->init));   /* int<->double for scalars */
                if (d->is_global) {
                    /* scalar global: store via rip-relative */
                    store_rip(d->tp, d->off);
                } else {
                    Local tmp;
                    tmp.name = d->name; tmp.off = d->off; tmp.ty = d->tp;
                    tmp.is_param = 0; tmp.is_global = 0; tmp.is_static = 0;
                    store_local(&tmp);
                }
            }
        }
        break;
    case ND_EXPR:
        gen_expr(node->expr);
        break;
    case ND_IF: {
        int lEnd = new_label();
        gen_expr(node->cond);
        gen_truth(node_type(node->cond));
        emit(0x85, 0xC0);
        if (node->els) {
            int lElse = new_label();
            jcc_rel_label(0x84, lElse);          /* jz else */
            gen_stmt(node->then);
            jmp_rel_label(lEnd);
            define_label(lElse);
            gen_stmt(node->els);
            define_label(lEnd);
        } else {
            jcc_rel_label(0x84, lEnd);
            gen_stmt(node->then);
            define_label(lEnd);
        }
        break;
    }
    case ND_WHILE: {
        int lBegin = new_label(), lEnd = new_label();
        define_label(lBegin);
        gen_expr(node->cond);
        gen_truth(node_type(node->cond));
        emit(0x85, 0xC0);
        jcc_rel_label(0x84, lEnd);
        if (loopDepth < 256) { brkStack[loopDepth] = lEnd; contStack[loopDepth] = lBegin; }
        loopDepth++;
        gen_stmt(node->body);
        loopDepth--;
        jmp_rel_label(lBegin);
        define_label(lEnd);
        break;
    }
    case ND_SWITCH: {
        int i, lEnd = new_label(), lDef = new_label(), tempOff = curGenFn->maxOffset + 8;
        gen_expr(node->cond);
        emit_mov_store_rbp64(R_RAX, tempOff);    /* save switch value */
        if (switchDepth < 256) switchBrk[switchDepth] = lEnd;
        switchDepth++;
        for (i = 0; i < node->ncases; i++) {
            node->cases[i].label = new_label();
            emit_mov_load_rbp64(R_RAX, tempOff);
            emit1(0x48); emit1(0x3D);            /* cmp rax, imm32 */
            emit32((unsigned)node->cases[i].val);
            jcc_rel_label(0x84, node->cases[i].label);   /* jz case */
        }
        if (node->defbody) jmp_rel_label(lDef);
        else jmp_rel_label(lEnd);
        for (i = 0; i < node->ncases; i++) {
            define_label(node->cases[i].label);
            gen_stmt(node->cases[i].body);
        }
        if (node->defbody) {
            define_label(lDef);
            gen_stmt(node->defbody);
        }
        define_label(lEnd);
        switchDepth--;
        break;
    }
    case ND_FOR: {
        int lBegin, lCont, lEnd;
        if (node->init) gen_stmt(node->init);
        lBegin = new_label(); lCont = new_label(); lEnd = new_label();
        define_label(lBegin);
        if (node->cond) {
            gen_expr(node->cond);
            gen_truth(node_type(node->cond));
            emit(0x85, 0xC0);
            jcc_rel_label(0x84, lEnd);
        }
        if (loopDepth < 256) { brkStack[loopDepth] = lEnd; contStack[loopDepth] = lCont; }
        loopDepth++;
        gen_stmt(node->body);
        loopDepth--;
        define_label(lCont);
        if (node->inc) gen_expr(node->inc);
        jmp_rel_label(lBegin);
        define_label(lEnd);
        break;
    }
    case ND_RETURN:
        if (node->expr) {
            gen_expr(node->expr);
            if (curGenFn->ret)
                gen_conv(curGenFn->ret, node_type(node->expr));   /* int<->double */
        } else {
            if (curGenFn->ret && curGenFn->ret->kind == TY_DOUBLE) xorpd_xmm0();
            else if (curGenFn->ret && curGenFn->ret->kind == TY_FLOAT) xorps_xmm0();
            else emit(0x31, 0xC0);                 /* xor eax, eax */
        }
        jmp_rel_label(epiLabel);
        break;
    case ND_BREAK:
        if (switchDepth > 0) jmp_rel_label(switchBrk[switchDepth - 1]);
        else if (loopDepth > 0) jmp_rel_label(brkStack[loopDepth - 1]);
        else error_at(node->line, node->col, "break used outside of a loop or switch");
        break;
    case ND_CONTINUE:
        if (loopDepth <= 0)
            error_at(node->line, node->col, "continue used outside of a loop");
        jmp_rel_label(contStack[loopDepth - 1]);
        break;
    default:
        error_at(node->line, node->col, "internal error: unknown statement node");
    }
}

static void gen_function(Func *fn) {
    int i, frame;
    int argRegs[4] = { R_RCX, R_RDX, 8, 9 };
    if (!fn->body) return;   /* prototype only: nothing to emit */
    if (fn->emitted) return; /* already generated (dependency discovery) */
    fn->emitted = 1;
    fn->codeOff = codeLen;
    curLocals = fn->locals;
    curNLocals = fn->nlocals;
    curGenFn = fn;
    /* prologue */
    emit1(0x55);                 /* push rbp */
    emit3(0x48, 0x89, 0xE5);     /* mov rbp, rsp */
    frame = (fn->maxOffset + 48 + 15) & ~15;
    sub_rsp(frame);
    /* store parameters into their stack slots */
    {
        int stkoff = 48;   /* first stack argument sits at [rbp+48] */
        for (i = 0; i < fn->nparams; i++) {
            Local *p = &fn->locals[i];
            unsigned char d = (unsigned char)(-p->off);
            int reg = (i < 4) ? argRegs[i] : 0;
            char k = type_kind(p->ty);
            if (k == 's' && p->ty->size > 8) {
                /* struct-by-value parameter: copy from the caller's stack area */
                emit1(0x56);                               /* push rsi */
                emit4(0x48, 0x8D, 0x75, stkoff & 0xFF);    /* lea rsi, [rbp+stkoff] */
                emit_lea_rbp(R_RDI, p->off);               /* lea rdi, [rbp-off] */
                emit1(0xB9); emit32((unsigned)p->ty->size);/* mov ecx, size */
                emit(0xF3, 0xA4);                          /* rep movsb */
                emit1(0x5E);                               /* pop rsi */
                stkoff += (p->ty->size + 7) & ~7;
            } else if (i >= 4) {
                /* 5th+ scalar parameter arrives on the stack at [rbp+48+8*(i-4)] */
                int so = 48 + 8 * (i - 4);
                if (k == 'f') {
                    emit5(0xF3, 0x0F, 0x10, 0x45, so & 0xFF);  /* movss xmm0, [rbp+so] */
                    store_flt_rbp(p->off);
                } else if (k == 'd') {
                    emit5(0xF2, 0x0F, 0x10, 0x45, so & 0xFF);  /* movsd xmm0, [rbp+so] */
                    store_dbl_rbp(p->off);
                } else if (k == 'p' || k == 'l' || k == 'u') {
                    emit4(0x48, 0x8B, 0x45, so & 0xFF);   /* mov rax, [rbp+so] */
                    emit_mov_store_rbp64(R_RAX, p->off);
                } else if (k == 'c') {
                    emit4(0x0F, 0xB6, 0x45, so & 0xFF);   /* movzx eax, byte [rbp+so] */
                    emit_mov_store_rbp8(R_RAX, p->off);
                } else {
                    emit3(0x8B, 0x45, so & 0xFF);         /* mov eax, [rbp+so] */
                    emit_mov_store_rbp32(R_RAX, p->off);
                }
            } else if (k == 'f') {
                /* float parameter arrives in xmm0..xmm3 (low 32 bits) */
                emit5(0xF3, 0x0F, 0x11, modrm(1, i, R_RBP), d);   /* movss [rbp-off], xmm{i} */
            } else if (k == 'd') {
                /* double parameter arrives in xmm0..xmm3 (parallel to rcx..r9) */
                emit5(0xF2, 0x0F, 0x11, modrm(1, i, R_RBP), d);   /* movsd [rbp-off], xmm{i} */
            } else if (k == 'p' || k == 'l' || k == 'u') {
                if (reg < 8) emit4(0x48, 0x89, modrm(1, reg, R_RBP), d);
                else emit4(0x4C, 0x89, modrm(1, reg - 8, R_RBP), d);
            } else {
                if (reg < 8) emit3(0x89, modrm(1, reg, R_RBP), d);
                else emit4(0x44, 0x89, modrm(1, reg - 8, R_RBP), d);
            }
        }
    }
    /* variadic functions: keep the register arguments reachable from va_list */
    if (fn->variadic) {
        /* store rcx,rdx,r8,r9 into the caller's shadow area [rbp+16..rbp+40] */
        emit4(0x48, 0x89, 0x4D, 0x10);   /* mov [rbp+16], rcx */
        emit4(0x48, 0x89, 0x55, 0x18);   /* mov [rbp+24], rdx */
        emit4(0x4C, 0x89, 0x45, 0x20);   /* mov [rbp+32], r8 */
        emit4(0x4C, 0x89, 0x4D, 0x28);   /* mov [rbp+40], r9 */
    }
    epiLabel = new_label();
    loopDepth = 0;
    stackDepth = 0;
    for (i = 0; i < fn->body->nstmts; i++) gen_stmt(fn->body->stmts[i]);
    if (fn->ret && fn->ret->kind == TY_DOUBLE) xorpd_xmm0();   /* fall-through returns 0.0 */
    else if (fn->ret && fn->ret->kind == TY_FLOAT) xorps_xmm0();
    else emit(0x31, 0xC0);            /* xor eax, eax (fall-through return value) */
    define_label(epiLabel);
    emit3(0x48, 0x89, 0xEC);     /* mov rsp, rbp */
    emit1(0x5D);                 /* pop rbp */
    emit1(0xC3);                 /* ret */
    curLocals = NULL;
    curNLocals = 0;
    curGenFn = NULL;
}

/* startup code that initialises globals and function-local statics */
/* emit initializers for array t whose storage starts at absolute offset
 * absOff (.data offset when isData, rbp-relative slot when !isData).
 * inits is the flat row-major (padded) list; *pi indexes into it; entries
 * beyond ninits are written as zero (partial initialization).
 * NOTE: elements are addressed with base+index*elemSize (upwards), matching
 * how array accesses (ND_INDEX) are generated. */
static void gen_array_init_flat(Type *t, int absOff, int isData, Node **inits, int ninits, int *pi) {
    int k;
    for (k = 0; k < t->array_len; k++) {
        if (t->base->kind == TY_ARRAY) {
            /* row offset: .data grows upward (+), the local frame grows
             * downward from rbp (off counts upward toward rbp, so a later
             * row sits at a smaller off) */
            int elemOff = isData ? absOff + k * ty_size(t->base)
                                 : absOff - k * ty_size(t->base);
            gen_array_init_flat(t->base, elemOff, isData, inits, ninits, pi);
        } else {
            if (isData) { emit3(0x48, 0x8D, 0x05); add_fixup(FX_DATA, NULL, absOff, 7); }
            else emit_lea_rbp(R_RAX, absOff);
            if (k) add_rax_imm(k * ty_size(t->base));
            push_rax();
            if (*pi < ninits) {
                gen_expr(inits[*pi]);
                gen_conv(t->base, node_type(inits[*pi]));
            } else {
                emit1(0xB8), emit32(0);      /* mov eax, 0 */
                if (t->base->kind == TY_DOUBLE) xorpd_xmm0();
                else if (t->base->kind == TY_FLOAT) xorps_xmm0();
            }
            (*pi)++;
            pop_rdi();
            store_to_addr(t->base);
        }
    }
}

static void gen_global_init_code(void) {
    int i, k;
    for (i = 0; i < nglobals; i++) {
        Global *g = &globals[i];
        if (g->is_bss) continue;
        if (g->ty->kind == TY_ARRAY) {
            int pi = 0;
            gen_array_init_flat(g->ty, g->off, 1, g->inits, g->ninits, &pi);
        } else if (g->ty->kind == TY_STRUCT && g->ninits > 0) {
            for (k = 0; k < g->ninits && k < g->ty->nmembers; k++) {
                emit3(0x48, 0x8D, 0x05); add_fixup(FX_DATA, NULL, g->off, 7);
                add_rax_imm(g->ty->members[k].offset);
                push_rax();
                gen_expr(g->inits[k]);
                gen_conv(g->ty->members[k].ty, node_type(g->inits[k]));
                pop_rdi();
                store_to_addr(g->ty->members[k].ty);
            }
        } else if (g->init) {
            gen_expr(g->init);
            gen_conv(g->ty, node_type(g->init));
            store_rip(g->ty, g->off);
        }
    }
}

/* parse the command line into argc/argv (rcx=argc, rdx=argv) */
static void gen_cmdline_argv(void) {
    int off_argv = add_global("__ngcc_argv", new_array_type(new_ptr_type(&ty_char), 64), NULL, 0, NULL, 0);
    int off_argbuf = add_global("__ngcc_argbuf", new_array_type(&ty_char, 8192), NULL, 0, NULL, 0);
    int lSkip = new_label(), lNext = new_label(), lCopy = new_label();
    int lQuote = new_label(), lQcopy = new_label(), lQend = new_label();
    int lTokend = new_label(), lDone = new_label();
    sub_rsp(0x28);
    call_iat(import_index("GetCommandLineA"));
    add_rsp(0x28);
    emit3(0x48, 0x89, 0xC6);          /* mov rsi, rax */
    emit(0x31, 0xD2);                 /* xor edx, edx (argc) */
    emit3(0x48, 0x8D, 0x3D); add_fixup(FX_DATA, NULL, off_argbuf, 7);  /* lea rdi, [rip+buf] */
    define_label(lSkip);
    emit(0x8A, 0x06);                 /* mov al, [rsi] */
    emit(0x84, 0xC0);                 /* test al, al */
    jcc_rel_label(0x84, lDone);       /* jz done */
    emit(0x3C, 0x20); jcc_rel_label(0x84, lNext);   /* cmp al,' '; jz next */
    emit(0x3C, 0x09); jcc_rel_label(0x84, lNext);   /* cmp al,'\t' */
    emit(0x3C, 0x0D); jcc_rel_label(0x84, lNext);   /* cmp al,'\r' */
    emit(0x3C, 0x0A); jcc_rel_label(0x84, lNext);   /* cmp al,'\n' */
    emit3(0x48, 0x8D, 0x0D); add_fixup(FX_DATA, NULL, off_argv, 7);   /* lea rcx, [rip+argv] */
    emit4(0x48, 0x89, 0x3C, 0xD1);    /* mov [rcx+rdx*8], rdi */
    emit(0xFF, 0xC2);                 /* inc edx */
    define_label(lCopy);
    emit(0x8A, 0x06);
    emit(0x84, 0xC0);
    jcc_rel_label(0x84, lDone);        /* NUL: end of command line -> done */
    emit(0x3C, 0x20); jcc_rel_label(0x84, lTokend);
    emit(0x3C, 0x09); jcc_rel_label(0x84, lTokend);
    emit(0x3C, 0x22); jcc_rel_label(0x84, lQuote);   /* cmp al,'"' */
    emit(0x88, 0x07);                 /* mov [rdi], al */
    emit3(0x48, 0xFF, 0xC7);          /* inc rdi */
    emit3(0x48, 0xFF, 0xC6);          /* inc rsi */
    jmp_rel_label(lCopy);
    define_label(lQuote);
    emit3(0x48, 0xFF, 0xC6);          /* inc rsi */
    define_label(lQcopy);
    emit(0x8A, 0x06);
    emit(0x84, 0xC0);
    jcc_rel_label(0x84, lDone);        /* NUL: end of command line -> done */
    emit(0x3C, 0x22); jcc_rel_label(0x84, lQend);
    emit(0x88, 0x07);
    emit3(0x48, 0xFF, 0xC7);
    emit3(0x48, 0xFF, 0xC6);
    jmp_rel_label(lQcopy);
    define_label(lQend);
    emit3(0x48, 0xFF, 0xC6);
    jmp_rel_label(lCopy);
    define_label(lTokend);
    emit3(0xC6, 0x07, 0x00);          /* mov byte [rdi], 0 */
    emit3(0x48, 0xFF, 0xC7);          /* inc rdi */
    define_label(lNext);
    emit3(0x48, 0xFF, 0xC6);          /* inc rsi */
    jmp_rel_label(lSkip);
    define_label(lDone);
    emit3(0xC6, 0x07, 0x00);          /* mov byte [rdi], 0 */
    emit3(0x48, 0x8D, 0x0D); add_fixup(FX_DATA, NULL, off_argv, 7);
    emit4(0x48, 0xC7, 0x04, 0xD1); emit32(0);   /* mov qword [rcx+rdx*8], 0 */
    emit(0x89, 0xD1);                 /* mov ecx, edx (argc) */
    emit3(0x48, 0x8D, 0x15); add_fixup(FX_DATA, NULL, off_argv, 7);   /* lea rdx, [rip+argv] */
}

static void gen_start(void) {
    int mi = find_gfunc("main");
    emit4(0x48, 0x83, 0xE4, 0xF0);       /* and rsp, -16 */
    gen_global_init_code();
    gen_cmdline_argv();                  /* rcx = argc, rdx = argv */
    emit3(0x45, 0x31, 0xC0);              /* xor r8d, r8d */
    emit3(0x45, 0x31, 0xC9);              /* xor r9d, r9d */
    call_rel_func("main");
    if (gfuncs[mi]->ret->kind == TY_VOID) emit(0x31, 0xC0);  /* xor eax, eax */
    emit(0x89, 0xC1);                    /* mov ecx, eax (exit code) */
    call_iat(import_index("ExitProcess"));
}

static void patch_fixups(void) {
    int i;
    for (i = 0; i < nfixups; i++) {
        Fixup *fx = &fixups[i];
        long long target;
        int gi;
        if (fx->t == FX_FUNC) {
            gi = find_gfunc(fx->name);
            if (gi >= 0 && gfuncs[gi]->body != NULL) {
                target = IMAGE_BASE + RVA_TEXT + gfuncs[gi]->codeOff;
            } else if (import_index(fx->name) >= 0) {
                target = IMAGE_BASE + idataBaseRVA + iatEntryOffsets[import_index(fx->name)];
            } else {
                error_at(0, 0, "undefined reference to '%s'", fx->name);
                target = 0;
            }
        } else if (fx->t == FX_LABEL) {
            target = IMAGE_BASE + RVA_TEXT + find_label(fx->id);
        } else if (fx->t == FX_DATA) {
            target = IMAGE_BASE + dataBaseRVA + fx->id;
        } else {
            target = IMAGE_BASE + idataBaseRVA + iatEntryOffsets[fx->id];
        }
        patch32(fx->pos, (unsigned)(target - (IMAGE_BASE + RVA_TEXT + fx->pos + 4)));
    }
}

/* ============================================================ PE writer */

static void put16(unsigned char *p, int o, unsigned v) {
    p[o] = v & 0xFF; p[o + 1] = (v >> 8) & 0xFF;
}
static void put32(unsigned char *p, int o, unsigned v) {
    p[o] = v & 0xFF; p[o + 1] = (v >> 8) & 0xFF;
    p[o + 2] = (v >> 16) & 0xFF; p[o + 3] = (v >> 24) & 0xFF;
}
static void put64(unsigned char *p, int o, unsigned long long v) {
    int i;
    for (i = 0; i < 8; i++) p[o + i] = (unsigned char)(v >> (8 * i));
}

static unsigned char *build_idata(int *sizeOut) {
    idataBaseRVA = dataBaseRVA + ((dataLen + 0xFFF) & ~0xFFF);   /* place .idata after .data */
    static const char *dlls[][35] = {
        { "msvcrt.dll", "printf", "scanf", "puts", "putchar", "getchar", "exit",
          "malloc", "free", "calloc", "rand", "srand", "time", "__iob_func",
          "fopen", "fread", "fwrite", "fclose", "fseek", "ftell",
          "fprintf", "vfprintf", "fputc", "memcpy", "memset", "memcmp",
          "strcmp", "strlen", "strncmp", "strchr", "strrchr", "getenv", "realloc",
          "pow",
          NULL },
        { "kernel32.dll", "GetCommandLineA", "ExitProcess", "FindFirstFileA", "FindNextFileA", "FindClose",
          NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
    };
    static const int ndlls = 2;
    unsigned char *b;
    int off = 60;                       /* 3 descriptors */
    int nameOffsets[2], intOffsets[2], iatOffsets[2];
    int funcOffsets[NIMPORTS];
    int d, k, doff;
    b = xmalloc(4096);
    memset(b, 0, 4096);
    for (d = 0; d < ndlls; d++) {
        const char *dn = dlls[d][0];
        int len = (int)strlen(dn);
        nameOffsets[d] = off;
        memcpy(b + off, dn, (size_t)len);
        off += len + 1;
    }
    for (d = 0; d < ndlls; d++) {
        for (k = 1; dlls[d][k]; k++) {
            const char *fn = dlls[d][k];
            int len = (int)strlen(fn);
            int idx;
            for (idx = 0; idx < NIMPORTS; idx++)
                if (!strcmp(IMPORT_FUNCS[idx], fn)) break;
            if (off % 2) off++;
            funcOffsets[idx] = off;
            b[off++] = 0; b[off++] = 0;   /* hint */
            memcpy(b + off, fn, (size_t)len);
            off += len + 1;
        }
    }
    for (d = 0; d < ndlls; d++) {
        intOffsets[d] = off;
        for (k = 1; dlls[d][k]; k++) {
            int idx;
            for (idx = 0; idx < NIMPORTS; idx++)
                if (!strcmp(IMPORT_FUNCS[idx], dlls[d][k])) break;
            put32(b, off, idataBaseRVA + (unsigned)funcOffsets[idx]);
            off += 8;
        }
        off += 8;                       /* null terminator */
    }
    iatBaseOff = off;
    for (d = 0; d < ndlls; d++) {
        iatOffsets[d] = off;
        for (k = 1; dlls[d][k]; k++) {
            int idx;
            for (idx = 0; idx < NIMPORTS; idx++)
                if (!strcmp(IMPORT_FUNCS[idx], dlls[d][k])) break;
            iatEntryOffsets[idx] = off;
            put32(b, off, idataBaseRVA + (unsigned)funcOffsets[idx]);
            off += 8;
        }
        off += 8;
    }
    iatTotalSize = off - iatBaseOff;
    doff = 0;
    for (d = 0; d < ndlls; d++) {
        put32(b, doff, idataBaseRVA + (unsigned)intOffsets[d]);       /* OriginalFirstThunk */
        put32(b, doff + 12, idataBaseRVA + (unsigned)nameOffsets[d]); /* Name */
        put32(b, doff + 16, idataBaseRVA + (unsigned)iatOffsets[d]);  /* FirstThunk */
        doff += 20;
    }
    *sizeOut = off;
    return b;
}

static void write_section(unsigned char *p, int o, const char *name, int vsize,
                          int vaddr, int rawSize, int rawPtr, unsigned chars) {
    int i, len = (int)strlen(name);
    for (i = 0; i < 8; i++) p[o + i] = i < len ? (unsigned char)name[i] : 0;
    put32(p, o + 8, (unsigned)vsize);
    put32(p, o + 12, (unsigned)vaddr);
    put32(p, o + 16, (unsigned)rawSize);
    put32(p, o + 20, (unsigned)rawPtr);
    put32(p, o + 36, chars);
}

/* round v up to the next multiple of a */
static int align_up(int v, int a) { return ((v + a - 1) / a) * a; }

static unsigned char *build_pe(const unsigned char *codeBytes, int codeSize,
                               const unsigned char *dataBytes, int dataSize,
                               const unsigned char *idataBytes, int idataSize,
                               int *sizeOut) {
    int rawCode = align_up(codeSize, 0x200);
    int rawData = align_up(dataSize, 0x200);
    int rawIdata = align_up(idataSize, 0x200);
    int sizeOfImage = idataBaseRVA + align_up(idataSize, 0x1000);
    unsigned char *file;
    int oh = 0x58, dd, sh;
    file = xmalloc((size_t)(0x200 + rawCode + rawData + rawIdata));
    memset(file, 0, (size_t)(0x200 + rawCode + rawData + rawIdata));
    /* DOS header */
    memcpy(file, "MZ", 2);
    put32(file, 0x3C, 0x40);            /* e_lfanew */
    /* PE signature */
    memcpy(file + 0x40, "PE\0\0", 4);
    /* COFF header */
    put16(file, 0x44, 0x8664);          /* Machine: AMD64 */
    put16(file, 0x46, 3);               /* NumberOfSections */
    put16(file, 0x54, 0xF0);            /* SizeOfOptionalHeader */
    put16(file, 0x56, 0x0022);          /* Characteristics */
    /* Optional header (PE32+) */
    put16(file, oh + 0, 0x20B);         /* Magic */
    put32(file, oh + 4, (unsigned)rawCode);
    put32(file, oh + 8, (unsigned)(rawData + rawIdata));
    put32(file, oh + 16, RVA_TEXT);     /* AddressOfEntryPoint */
    put32(file, oh + 20, RVA_TEXT);     /* BaseOfCode */
    put64(file, oh + 24, IMAGE_BASE);   /* ImageBase */
    put32(file, oh + 32, 0x1000);       /* SectionAlignment */
    put32(file, oh + 36, 0x200);        /* FileAlignment */
    put16(file, oh + 40, 6);            /* MajorOperatingSystemVersion */
    put16(file, oh + 48, 6);            /* MajorSubsystemVersion */
    put32(file, oh + 56, (unsigned)sizeOfImage);
    put32(file, oh + 60, 0x200);        /* SizeOfHeaders */
    put16(file, oh + 68, 3);            /* Subsystem: console */
    put32(file, oh + 72, 0x100000);     /* SizeOfStackReserve (low) */
    put32(file, oh + 80, 0x1000);       /* SizeOfStackCommit (low) */
    put32(file, oh + 88, 0x100000);     /* SizeOfHeapReserve (low) */
    put32(file, oh + 96, 0x1000);       /* SizeOfHeapCommit (low) */
    put32(file, oh + 108, 16);          /* NumberOfRvaAndSizes */
    dd = oh + 112;                      /* data directories */
    put32(file, dd + 8, idataBaseRVA);     /* dir[1]: import table */
    put32(file, dd + 12, (unsigned)idataSize);
    put32(file, dd + 96, idataBaseRVA + (unsigned)iatBaseOff);   /* dir[12]: IAT */
    put32(file, dd + 100, (unsigned)iatTotalSize);
    /* section headers */
    sh = oh + 0xF0;
    write_section(file, sh, ".text", codeSize, RVA_TEXT, rawCode, 0x200, 0x60000020);
    write_section(file, sh + 40, ".data", dataSize, dataBaseRVA, rawData, 0x200 + rawCode, 0xC0000040);
    write_section(file, sh + 80, ".idata", idataSize, idataBaseRVA, rawIdata,
                  0x200 + rawCode + rawData, 0xC0000040);
    /* payload */
    memcpy(file + 0x200, codeBytes, (size_t)codeSize);
    memcpy(file + 0x200 + rawCode, dataBytes, (size_t)dataSize);
    memcpy(file + 0x200 + rawCode + rawData, idataBytes, (size_t)idataSize);
    *sizeOut = 0x200 + rawCode + rawData + rawIdata;
    return file;
}

/* ============================================================ driver */

/* minimal WIN32_FIND_DATAA (kernel32; also imported for self-hosted builds).
 * NOTE: use 4-byte 'int' fields (DWORD) — ngcc's 'unsigned long'/'unsigned'
 * are 8 bytes, which would misalign cFileName for the Windows API. */
typedef struct {
    int dwFileAttributes;
    int ftCreationTime[2];
    int ftLastAccessTime[2];
    int ftLastWriteTime[2];
    int nFileSizeHigh;
    int nFileSizeLow;
    int dwReserved0, dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} NgFindData;
extern void *FindFirstFileA(const char *name, NgFindData *fd);
extern int FindNextFileA(void *h, NgFindData *fd);
extern int FindClose(void *h);

static char *read_file(const char *path, long *lenOut) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) {
        fprintf(stderr, "NGCC: error: cannot read '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = xmalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "NGCC: error: cannot read '%s'\n", path);
        exit(1);
    }
    buf[sz] = 0;
    fclose(f);
    *lenOut = sz;
    return buf;
}

/* directory of a source path ("" if none); copies into out (out[0]=0 if no dir) */
static void src_dir(const char *src, char *out, int outcap) {
    const char *slash = strrchr(src, '\\');
    const char *slash2 = strrchr(src, '/');
    int dlen;
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (!slash) { out[0] = 0; return; }
    dlen = (int)(slash - src);
    if (dlen >= outcap) dlen = outcap - 1;
    memcpy(out, src, (size_t)dlen);
    out[dlen] = 0;
}

/* list "dir\\*.c" (or "*.c" when dir is empty); returns malloc'd array of
 * malloc'd full paths; caller frees. */
static char **list_dir_c_files(const char *dir, int *outN) {
    char pattern[600];
    NgFindData fd;
    void *h;
    char **files = NULL;
    int n = 0, cap = 0;
    if (dir[0]) {
        int l = (int)strlen(dir);
        if (l + 6 >= (int)sizeof(pattern)) { *outN = 0; return NULL; }
        memcpy(pattern, dir, (size_t)l);
        memcpy(pattern + l, "\\*.c", 5);
        pattern[l + 5] = 0;
    } else {
        memcpy(pattern, "*.c", 4);
    }
    h = FindFirstFileA(pattern, &fd);
    if (h == (void *)-1) { *outN = 0; return NULL; }
    for (;;) {
        char *full;
        int fl = (int)strlen(fd.cFileName);
        if (fl > 2 && !strcmp(fd.cFileName + fl - 2, ".c")) {
            if (n == cap) { cap = cap ? cap * 2 : 16; files = xrealloc(files, (size_t)cap * sizeof(char *)); }
            full = xmalloc((size_t)fl + (dir[0] ? (size_t)strlen(dir) + 2 : 1));
            if (dir[0]) {
                int dl = (int)strlen(dir);
                memcpy(full, dir, (size_t)dl);
                full[dl] = '\\';
                memcpy(full + dl + 1, fd.cFileName, (size_t)fl + 1);
            } else {
                memcpy(full, fd.cFileName, (size_t)fl + 1);
            }
            files[n++] = full;
        }
        if (!FindNextFileA(h, &fd)) break;
    }
    FindClose(h);
    *outN = n;
    return files;
}

/* lightweight scan: collect top-level function definition names in a .c file.
 * Recognises "ident ( ... ) {" at brace depth 0.  Purely token-based, so a
 * file can be checked for a missing definition without full parsing. */
static void scan_file_funcs(const char *path, const char ***out, int *outN) {
    long len;
    char *src;
    Token *ts;
    int nt, i, depth = 0;
    const char **names = NULL;
    int n = 0, cap = 0;
    Macro *sM = gMacros; int sN = gNmacros;
    int sInc = nincludedFiles; const char **sIncArr = includedFiles;
    int sIncCap = capIncludedFiles;
    FILE *f = fopen(path, "rb");
    if (!f) { *out = NULL; *outN = 0; return; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    src = xmalloc((size_t)len + 1);
    if (len > 0 && fread(src, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(src); *out = NULL; *outN = 0; return; }
    src[len] = 0;
    fclose(f);
    gMacros = NULL; gNmacros = 0; nincludedFiles = 0; includedFiles = NULL;
    capIncludedFiles = 0;
    ts = tokenize(src, 0, &nt);
    for (i = 0; i < nt; i++) {
        Token *t = &ts[i];
        if (t->kind == TK_PUNCT && !strcmp(t->text, "{")) { depth++; continue; }
        if (t->kind == TK_PUNCT && !strcmp(t->text, "}")) { if (depth > 0) depth--; continue; }
        if (depth != 0) continue;
        if (t->kind == TK_IDENT && i + 1 < nt &&
            ts[i + 1].kind == TK_PUNCT && !strcmp(ts[i + 1].text, "(")) {
            /* find the matching ')' then check for '{' */
            int d = 1, k = i + 2;
            while (k < nt && d > 0) {
                if (ts[k].kind == TK_PUNCT && !strcmp(ts[k].text, "(")) d++;
                else if (ts[k].kind == TK_PUNCT && !strcmp(ts[k].text, ")")) d--;
                k++;
            }
            if (d == 0 && k < nt && ts[k].kind == TK_PUNCT && !strcmp(ts[k].text, "{")) {
                int j, dup = 0;
                for (j = 0; j < n; j++) if (!strcmp(names[j], t->text)) { dup = 1; break; }
                if (!dup) {
                    if (n == cap) { cap = cap ? cap * 2 : 8; names = xrealloc(names, (size_t)cap * sizeof(const char *)); }
                    names[n++] = t->text;
                }
            }
        }
    }
    free(ts);
    free(src);
    gMacros = sM; gNmacros = sN;
    nincludedFiles = sInc; includedFiles = sIncArr;
    capIncludedFiles = sIncCap;
    *out = names;
    *outN = n;
}

static void usage(void) {
    fprintf(stderr,
        "NGCC - a small C compiler for Windows x64 (produces native .exe, like gcc)\n"
        "\n"
        "Usage: ngcc <source.c> [-o <output.exe>]\n"
        "       ngcc <source>          (auto-appends .c)\n"
        "       ngcc <source> <out>    (auto-appends .c / .exe)\n"
        "       ngcc a.c b.c           (multi-file: one exe from several .c)\n"
        "       ngcc -v, --version     (show version)\n"
        "       ngcc -h, --help        (this help)\n"
        "\n"
        "When a called function is declared but never defined, ngcc automatically\n"
        "scans the source file's directory and compiles any .c that defines it\n"
        "(transitively).  Local headers via #include \"sub/name.h\" are resolved\n"
        "relative to the source file.\n"
        "\n"
        "Compiles C into a native Windows x64 .exe (PE32+), self-contained.\n"
        "No external tools required.\n"
        "\n"
        "GitHub: https://github.com/Jokerdajinbao\n"
        "Bilibili: https://space.bilibili.com/41660208\n"
        "Built with the Deepseek Harness agent tool.\n");
}

static void print_version(void) {
    fprintf(stderr, "NGCC version %s\n", NGCC_VERSION);
}

int main(int argc, char **argv) {
    const char *srcs[16];
    int nsrcs = 0;
    const char *out = NULL;
    long srcLen;
    Token *tokens;
    int ntoks;
    Parser parser;
    int i, si, mi;
    unsigned char *idata;
    int idataSize;
    unsigned char *pe;
    int peSize;
    FILE *f;
    char *outBase;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if (i + 1 >= argc) { fprintf(stderr, "NGCC: error: missing argument for '%s'\n", argv[i]); return 1; }
            {
                /* auto-append ".exe" when the output name has no suffix */
                const char *ov = argv[++i];
                int olen = (int)strlen(ov);
                if (olen > 4 && !strcmp(ov + olen - 4, ".exe")) {
                    out = ov;
                } else {
                    char *s = xmalloc((size_t)olen + 5);
                    memcpy(s, ov, (size_t)olen);
                    memcpy(s + olen, ".exe", 5);
                    out = s;
                }
            }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            print_version();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "NGCC: error: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            int sl = (int)strlen(argv[i]);
            int isC = sl > 2 && !strcmp(argv[i] + sl - 2, ".c");
            if (nsrcs == 0) {
                /* first non-option argument: the source file; auto-append ".c" */
                if (isC) {
                    srcs[nsrcs++] = argv[i];
                } else {
                    char *s = xmalloc((size_t)sl + 3);
                    memcpy(s, argv[i], (size_t)sl);
                    memcpy(s + sl, ".c", 3);
                    srcs[nsrcs++] = s;
                }
            } else if (isC) {
                /* another source file (multi-file compilation) */
                if (nsrcs < 16) srcs[nsrcs++] = argv[i];
                else { fprintf(stderr, "NGCC: error: too many input files\n"); return 1; }
            } else if (!out) {
                /* a non-.c second positional argument: the output file name */
                if (sl > 4 && !strcmp(argv[i] + sl - 4, ".exe")) {
                    out = argv[i];
                } else {
                    char *s = xmalloc((size_t)sl + 5);
                    memcpy(s, argv[i], (size_t)sl);
                    memcpy(s + sl, ".exe", 5);
                    out = s;
                }
            } else {
                fprintf(stderr, "NGCC: error: too many input files (NGCC compiles one or more .c files)\n");
                return 1;
            }
        }
    }
    if (nsrcs == 0) { usage(); return 1; }

    /* parse all command-line source files (prototypes merge with definitions) */
    for (si = 0; si < nsrcs; si++) {
        int fi;
        const char *src = srcs[si];
        curFile = src;
        curSrc = read_file(src, &srcLen);
        nincludedFiles = 0;   /* fresh include-once set per translation unit */
        gMacros = NULL;
        gNmacros = 0;         /* fresh macro table per translation unit */

        tokens = tokenize(curSrc, 0, &ntoks);
        if (getenv("NGCC_DUMP_TOKENS")) {
            int ti;
            for (ti = 0; ti < ntoks; ti++) {
                Token *t = &tokens[ti];
                fprintf(stderr, "tok[%d] kind=%d text=%s line=%d col=%d\n",
                        ti, t->kind, t->text ? t->text : "", t->line, t->col);
            }
        }
        memset(&parser, 0, sizeof(parser));
        parser.toks = tokens;
        while (parser.toks[parser.pos].kind != TK_EOF) {
            if (at_punct(&parser, ";")) { parser.pos++; continue; }
            if (is_function_decl(&parser))
                parse_function(&parser);
            else
                parse_decl(&parser);   /* global variable / typedef */
        }
        /* merge this translation unit's functions into the shared list,
         * letting definitions supersede earlier prototypes */
        for (fi = 0; fi < parser.nfuncs; fi++) {
            Func *fn = parser.funcs[fi];
            int gi = find_gfunc(fn->name);
            if (gi >= 0) {
                Func *ex = gfuncs[gi];
                if (ex->body == NULL && fn->body != NULL) {
                    gfuncs[gi] = fn;          /* definition replaces prototype */
                } else if (ex->body != NULL && fn->body != NULL) {
                    error_at(0, 0, "duplicate definition of function '%s'", fn->name);
                }
                /* prototype after a definition, or prototype after prototype: keep */
            } else {
                if (ngfuncs == capgfuncs) {
                    capgfuncs = capgfuncs ? capgfuncs * 2 : 32;
                    gfuncs = xrealloc(gfuncs, (size_t)capgfuncs * sizeof(Func *));
                }
                gfuncs[ngfuncs++] = fn;
            }
        }
        free(tokens);
        free((char *)curSrc);
    }

    mi = find_gfunc("main");
    if (mi < 0)
        error_at(0, 0, "no 'main' function found");
    if (gfuncs[mi]->body == NULL)
        error_at(0, 0, "'main' is declared but never defined");

    gen_start();

    /* automatic same-directory dependency discovery:
     * when a called function is only declared (prototype) but never defined,
     * look for a .c file in the same directory that defines it and compile it
     * too (like gcc, but without any config: just "obvious" neighbours).
     * gen_function() skips already-emitted functions via fn->emitted, so the
     * whole table can be regenerated each round at no cost. */
    {
        char dir[300];
        char **candFiles = NULL;
        int ncand = 0;
        const char **added = NULL;
        int nadded = 0, capadded = 0;
        int didAdd = 1;
        src_dir(srcs[0], dir, (int)sizeof(dir));
        while (didAdd) {
            const char **missing = NULL;
            int nmissing = 0, capmissing = 0;
            int fx;
            /* generate everything parsed so far (emitted flag prevents dupes) */
            for (i = 0; i < ngfuncs; i++) gen_function(gfuncs[i]);
            /* collect missing symbols from FX_FUNC fixups emitted so far:
             * only functions actually called (or addressed) but undefined */
            for (fx = 0; fx < nfixups; fx++) {
                Fixup *f = &fixups[fx];
                int gi, j;
                if (f->t != FX_FUNC) continue;
                gi = find_gfunc(f->name);
                if (gi >= 0 && gfuncs[gi]->body != NULL) continue;
                /* a prototype that names a DLL import (extern) is not missing */
                if (import_index(f->name) >= 0) continue;
                if (gi < 0) continue;   /* name-only reference without prototype */
                for (j = 0; j < nmissing; j++)
                    if (!strcmp(missing[j], f->name)) break;
                if (j == nmissing) {
                    if (nmissing == capmissing) {
                        capmissing = capmissing ? capmissing * 2 : 8;
                        missing = xrealloc(missing, (size_t)capmissing * sizeof(const char *));
                    }
                    missing[nmissing++] = f->name;
                }
            }
            didAdd = 0;
            if (nmissing > 0) {
                /* enumerate the directory's .c files once */
                if (!candFiles)
                    candFiles = list_dir_c_files(dir, &ncand);
                for (fx = 0; fx < ncand && !didAdd; fx++) {
                    const char **defs = NULL;
                    int ndefs = 0, d;
                    int already = 0, s;
                    for (s = 0; s < nsrcs; s++)
                        if (!strcmp(candFiles[fx], srcs[s])) { already = 1; break; }
                    if (already) continue;
                    for (s = 0; s < nadded; s++)
                        if (!strcmp(candFiles[fx], added[s])) { already = 1; break; }
                    if (already) continue;
                    scan_file_funcs(candFiles[fx], &defs, &ndefs);
                    for (d = 0; d < ndefs && !didAdd; d++) {
                        int m;
                        for (m = 0; m < nmissing; m++) {
                            if (!strcmp(defs[d], missing[m])) {
                                /* this file defines a missing symbol: compile it */
                                const char *src = candFiles[fx];
                                int fi2;
                                curFile = src;
                                curSrc = read_file(src, &srcLen);
                                nincludedFiles = 0;
                                gMacros = NULL;
                                gNmacros = 0;
                                tokens = tokenize(curSrc, 0, &ntoks);
                                memset(&parser, 0, sizeof(parser));
                                parser.toks = tokens;
                                while (parser.toks[parser.pos].kind != TK_EOF) {
                                    if (at_punct(&parser, ";")) { parser.pos++; continue; }
                                    if (is_function_decl(&parser))
                                        parse_function(&parser);
                                    else
                                        parse_decl(&parser);
                                }
                                for (fi2 = 0; fi2 < parser.nfuncs; fi2++) {
                                    Func *fn = parser.funcs[fi2];
                                    int gi2 = find_gfunc(fn->name);
                                    if (gi2 >= 0) {
                                        Func *ex = gfuncs[gi2];
                                        if (ex->body == NULL && fn->body != NULL)
                                            gfuncs[gi2] = fn;
                                        else if (ex->body != NULL && fn->body != NULL)
                                            error_at(0, 0, "duplicate definition of function '%s'", fn->name);
                                    } else {
                                        if (ngfuncs == capgfuncs) {
                                            capgfuncs = capgfuncs ? capgfuncs * 2 : 32;
                                            gfuncs = xrealloc(gfuncs, (size_t)capgfuncs * sizeof(Func *));
                                        }
                                        gfuncs[ngfuncs++] = fn;
                                    }
                                }
                                free(tokens);
                                free((char *)curSrc);
                                if (nadded == capadded) {
                                    capadded = capadded ? capadded * 2 : 16;
                                    added = xrealloc(added, (size_t)capadded * sizeof(const char *));
                                }
                                added[nadded++] = src;
                                didAdd = 1;
                                break;
                            }
                        }
                    }
                    free((void *)defs);
                }
            }
            free(missing);
        }
        if (candFiles) {
            for (i = 0; i < ncand; i++) free(candFiles[i]);
            free(candFiles);
        }
        free((void *)added);
    }

    dataBaseRVA = RVA_TEXT + ((codeLen + 0xFFF) & ~0xFFF);   /* .data right after .text */

    idata = build_idata(&idataSize);
    patch_fixups();
    pe = build_pe(code, codeLen, dataBuf, dataLen, idata, idataSize, &peSize);

    if (!out) {
        const char *base = srcs[0];
        const char *slash = strrchr(srcs[0], '\\');
        const char *slash2 = strrchr(srcs[0], '/');
        const char *dot;
        int blen;
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        base = slash ? slash + 1 : srcs[0];
        dot = strrchr(base, '.');
        blen = dot ? (int)(dot - base) : (int)strlen(base);
        outBase = xmalloc((size_t)blen + 5);
        memcpy(outBase, base, (size_t)blen);
        memcpy(outBase + blen, ".exe", 5);
        out = outBase;
    }

    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "NGCC: error: cannot write '%s'\n", out);
        return 1;
    }
    if (fwrite(pe, 1, (size_t)peSize, f) != (size_t)peSize) {
        fprintf(stderr, "NGCC: error: cannot write '%s'\n", out);
        fclose(f);
        return 1;
    }
    fclose(f);
    fprintf(stderr, "NGCC: %s -> %s (%d bytes)\n", srcs[0], out, peSize);
    return 0;
}

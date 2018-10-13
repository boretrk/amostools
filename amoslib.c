#include "amoslib.h"

static void print_binary(FILE *out, unsigned int value) {
    char buf[33], *p = &buf[33];
    *--p = '\0';
    do {*--p = value & 1 ? '1' : '0'; value >>= 1; } while (value);
    fprintf(out, "%%%s", p);
}

typedef union { unsigned int i; float f; } flint;

static void print_float(FILE *out, unsigned int value) {
    char buf[30], *p;
    flint f = {.i = 0};
    /* convert AMOS float to IEEE 754 float */
    if (value) {
	int m = (value >> 8) & 0x7FFFFF;
	int s = (value >> 7) & 1;
	int e = (value & 0x7F) + 62;
	f.i = (s << 31) | ((e & 0xFF) << 23) | m;
    }
    /* print via a buffer so we can see what was produced */
    snprintf(buf, sizeof(buf), "%G", f.f);
    fprintf(out, "%s", buf);
    /* add ".0" if %G format did not include it */
    for (p = &buf[0]; *p; p++) if (*p == '.' || *p == 'E') return;
    fprintf(out, ".0");
}

static char *lookup_token(int slot, int offset,
			  struct AMOS_token *table[AMOS_TOKEN_TABLE_SIZE])
{
    struct AMOS_token *e;
    int key = (slot << 16) | offset;
    for (e = table[key % AMOS_TOKEN_TABLE_SIZE]; e; e = e->next) {
	if (e->key == key) return e->text;
    }
    return NULL;
}

int AMOS_print_source(unsigned char *src, size_t len, FILE *out,
		      struct AMOS_token *table[AMOS_TOKEN_TABLE_SIZE])
{
    unsigned int slot, token, linelen=0, inpos, i, compiled_len = 0;
    unsigned char *line, *endline, space_just_printed;
    int err = 0;

    /* while we have remaining input bytes */
    for (inpos = 0; inpos < len;) {
	/* skip compiled procedures */
	if (compiled_len) {
	    fprintf(out, "   ' COMPILED PROCEDURE -- can't convert this to AMOS code\n");
	    inpos += compiled_len + 8 - linelen;
	    if (inpos > len) break;
	    compiled_len = 0;
	}

	line = &src[inpos];
	linelen = line[0] * 2;
	inpos += linelen;

	/* stop if line claims to be zero length (bad data) */
	if (linelen == 0) {
	    err |= 1;
	    break;
	}

	/* if the line is said to be longer than we actually have bytes for,
	 * mark this as an error, but continue with the bytes we have. */
	if (inpos > len) {
	    err |= 1;
	    linelen = len - (inpos - linelen);
	}

	/*printf("LINE: ");for(i=0;i<linelen;i+=2)printf("%02X%02X ",line[i],line[i+1]);puts("");*/

	/* start the line with the given indent level */
	if ((i = line[1]) > 1) {
	    while (i-- > 1) putc(' ', out);
	}
	space_just_printed = 1;

	/* decode this line */
	endline = &line[linelen];
	line += 2;

	while ((line < endline) && (token = amos_deek(line))) {
	    line += 2;

	    if (token <= 0x0018) {
		/* Tokens 0x0000 to 0x0018 are "variable" tokens. These represent
		 * variable, label and procedure names:
		 * - 0x0006 = TkVar, variable reference e.g. "X" in "Print X".
		 * - 0x000C = TkLab, label, e.g. "X:" or "190" (at the start of a line)
		 * - 0x0012 = TkPro, procedure reference, e.g. "X" in "X[42]"
		 * - 0x0018 = TkLGo, label reference, e.g. "X" in "Goto X"
		 *
		 * All these tokens have the following format:
		 * - 2 bytes: unknown purpose
		 * - 1 byte:  length of ASCII string for variable/label name
		 * - 1 byte:  flags. For TkVar, TkPro and TkLGo:
		 *            - flags & 0x01 = this is a floating point ref (eg "X#")
		 *            - flags & 0x02 = this is a string ref (eg "X$")
		 * - n bytes: ASCII string, with the above-given string length,
		 *            rounded to a multiple of two, null terminated.
		 */
		for (i = 0; i < line[2]; i++) {
		    unsigned char c = line[4+i];
		    if (!c) break;
		    if (c >= 'a' && c <= 'z') c -= ('a'-'A'); /* to uppercase */
		    putc(c, out);
		}
		if (token == 0x000C) {
		    /* if not a "line number" label, the label needs a colon */
		    if (!(line[4] >= '0' && line[4] <= '9')) putc(':', out);
		}
		else {
		    if (line[3] & 0x01) putc('#', out);
		    else if (line[3] & 0x02) putc('$', out);
		    if (token == 0x0012) {
			putc(' ', out);
			space_just_printed = 1;
		    }
		}
		/* advance to the next token */
		line += ((line[2] & 1) ? 5 : 4) + line[2];
	    }
	    else if (token < 0x004E) {
		/* Tokens 0x0019 to 0x004D are "constant" tokens. These represent
		 * literal numbers and strings:
		 *
		 * - 0x001E = TkBin, a binary integer, e.g. %100101
		 * - 0x0026 = TkCh1, a string with double quotes, e.g. "hello"
		 * - 0x002E = TkCh2, a string with single quotes, e.g. 'hello'
		 * - 0x0036 = TkHex, a hexidecimal integer, e.g. $80FAA010
		 * - 0x003E = TkEnt, a decimal integer, e.g. 1234567890
		 * - 0x0046 = TkFl,  a floating-point number, eg 3.142
		 *
		 * TkBin, TkHex, TkEnt, TkFl have this format:
		 * - 4 bytes: the value
		 *
		 * TkCh1 and TkCh2 have this format:
		 * - 2 bytes: the length of the string
		 * - n bytes: the string, for the above number of bytes rounded up
		 *            to a multiple of two bytes and padded with nulls.
		 */
		switch (token) {
		case 0x001E: /* TkBin */
		    print_binary(out, amos_leek(line)); line += 4;
		    break;
		case 0x0026: /* TkCh1 */
		    i = amos_deek(line); line += 2;
		    fprintf(out, "\"%s\"", i ? (char *) line : "");
		    line += i; if (i & 1) line++;
		    break;
		case 0x002E: /* TkCh2 */
		    i = amos_deek(line); line += 2;
		    fprintf(out, "'%s'", i ? (char *) line : "");
		    line += i; if (i & 1) line++;
		    break;
		case 0x0036: /* TkHex */
		    fprintf(out, "$%X", amos_leek(line)); line += 4;
		    break;
		case 0x003E: /* TkEnt */
		    fprintf(out, "%d", (int) amos_leek(line)); line += 4;
		    break;
		case 0x0046: /* TkFl */
		    print_float(out, amos_leek(line)); line += 4;
		    break;
		default:
		    fprintf(out, "ILLEGAL_CONST_%04X", token);
		    err |= 2;
		}
	    }
	    else {
		char *tok;
		/* all other tokens: 0x004E to 0xFFFF
		 *
		 * other than the extension tokens, these are actual instructions,
		 * functions or system variables which reside in AMOS's token table.
		 * The offset given leads to the handlers for these instructions,
		 * as well as their name and what would be correct parameters.
		 *
		 * AMOS allows "extensions" to the language. They have a token table
		 * just like AMOS, each extension has its own table.
		 *
		 * The extension token is token 0x004E and has this format:
		 * - 1 byte:  extension number [1-25]
		 * - 1 byte:  unused
		 * - 2 bytes: offset into extension's token table
		 *
		 * Other than this, some tokens in the core language have a special
		 * format. They are:
		 *
		 * TkRem1 (0x064A) and TkRem2 (0x0652):
		 * - 1 byte:  unused
		 * - 1 byte:  length of the remark
		 * - n bytes: the remark -- ASCII text, null terminated,
		 *            padded to a multiple of two bytes.
		 *
		 * TkFor (0x023C), TkRpt (0x0250), TkWhl (0x0268), TkDo (0x027E),
		 * TkIf (0x02BE), TkElse (0x02D0), TkData (0x0404) and AMOS Pro's
		 * Else If (0x25A4)
		 * - 2 bytes: unknown purpose
		 *
		 * TkExIf (0x0290), TkExit (0x029E) and TkOn (0x0316):
		 * - 4 bytes: unknown purpose
		 *
		 * TkProc (0x0376)
		 * - 4 bytes: number of bytes to corresponding ENDPROC line
		 *            (start of line + 8 + above = start of ENDPROC line)
		 *            (start of line + 8 + 6 + above = line _after_ ENDPROC)
		 * - 2 bytes: part of seed for encryption
		 * - 1 byte: flags:
		 *   - flags & 0x80 -- procedure is folded
		 *   - flags & 0x40 -- procedure is locked and should not be unfolded
		 *   - flags & 0x20 -- procedure is currently encrypted
		 *   - flags & 0x10 -- procedure contains compiled code, not tokens
		 * - 1 byte: part of seed for encryption
		 */
		if (token == 0x004E) {
		    slot = line[0];
		    token = amos_deek(&line[2]);
		    line += 4;
		}
		else {
		    slot = 0;
		}

		if ((tok = lookup_token(slot, token, table))) {
		    /* avoid double-printing spaces */
		    if (*tok == ' ' && space_just_printed) tok++;
		    fprintf(out, "%s", tok);
		    while (*tok) space_just_printed = (*tok++ == ' ');
		}
		else {
		    /* unknown token */
		    fprintf(out, "EXTENSION_%02X_%04X", slot, token);
		    err |= 4;
		}

		/* special tokens in the core language with extra data after them */
		if (slot == 0) {
		    switch (token) {
		    case 0x064A: /* TkRem1 */
		    case 0x0652: /* TkRem2 */
			fprintf(out, "%s", &line[2]);
			i = line[1]; line += 2 + i; if (i & 1) line++;
			break;

		    case 0x023C: /* TkFor */
		    case 0x0250: /* TkRpt */
		    case 0x0268: /* TkWhl */
		    case 0x027E: /* TkDo */
		    case 0x02BE: /* TkIf */
		    case 0x02D0: /* TkElse */
		    case 0x0404: /* TkData */
		    case 0x25A4: /* AMOS Pro "Else If" */
			line += 2;
			break;
          
		    case 0x0290: /* TkExIf */
		    case 0x029E: /* TkExit */
		    case 0x0316: /* TkOn */
			line += 4;
			break;
          
		    case 0x0376: /* TkProc */
			if (line[6] & 0x20) AMOS_decrypt_procedure(line-4);
			if (line[6] & 0x10) compiled_len = amos_leek(&line[0]);
			line += 8;
			break;
		    }
		}
	    }
	}
	putc('\n', out);
    }
    return err;
}

void AMOS_decrypt_procedure(unsigned char *src) {
    unsigned char *line, *next, *endline;
    unsigned int key, key2, key3, size;

    /* do not operate on compiled procedures */
    if (src[10] & 0x10) return;

    size = amos_leek(&src[4]);
    line = next = &src[src[0] * 2]; /* the line after PROCEDURE */
    endline = &src[size + 8 + 6]; /* the start of the line after END PROC */

    /* initialise keys */
    key = (size << 8) | src[11];
    key2 = 1;
    key3 = amos_deek(&src[8]);

    while (line < endline) {
        line = next; next = &line[line[0] * 2];
	if (!line[0]) return; /* avoid infinite loop on bad data */
        for (line += 4; line < next;) {
            *line++ ^= (key >> 8) & 0xFF;
            *line++ ^=  key       & 0xFF;
            key  += key2;
            key2 += key3;
            key = (key >> 1) | (key << 31); /* rotate right one bit */
        }
    }
    src[10] ^= 0x20; /* toggle "is encrypted" bit */
}


/* parse extension names out of an AMOS 1.3/Pro interpreter config file */
int AMOS_parse_config(unsigned char *src, size_t len,
		      char *slots[AMOS_EXTENSION_SLOTS])
{
    /* AMOSPro_Interpreter_Config format: PId1 / PIt1 */
    if (len > 100 && amos_leek(src) == 0x50496431) {
        unsigned int idlen = amos_leek(&src[4]);
        if (idlen < (len - 92) && amos_leek(&src[idlen + 8]) == 0x50497431) {
            unsigned char *p = &src[idlen + 16];
            int i;
            /* config strings 16-41 are extensions 1-25 */
            for (i = 1; i < (16+AMOS_EXTENSION_SLOTS); i++) {
                if (i >= 16) slots[i - 16] = (char *) &p[2];
                p += p[1] + 2;
		if ((p - src) > (int) len) return 1;
            }
            return 0; /* success */
        }
    }

    /* AMOS1_3_Pal.env, etc. format: Amiga code hunk */
    if (len > 300 && amos_leek(src) == 0x3f3 && amos_leek(&src[24]) == 0x3e9) {
	unsigned int dta = amos_leek(&src[32]);
        /* look up config entry 66 */
        unsigned int offset = amos_deek(&src[36 + 65 * 4]) + 36 - dta;
	unsigned int flags  = amos_deek(&src[36 + 65 * 4 + 2]);
	/* entry must be list of strings */
        if (flags & 0x8000 && offset < len) {
	    unsigned char *s = &src[offset], *end = &src[len];
	    int i;
	    for (i = 0; i < AMOS_EXTENSION_SLOTS; i++) {
		if (s >= end) return 1;
		if (*s == 0xFF) break; /* end of list */
		slots[i] = (char *) s;
		while (*s++ && s < end); /* skip string */
	    }
	    for (; i < AMOS_EXTENSION_SLOTS; i++) {
		slots[i] = NULL;
	    }
	    return 0; /* success */
        }
    }
    
    return 1; /* failure */
}

static int add_token(unsigned int key, unsigned char *name, char type,
		     struct AMOS_token *table[AMOS_TOKEN_TABLE_SIZE],
		     unsigned char **last_name)
{
    int len, upcase, done, append_space, prepend_space;
    struct AMOS_token *e;
    unsigned char *s;

    /* if name begins with '!', it can be recalled with blank name */
    if (*name == 0x80) {
	if (!(name = *last_name)) return 0; /* skip this token */
    }
    else if (*name == '!') {
	*last_name = ++name; /* skip '!', save name */
    }

    /* if type is not O, 0, 1, 2 or V, prepend a space (if not already done) */
    prepend_space = (*name != ' ' && type != 'O' && type != '0' &&
		      type != '1' && type != '2' && type != 'V');

    /* if type is I, append a space */
    append_space = (type == 'I');
    
    /* measure length of name */
    len = 0;
    while (name[len] < 0x80) len++;
    if (prepend_space) len++;
    if (append_space) len++;
    len++;

    /* allocate token table entry and link it into table */
    e = malloc(sizeof(struct AMOS_token) + len);
    if (!e) return 1; /* failure, out of memory */
    e->key = key;
    e->next = table[key % AMOS_TOKEN_TABLE_SIZE];
    table[key % AMOS_TOKEN_TABLE_SIZE] = e;

    /* copy text, capitalise words */
    s = (unsigned char *) &e->text[0];
    if (prepend_space) *s++ = ' ';
    for (upcase = 1, done = 0; !done;) {
	unsigned char c = *name++;
	if (c & 0x80) done = 1, c &= 0x7F;
	if (c >= 'a' && c <= 'z') {
	    if (upcase) upcase = 0, c -= 'a'-'A';
	}
	else if (c == ' ') {
	    upcase = 1; /* end of word */
	}
	*s++ = (char) c;
    }
    if (append_space) *s++ = ' ';
    *s++ = '\0';
    /*printf("$%06x: %s\n", e->key, e->text);*/
    return 0; /* success */
}

int AMOS_parse_extension(unsigned char *src, size_t len, int slot, int start,
			 struct AMOS_token *table[AMOS_TOKEN_TABLE_SIZE])
{
    unsigned char *p, *end = &src[len], *pname, *ptype, *last_name = NULL;
    unsigned int tkoff;

    /* Extension format is an Amiga hunk file with a single code hunk */
    if (len < 54 || amos_leek(src) != 0x3f3 || amos_leek(&src[24]) != 0x3e9) {
	return 1;
    }

    /* the first bytes in the code hunk are a header: 4 longwords and 1 word
     * The four longwords are the sizes of the 4 sections that follow.
     * AMOSPro 2.0 appends 4 bytes to the header, the text "AP20" */
    tkoff = amos_leek(&src[32]) + 32 + 18;
    if (amos_leek(&src[32 + 18]) == 0x41503230) tkoff += 4;
    if (tkoff > len) return 1;

    /* each token has this format:
     * 2 byte "instruction" code pointer (0 marks end of table)
     * 2 byte "function" code pointer
     * instruction name (ASCII chars, final char has high bit set)
     * instruction parameters (ASCII ended by 0xFF, 0xFE or 0xFD)
     * 0/1 byte: realign to word boundary if needed
     */
    for (p = &src[tkoff + start]; (p+2) < end;) {
	/* unique key is slot and 16-bit offset within table */
	unsigned int key = (slot << 16) | (((p - src) - tkoff) & 0xFFFF);
	if (!amos_deek(p)) return 0; /* success: reached end of list */
	p += 4;
	pname = p; while (p < end && *p < 0x80) p++; p++;
	ptype = p; while (p < end && *p < 0xFD) p++; p++;
	if ((p - src) & 1) p++; /* re-align to word boundary */

	/* add entry to token table */
	if (add_token(key, pname, *ptype, table, &last_name)) return 1;
    }	
    return 1; /* failure: ran out of list before end */
}

/* find extension number by scanning init code for MOVE #slot-1,D0 before RTS
 * This works on all extensions I can find, except:
 * Dump.Lib V1.1 (has an RTS just before the desired MOVEQ #n,D0 / RTS)
 * AMOSPro_TURBO_Plus.Lib V2.15 (complex startup code)
 * Intuition.Lib / AMOSPro_Intuition.Lib V1.3a (complex startup code)
 */
int AMOS_find_slot(unsigned char *src, size_t len) {
    unsigned char *p, *end;
    unsigned int tkoff, codeoff, titleoff;
    int b = -1, w = -1, l = -1;

    if (len < 50) return -1;

    tkoff    = amos_leek(&src[32]) + 32 + 18;
    codeoff  = amos_leek(&src[36]) + tkoff;
    titleoff = amos_leek(&src[40]) + codeoff;
    if (amos_leek(&src[32 + 18]) == 0x41503230) {
	tkoff += 4;
	codeoff += 4;
	titleoff += 4;
    }
    if (codeoff > len || titleoff > len) return -1;
    
    for (p = &src[codeoff], end = &src[titleoff]; p+2 < end; p += 2) {
	unsigned int c = amos_deek(p);
	if (c == 0x4E75) { /* stop at first RTS */
	    break;
	}
	else if (c >= 0x7000 && c <= (0x7000 | AMOS_EXTENSION_SLOTS)) {
	    b = (c & 0xFF) + 1; /* MOVEQ #slot-1,D0 */
	}
	else if (c == 0x303C && p+4 < end) {
	    c = amos_deek(&p[2]); /* MOVE.W #slot-1,D0 */
	    if (c <= AMOS_EXTENSION_SLOTS) w = c + 1;
	}
	else if (c == 0x203C && p+6 < end) {
	    c = amos_leek(&p[2]); /* MOVE.L #slot-1,D0 */
	    if (c <= AMOS_EXTENSION_SLOTS) l = c + 1;
	}
    }

    /* Prefer MOVEQ (most extensions) over MOVE.L (needed only for TOME
     * and CTEXT) and MOVE.W (needed only for SLN) */
    return (b != -1) ? b : (l != -1) ? l : w;
}

void AMOS_free_tokens(struct AMOS_token *table[AMOS_TOKEN_TABLE_SIZE]) {
    int i;
    for (i = 0; i < AMOS_TOKEN_TABLE_SIZE; i++) {
	struct AMOS_token *e, *next;
	for (e = table[i]; e; e = next) {
	    next = e->next;
	    free(e);
	}
    }
}

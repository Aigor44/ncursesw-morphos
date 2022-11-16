/****************************************************************************
 * Copyright 2018-2021,2022 Thomas E. Dickey                                *
 * Copyright 1998-2016,2017 Free Software Foundation, Inc.                  *
 *                                                                          *
 * Permission is hereby granted, free of charge, to any person obtaining a  *
 * copy of this software and associated documentation files (the            *
 * "Software"), to deal in the Software without restriction, including      *
 * without limitation the rights to use, copy, modify, merge, publish,      *
 * distribute, distribute with modifications, sublicense, and/or sell       *
 * copies of the Software, and to permit persons to whom the Software is    *
 * furnished to do so, subject to the following conditions:                 *
 *                                                                          *
 * The above copyright notice and this permission notice shall be included  *
 * in all copies or substantial portions of the Software.                   *
 *                                                                          *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS  *
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF               *
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.   *
 * IN NO EVENT SHALL THE ABOVE COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,   *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR    *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR    *
 * THE USE OR OTHER DEALINGS IN THE SOFTWARE.                               *
 *                                                                          *
 * Except as contained in this notice, the name(s) of the above copyright   *
 * holders shall not be used in advertising or otherwise to promote the     *
 * sale, use or other dealings in this Software without prior written       *
 * authorization.                                                           *
 ****************************************************************************/

/****************************************************************************
 *  Author: Zeyd M. Ben-Halim <zmbenhal@netcom.com> 1992,1995               *
 *     and: Eric S. Raymond <esr@snark.thyrsus.com>                         *
 *     and: Thomas E. Dickey                        1996-on                 *
 *     and: Juergen Pfeifer                         2009                    *
 ****************************************************************************/

/*-----------------------------------------------------------------
 *
 *	lib_doupdate.c
 *
 * 	The routine doupdate() and its dependents.
 * 	All physical output is concentrated here (except _nc_outch()
 *	in lib_tputs.c).
 *
 *-----------------------------------------------------------------*/

#define NEW_PAIR_INTERNAL 1

#include <curses.priv.h>

#ifndef CUR
#define CUR SP_TERMTYPE
#endif

#if defined __HAIKU__ && defined __BEOS__
#undef __BEOS__
#endif

#ifdef __BEOS__
#undef false
#undef true
#include <OS.h>
#endif

#if defined(TRACE) && HAVE_SYS_TIMES_H && HAVE_TIMES
#define USE_TRACE_TIMES 1
#else
#define USE_TRACE_TIMES 0
#endif

#if HAVE_SYS_TIME_H && HAVE_SYS_TIME_SELECT
#include <sys/time.h>
#endif

#if USE_TRACE_TIMES
#include <sys/times.h>
#endif

#if USE_FUNC_POLL
#elif HAVE_SELECT
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#endif

#include <ctype.h>

MODULE_ID("$Id: tty_update.c,v 1.314 2022/07/23 22:12:59 tom Exp $")

/*
 * This define controls the line-breakout optimization.  Every once in a
 * while during screen refresh, we want to check for input and abort the
 * update if there's some waiting.  CHECK_INTERVAL controls the number of
 * changed lines to be emitted between input checks.
 *
 * Note: Input-check-and-abort is no longer done if the screen is being
 * updated from scratch.  This is a feature, not a bug.
 */
#define CHECK_INTERVAL	5

#define FILL_BCE(sp) (sp->_coloron && !sp->_default_color && !back_color_erase)

static const NCURSES_CH_T blankchar = NewChar(BLANK_TEXT);
static NCURSES_CH_T normal = NewChar(BLANK_TEXT);

/*
 * Enable checking to see if doupdate and friends are tracking the true
 * cursor position correctly.  NOTE: this is a debugging hack which will
 * work ONLY on ANSI-compatible terminals!
 */
/* #define POSITION_DEBUG */

static NCURSES_INLINE NCURSES_CH_T ClrBlank(NCURSES_SP_DCLx WINDOW *win);

#if NCURSES_SP_FUNCS
static int ClrBottom(SCREEN *, int total);
static void ClearScreen(SCREEN *, NCURSES_CH_T blank);
static void ClrUpdate(SCREEN *);
static void DelChar(SCREEN *, int count);
static void InsStr(SCREEN *, NCURSES_CH_T *line, int count);
static void TransformLine(SCREEN *, int const lineno);
#else
static int ClrBottom(int total);
static void ClearScreen(NCURSES_CH_T blank);
static void ClrUpdate(void);
static void DelChar(int count);
static void InsStr(NCURSES_CH_T *line, int count);
static void TransformLine(int const lineno);
#endif

#ifdef POSITION_DEBUG
/****************************************************************************
 *
 * Debugging code.  Only works on ANSI-standard terminals.
 *
 ****************************************************************************/

static void
position_check(NCURSES_SP_DCLx int expected_y, int expected_x, const char *legend)
/* check to see if the real cursor position matches the virtual */
{
    char buf[20];
    char *s;
    int y, x;

    if (!_nc_tracing || (expected_y < 0 && expected_x < 0))
	return;

    NCURSES_SP_NAME(_nc_flush) (NCURSES_SP_ARG);
    memset(buf, '\0', sizeof(buf));
    NCURSES_PUTP2_FLUSH("cpr", "\033[6n");	/* only works on ANSI-compatibles */
    *(s = buf) = 0;
    do {
	int ask = sizeof(buf) - 1 - (s - buf);
	int got = read(0, s, ask);
	if (got == 0)
	    break;
	s += got;
    } while (strchr(buf, 'R') == 0);
    _tracef("probe returned %s", _nc_visbuf(buf));

    /* try to interpret as a position report */
    if (sscanf(buf, "\033[%d;%dR", &y, &x) != 2) {
	_tracef("position probe failed in %s", legend);
    } else {
	if (expected_x < 0)
	    expected_x = x - 1;
	if (expected_y < 0)
	    expected_y = y - 1;
	if (y - 1 != expected_y || x - 1 != expected_x) {
	    NCURSES_SP_NAME(beep) (NCURSES_SP_ARG);
	    NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				    TIPARM_2("\033[%d;%dH",
					     expected_y + 1,
					     expected_x + 1),
				    1, NCURSES_SP_NAME(_nc_outch));
	    _tracef("position seen (%d, %d) doesn't match expected one (%d, %d) in %s",
		    y - 1, x - 1, expected_y, expected_x, legend);
	} else {
	    _tracef("position matches OK in %s", legend);
	}
    }
}
#else
#define position_check(expected_y, expected_x, legend)	/* nothing */
#endif /* POSITION_DEBUG */

/****************************************************************************
 *
 * Optimized update code
 *
 ****************************************************************************/

static NCURSES_INLINE void
GoTo(NCURSES_SP_DCLx int const row, int const col)
{
    TR(TRACE_MOVE, ("GoTo(%p, %d, %d) from (%d, %d)",
		    (void *) SP_PARM, row, col, SP_PARM->_cursrow, SP_PARM->_curscol));

    position_check(NCURSES_SP_ARGx
		   SP_PARM->_cursrow,
		   SP_PARM->_curscol, "GoTo");

    TINFO_MVCUR(NCURSES_SP_ARGx
		SP_PARM->_cursrow,
		SP_PARM->_curscol,
		row, col);
    position_check(NCURSES_SP_ARGx
		   SP_PARM->_cursrow,
		   SP_PARM->_curscol, "GoTo2");
}

//MLA-begin
#ifdef __MORPHOS__
#define wcwidth morphos_wcwidth
/*
 * Copyright (C) Fredrik Fornwall 2016.
 * Distributed under the MIT License.
 *
 * Implementation of wcwidth(3) as a C port of:
 * https://github.com/jquast/wcwidth
 *
 * Report issues at:
 * https://github.com/termux/wcwidth
 *
 * IMPORTANT:
 * Must be kept in sync with the following:
 * https://github.com/termux/termux-app/blob/master/terminal-emulator/src/main/java/com/termux/terminal/WcWidth.java
 * https://github.com/termux/libandroid-support
 * https://github.com/termux/termux-packages/tree/master/libandroid-support
 */

#include <stdbool.h>
#include <stdlib.h>

struct width_interval {
        int start;
        int end;
};

// From https://github.com/jquast/wcwidth/blob/master/wcwidth/table_zero.py
// at commit b29897e5a1b403a0e36f7fc991614981cbc42475 (2020-07-14):
static struct width_interval ZERO_WIDTH[] = {
        {0x00300, 0x0036f},  // Combining Grave Accent  ..Combining Latin Small Le
        {0x00483, 0x00489},  // Combining Cyrillic Titlo..Combining Cyrillic Milli
        {0x00591, 0x005bd},  // Hebrew Accent Etnahta   ..Hebrew Point Meteg
        {0x005bf, 0x005bf},  // Hebrew Point Rafe       ..Hebrew Point Rafe
        {0x005c1, 0x005c2},  // Hebrew Point Shin Dot   ..Hebrew Point Sin Dot
        {0x005c4, 0x005c5},  // Hebrew Mark Upper Dot   ..Hebrew Mark Lower Dot
        {0x005c7, 0x005c7},  // Hebrew Point Qamats Qata..Hebrew Point Qamats Qata
        {0x00610, 0x0061a},  // Arabic Sign Sallallahou ..Arabic Small Kasra
        {0x0064b, 0x0065f},  // Arabic Fathatan         ..Arabic Wavy Hamza Below
        {0x00670, 0x00670},  // Arabic Letter Superscrip..Arabic Letter Superscrip
        {0x006d6, 0x006dc},  // Arabic Small High Ligatu..Arabic Small High Seen
        {0x006df, 0x006e4},  // Arabic Small High Rounde..Arabic Small High Madda
        {0x006e7, 0x006e8},  // Arabic Small High Yeh   ..Arabic Small High Noon
        {0x006ea, 0x006ed},  // Arabic Empty Centre Low ..Arabic Small Low Meem
        {0x00711, 0x00711},  // Syriac Letter Superscrip..Syriac Letter Superscrip
        {0x00730, 0x0074a},  // Syriac Pthaha Above     ..Syriac Barrekh
        {0x007a6, 0x007b0},  // Thaana Abafili          ..Thaana Sukun
        {0x007eb, 0x007f3},  // Nko Combining Short High..Nko Combining Double Dot
        {0x007fd, 0x007fd},  // Nko Dantayalan          ..Nko Dantayalan
        {0x00816, 0x00819},  // Samaritan Mark In       ..Samaritan Mark Dagesh
        {0x0081b, 0x00823},  // Samaritan Mark Epentheti..Samaritan Vowel Sign A
        {0x00825, 0x00827},  // Samaritan Vowel Sign Sho..Samaritan Vowel Sign U
        {0x00829, 0x0082d},  // Samaritan Vowel Sign Lon..Samaritan Mark Nequdaa
        {0x00859, 0x0085b},  // Mandaic Affrication Mark..Mandaic Gemination Mark
        {0x008d3, 0x008e1},  // Arabic Small Low Waw    ..Arabic Small High Sign S
        {0x008e3, 0x00902},  // Arabic Turned Damma Belo..Devanagari Sign Anusvara
        {0x0093a, 0x0093a},  // Devanagari Vowel Sign Oe..Devanagari Vowel Sign Oe
        {0x0093c, 0x0093c},  // Devanagari Sign Nukta   ..Devanagari Sign Nukta
        {0x00941, 0x00948},  // Devanagari Vowel Sign U ..Devanagari Vowel Sign Ai
        {0x0094d, 0x0094d},  // Devanagari Sign Virama  ..Devanagari Sign Virama
        {0x00951, 0x00957},  // Devanagari Stress Sign U..Devanagari Vowel Sign Uu
        {0x00962, 0x00963},  // Devanagari Vowel Sign Vo..Devanagari Vowel Sign Vo
        {0x00981, 0x00981},  // Bengali Sign Candrabindu..Bengali Sign Candrabindu
        {0x009bc, 0x009bc},  // Bengali Sign Nukta      ..Bengali Sign Nukta
        {0x009c1, 0x009c4},  // Bengali Vowel Sign U    ..Bengali Vowel Sign Vocal
        {0x009cd, 0x009cd},  // Bengali Sign Virama     ..Bengali Sign Virama
        {0x009e2, 0x009e3},  // Bengali Vowel Sign Vocal..Bengali Vowel Sign Vocal
        {0x009fe, 0x009fe},  // Bengali Sandhi Mark     ..Bengali Sandhi Mark
        {0x00a01, 0x00a02},  // Gurmukhi Sign Adak Bindi..Gurmukhi Sign Bindi
        {0x00a3c, 0x00a3c},  // Gurmukhi Sign Nukta     ..Gurmukhi Sign Nukta
        {0x00a41, 0x00a42},  // Gurmukhi Vowel Sign U   ..Gurmukhi Vowel Sign Uu
        {0x00a47, 0x00a48},  // Gurmukhi Vowel Sign Ee  ..Gurmukhi Vowel Sign Ai
        {0x00a4b, 0x00a4d},  // Gurmukhi Vowel Sign Oo  ..Gurmukhi Sign Virama
        {0x00a51, 0x00a51},  // Gurmukhi Sign Udaat     ..Gurmukhi Sign Udaat
        {0x00a70, 0x00a71},  // Gurmukhi Tippi          ..Gurmukhi Addak
        {0x00a75, 0x00a75},  // Gurmukhi Sign Yakash    ..Gurmukhi Sign Yakash
        {0x00a81, 0x00a82},  // Gujarati Sign Candrabind..Gujarati Sign Anusvara
        {0x00abc, 0x00abc},  // Gujarati Sign Nukta     ..Gujarati Sign Nukta
        {0x00ac1, 0x00ac5},  // Gujarati Vowel Sign U   ..Gujarati Vowel Sign Cand
        {0x00ac7, 0x00ac8},  // Gujarati Vowel Sign E   ..Gujarati Vowel Sign Ai
        {0x00acd, 0x00acd},  // Gujarati Sign Virama    ..Gujarati Sign Virama
        {0x00ae2, 0x00ae3},  // Gujarati Vowel Sign Voca..Gujarati Vowel Sign Voca
        {0x00afa, 0x00aff},  // Gujarati Sign Sukun     ..Gujarati Sign Two-circle
        {0x00b01, 0x00b01},  // Oriya Sign Candrabindu  ..Oriya Sign Candrabindu
        {0x00b3c, 0x00b3c},  // Oriya Sign Nukta        ..Oriya Sign Nukta
        {0x00b3f, 0x00b3f},  // Oriya Vowel Sign I      ..Oriya Vowel Sign I
        {0x00b41, 0x00b44},  // Oriya Vowel Sign U      ..Oriya Vowel Sign Vocalic
        {0x00b4d, 0x00b4d},  // Oriya Sign Virama       ..Oriya Sign Virama
        {0x00b55, 0x00b56},  // (nil)                   ..Oriya Ai Length Mark
        {0x00b62, 0x00b63},  // Oriya Vowel Sign Vocalic..Oriya Vowel Sign Vocalic
        {0x00b82, 0x00b82},  // Tamil Sign Anusvara     ..Tamil Sign Anusvara
        {0x00bc0, 0x00bc0},  // Tamil Vowel Sign Ii     ..Tamil Vowel Sign Ii
        {0x00bcd, 0x00bcd},  // Tamil Sign Virama       ..Tamil Sign Virama
        {0x00c00, 0x00c00},  // Telugu Sign Combining Ca..Telugu Sign Combining Ca
        {0x00c04, 0x00c04},  // Telugu Sign Combining An..Telugu Sign Combining An
        {0x00c3e, 0x00c40},  // Telugu Vowel Sign Aa    ..Telugu Vowel Sign Ii
        {0x00c46, 0x00c48},  // Telugu Vowel Sign E     ..Telugu Vowel Sign Ai
        {0x00c4a, 0x00c4d},  // Telugu Vowel Sign O     ..Telugu Sign Virama
        {0x00c55, 0x00c56},  // Telugu Length Mark      ..Telugu Ai Length Mark
        {0x00c62, 0x00c63},  // Telugu Vowel Sign Vocali..Telugu Vowel Sign Vocali
        {0x00c81, 0x00c81},  // Kannada Sign Candrabindu..Kannada Sign Candrabindu
        {0x00cbc, 0x00cbc},  // Kannada Sign Nukta      ..Kannada Sign Nukta
        {0x00cbf, 0x00cbf},  // Kannada Vowel Sign I    ..Kannada Vowel Sign I
        {0x00cc6, 0x00cc6},  // Kannada Vowel Sign E    ..Kannada Vowel Sign E
        {0x00ccc, 0x00ccd},  // Kannada Vowel Sign Au   ..Kannada Sign Virama
        {0x00ce2, 0x00ce3},  // Kannada Vowel Sign Vocal..Kannada Vowel Sign Vocal
        {0x00d00, 0x00d01},  // Malayalam Sign Combining..Malayalam Sign Candrabin
        {0x00d3b, 0x00d3c},  // Malayalam Sign Vertical ..Malayalam Sign Circular
        {0x00d41, 0x00d44},  // Malayalam Vowel Sign U  ..Malayalam Vowel Sign Voc
        {0x00d4d, 0x00d4d},  // Malayalam Sign Virama   ..Malayalam Sign Virama
        {0x00d62, 0x00d63},  // Malayalam Vowel Sign Voc..Malayalam Vowel Sign Voc
        {0x00d81, 0x00d81},  // (nil)                   ..(nil)
        {0x00dca, 0x00dca},  // Sinhala Sign Al-lakuna  ..Sinhala Sign Al-lakuna
        {0x00dd2, 0x00dd4},  // Sinhala Vowel Sign Ketti..Sinhala Vowel Sign Ketti
        {0x00dd6, 0x00dd6},  // Sinhala Vowel Sign Diga ..Sinhala Vowel Sign Diga
        {0x00e31, 0x00e31},  // Thai Character Mai Han-a..Thai Character Mai Han-a
        {0x00e34, 0x00e3a},  // Thai Character Sara I   ..Thai Character Phinthu
        {0x00e47, 0x00e4e},  // Thai Character Maitaikhu..Thai Character Yamakkan
        {0x00eb1, 0x00eb1},  // Lao Vowel Sign Mai Kan  ..Lao Vowel Sign Mai Kan
        {0x00eb4, 0x00ebc},  // Lao Vowel Sign I        ..Lao Semivowel Sign Lo
        {0x00ec8, 0x00ecd},  // Lao Tone Mai Ek         ..Lao Niggahita
        {0x00f18, 0x00f19},  // Tibetan Astrological Sig..Tibetan Astrological Sig
        {0x00f35, 0x00f35},  // Tibetan Mark Ngas Bzung ..Tibetan Mark Ngas Bzung
        {0x00f37, 0x00f37},  // Tibetan Mark Ngas Bzung ..Tibetan Mark Ngas Bzung
        {0x00f39, 0x00f39},  // Tibetan Mark Tsa -phru  ..Tibetan Mark Tsa -phru
        {0x00f71, 0x00f7e},  // Tibetan Vowel Sign Aa   ..Tibetan Sign Rjes Su Nga
        {0x00f80, 0x00f84},  // Tibetan Vowel Sign Rever..Tibetan Mark Halanta
        {0x00f86, 0x00f87},  // Tibetan Sign Lci Rtags  ..Tibetan Sign Yang Rtags
        {0x00f8d, 0x00f97},  // Tibetan Subjoined Sign L..Tibetan Subjoined Letter
        {0x00f99, 0x00fbc},  // Tibetan Subjoined Letter..Tibetan Subjoined Letter
        {0x00fc6, 0x00fc6},  // Tibetan Symbol Padma Gda..Tibetan Symbol Padma Gda
        {0x0102d, 0x01030},  // Myanmar Vowel Sign I    ..Myanmar Vowel Sign Uu
        {0x01032, 0x01037},  // Myanmar Vowel Sign Ai   ..Myanmar Sign Dot Below
        {0x01039, 0x0103a},  // Myanmar Sign Virama     ..Myanmar Sign Asat
        {0x0103d, 0x0103e},  // Myanmar Consonant Sign M..Myanmar Consonant Sign M
        {0x01058, 0x01059},  // Myanmar Vowel Sign Vocal..Myanmar Vowel Sign Vocal
        {0x0105e, 0x01060},  // Myanmar Consonant Sign M..Myanmar Consonant Sign M
        {0x01071, 0x01074},  // Myanmar Vowel Sign Geba ..Myanmar Vowel Sign Kayah
        {0x01082, 0x01082},  // Myanmar Consonant Sign S..Myanmar Consonant Sign S
        {0x01085, 0x01086},  // Myanmar Vowel Sign Shan ..Myanmar Vowel Sign Shan
        {0x0108d, 0x0108d},  // Myanmar Sign Shan Counci..Myanmar Sign Shan Counci
        {0x0109d, 0x0109d},  // Myanmar Vowel Sign Aiton..Myanmar Vowel Sign Aiton
        {0x0135d, 0x0135f},  // Ethiopic Combining Gemin..Ethiopic Combining Gemin
        {0x01712, 0x01714},  // Tagalog Vowel Sign I    ..Tagalog Sign Virama
        {0x01732, 0x01734},  // Hanunoo Vowel Sign I    ..Hanunoo Sign Pamudpod
        {0x01752, 0x01753},  // Buhid Vowel Sign I      ..Buhid Vowel Sign U
        {0x01772, 0x01773},  // Tagbanwa Vowel Sign I   ..Tagbanwa Vowel Sign U
        {0x017b4, 0x017b5},  // Khmer Vowel Inherent Aq ..Khmer Vowel Inherent Aa
        {0x017b7, 0x017bd},  // Khmer Vowel Sign I      ..Khmer Vowel Sign Ua
        {0x017c6, 0x017c6},  // Khmer Sign Nikahit      ..Khmer Sign Nikahit
        {0x017c9, 0x017d3},  // Khmer Sign Muusikatoan  ..Khmer Sign Bathamasat
        {0x017dd, 0x017dd},  // Khmer Sign Atthacan     ..Khmer Sign Atthacan
        {0x0180b, 0x0180d},  // Mongolian Free Variation..Mongolian Free Variation
        {0x01885, 0x01886},  // Mongolian Letter Ali Gal..Mongolian Letter Ali Gal
        {0x018a9, 0x018a9},  // Mongolian Letter Ali Gal..Mongolian Letter Ali Gal
        {0x01920, 0x01922},  // Limbu Vowel Sign A      ..Limbu Vowel Sign U
        {0x01927, 0x01928},  // Limbu Vowel Sign E      ..Limbu Vowel Sign O
        {0x01932, 0x01932},  // Limbu Small Letter Anusv..Limbu Small Letter Anusv
        {0x01939, 0x0193b},  // Limbu Sign Mukphreng    ..Limbu Sign Sa-i
        {0x01a17, 0x01a18},  // Buginese Vowel Sign I   ..Buginese Vowel Sign U
        {0x01a1b, 0x01a1b},  // Buginese Vowel Sign Ae  ..Buginese Vowel Sign Ae
        {0x01a56, 0x01a56},  // Tai Tham Consonant Sign ..Tai Tham Consonant Sign
        {0x01a58, 0x01a5e},  // Tai Tham Sign Mai Kang L..Tai Tham Consonant Sign
        {0x01a60, 0x01a60},  // Tai Tham Sign Sakot     ..Tai Tham Sign Sakot
        {0x01a62, 0x01a62},  // Tai Tham Vowel Sign Mai ..Tai Tham Vowel Sign Mai
        {0x01a65, 0x01a6c},  // Tai Tham Vowel Sign I   ..Tai Tham Vowel Sign Oa B
        {0x01a73, 0x01a7c},  // Tai Tham Vowel Sign Oa A..Tai Tham Sign Khuen-lue
        {0x01a7f, 0x01a7f},  // Tai Tham Combining Crypt..Tai Tham Combining Crypt
        {0x01ab0, 0x01ac0},  // Combining Doubled Circum..(nil)
        {0x01b00, 0x01b03},  // Balinese Sign Ulu Ricem ..Balinese Sign Surang
        {0x01b34, 0x01b34},  // Balinese Sign Rerekan   ..Balinese Sign Rerekan
        {0x01b36, 0x01b3a},  // Balinese Vowel Sign Ulu ..Balinese Vowel Sign Ra R
        {0x01b3c, 0x01b3c},  // Balinese Vowel Sign La L..Balinese Vowel Sign La L
        {0x01b42, 0x01b42},  // Balinese Vowel Sign Pepe..Balinese Vowel Sign Pepe
        {0x01b6b, 0x01b73},  // Balinese Musical Symbol ..Balinese Musical Symbol
        {0x01b80, 0x01b81},  // Sundanese Sign Panyecek ..Sundanese Sign Panglayar
        {0x01ba2, 0x01ba5},  // Sundanese Consonant Sign..Sundanese Vowel Sign Pan
        {0x01ba8, 0x01ba9},  // Sundanese Vowel Sign Pam..Sundanese Vowel Sign Pan
        {0x01bab, 0x01bad},  // Sundanese Sign Virama   ..Sundanese Consonant Sign
        {0x01be6, 0x01be6},  // Batak Sign Tompi        ..Batak Sign Tompi
        {0x01be8, 0x01be9},  // Batak Vowel Sign Pakpak ..Batak Vowel Sign Ee
        {0x01bed, 0x01bed},  // Batak Vowel Sign Karo O ..Batak Vowel Sign Karo O
        {0x01bef, 0x01bf1},  // Batak Vowel Sign U For S..Batak Consonant Sign H
        {0x01c2c, 0x01c33},  // Lepcha Vowel Sign E     ..Lepcha Consonant Sign T
        {0x01c36, 0x01c37},  // Lepcha Sign Ran         ..Lepcha Sign Nukta
        {0x01cd0, 0x01cd2},  // Vedic Tone Karshana     ..Vedic Tone Prenkha
        {0x01cd4, 0x01ce0},  // Vedic Sign Yajurvedic Mi..Vedic Tone Rigvedic Kash
        {0x01ce2, 0x01ce8},  // Vedic Sign Visarga Svari..Vedic Sign Visarga Anuda
        {0x01ced, 0x01ced},  // Vedic Sign Tiryak       ..Vedic Sign Tiryak
        {0x01cf4, 0x01cf4},  // Vedic Tone Candra Above ..Vedic Tone Candra Above
        {0x01cf8, 0x01cf9},  // Vedic Tone Ring Above   ..Vedic Tone Double Ring A
        {0x01dc0, 0x01df9},  // Combining Dotted Grave A..Combining Wide Inverted
        {0x01dfb, 0x01dff},  // Combining Deletion Mark ..Combining Right Arrowhea
        {0x020d0, 0x020f0},  // Combining Left Harpoon A..Combining Asterisk Above
        {0x02cef, 0x02cf1},  // Coptic Combining Ni Abov..Coptic Combining Spiritu
        {0x02d7f, 0x02d7f},  // Tifinagh Consonant Joine..Tifinagh Consonant Joine
        {0x02de0, 0x02dff},  // Combining Cyrillic Lette..Combining Cyrillic Lette
        {0x0302a, 0x0302d},  // Ideographic Level Tone M..Ideographic Entering Ton
        {0x03099, 0x0309a},  // Combining Katakana-hirag..Combining Katakana-hirag
        {0x0a66f, 0x0a672},  // Combining Cyrillic Vzmet..Combining Cyrillic Thous
        {0x0a674, 0x0a67d},  // Combining Cyrillic Lette..Combining Cyrillic Payer
        {0x0a69e, 0x0a69f},  // Combining Cyrillic Lette..Combining Cyrillic Lette
        {0x0a6f0, 0x0a6f1},  // Bamum Combining Mark Koq..Bamum Combining Mark Tuk
        {0x0a802, 0x0a802},  // Syloti Nagri Sign Dvisva..Syloti Nagri Sign Dvisva
        {0x0a806, 0x0a806},  // Syloti Nagri Sign Hasant..Syloti Nagri Sign Hasant
        {0x0a80b, 0x0a80b},  // Syloti Nagri Sign Anusva..Syloti Nagri Sign Anusva
        {0x0a825, 0x0a826},  // Syloti Nagri Vowel Sign ..Syloti Nagri Vowel Sign
        {0x0a82c, 0x0a82c},  // (nil)                   ..(nil)
        {0x0a8c4, 0x0a8c5},  // Saurashtra Sign Virama  ..Saurashtra Sign Candrabi
        {0x0a8e0, 0x0a8f1},  // Combining Devanagari Dig..Combining Devanagari Sig
        {0x0a8ff, 0x0a8ff},  // Devanagari Vowel Sign Ay..Devanagari Vowel Sign Ay
        {0x0a926, 0x0a92d},  // Kayah Li Vowel Ue       ..Kayah Li Tone Calya Plop
        {0x0a947, 0x0a951},  // Rejang Vowel Sign I     ..Rejang Consonant Sign R
        {0x0a980, 0x0a982},  // Javanese Sign Panyangga ..Javanese Sign Layar
        {0x0a9b3, 0x0a9b3},  // Javanese Sign Cecak Telu..Javanese Sign Cecak Telu
        {0x0a9b6, 0x0a9b9},  // Javanese Vowel Sign Wulu..Javanese Vowel Sign Suku
        {0x0a9bc, 0x0a9bd},  // Javanese Vowel Sign Pepe..Javanese Consonant Sign
        {0x0a9e5, 0x0a9e5},  // Myanmar Sign Shan Saw   ..Myanmar Sign Shan Saw
        {0x0aa29, 0x0aa2e},  // Cham Vowel Sign Aa      ..Cham Vowel Sign Oe
        {0x0aa31, 0x0aa32},  // Cham Vowel Sign Au      ..Cham Vowel Sign Ue
        {0x0aa35, 0x0aa36},  // Cham Consonant Sign La  ..Cham Consonant Sign Wa
        {0x0aa43, 0x0aa43},  // Cham Consonant Sign Fina..Cham Consonant Sign Fina
        {0x0aa4c, 0x0aa4c},  // Cham Consonant Sign Fina..Cham Consonant Sign Fina
        {0x0aa7c, 0x0aa7c},  // Myanmar Sign Tai Laing T..Myanmar Sign Tai Laing T
        {0x0aab0, 0x0aab0},  // Tai Viet Mai Kang       ..Tai Viet Mai Kang
        {0x0aab2, 0x0aab4},  // Tai Viet Vowel I        ..Tai Viet Vowel U
        {0x0aab7, 0x0aab8},  // Tai Viet Mai Khit       ..Tai Viet Vowel Ia
        {0x0aabe, 0x0aabf},  // Tai Viet Vowel Am       ..Tai Viet Tone Mai Ek
        {0x0aac1, 0x0aac1},  // Tai Viet Tone Mai Tho   ..Tai Viet Tone Mai Tho
        {0x0aaec, 0x0aaed},  // Meetei Mayek Vowel Sign ..Meetei Mayek Vowel Sign
        {0x0aaf6, 0x0aaf6},  // Meetei Mayek Virama     ..Meetei Mayek Virama
        {0x0abe5, 0x0abe5},  // Meetei Mayek Vowel Sign ..Meetei Mayek Vowel Sign
        {0x0abe8, 0x0abe8},  // Meetei Mayek Vowel Sign ..Meetei Mayek Vowel Sign
        {0x0abed, 0x0abed},  // Meetei Mayek Apun Iyek  ..Meetei Mayek Apun Iyek
        {0x0fb1e, 0x0fb1e},  // Hebrew Point Judeo-spani..Hebrew Point Judeo-spani
        {0x0fe00, 0x0fe0f},  // Variation Selector-1    ..Variation Selector-16
        {0x0fe20, 0x0fe2f},  // Combining Ligature Left ..Combining Cyrillic Titlo
        {0x101fd, 0x101fd},  // Phaistos Disc Sign Combi..Phaistos Disc Sign Combi
        {0x102e0, 0x102e0},  // Coptic Epact Thousands M..Coptic Epact Thousands M
        {0x10376, 0x1037a},  // Combining Old Permic Let..Combining Old Permic Let
        {0x10a01, 0x10a03},  // Kharoshthi Vowel Sign I ..Kharoshthi Vowel Sign Vo
        {0x10a05, 0x10a06},  // Kharoshthi Vowel Sign E ..Kharoshthi Vowel Sign O
        {0x10a0c, 0x10a0f},  // Kharoshthi Vowel Length ..Kharoshthi Sign Visarga
        {0x10a38, 0x10a3a},  // Kharoshthi Sign Bar Abov..Kharoshthi Sign Dot Belo
        {0x10a3f, 0x10a3f},  // Kharoshthi Virama       ..Kharoshthi Virama
        {0x10ae5, 0x10ae6},  // Manichaean Abbreviation ..Manichaean Abbreviation
        {0x10d24, 0x10d27},  // Hanifi Rohingya Sign Har..Hanifi Rohingya Sign Tas
        {0x10eab, 0x10eac},  // (nil)                   ..(nil)
        {0x10f46, 0x10f50},  // Sogdian Combining Dot Be..Sogdian Combining Stroke
        {0x11001, 0x11001},  // Brahmi Sign Anusvara    ..Brahmi Sign Anusvara
        {0x11038, 0x11046},  // Brahmi Vowel Sign Aa    ..Brahmi Virama
        {0x1107f, 0x11081},  // Brahmi Number Joiner    ..Kaithi Sign Anusvara
        {0x110b3, 0x110b6},  // Kaithi Vowel Sign U     ..Kaithi Vowel Sign Ai
        {0x110b9, 0x110ba},  // Kaithi Sign Virama      ..Kaithi Sign Nukta
        {0x11100, 0x11102},  // Chakma Sign Candrabindu ..Chakma Sign Visarga
        {0x11127, 0x1112b},  // Chakma Vowel Sign A     ..Chakma Vowel Sign Uu
        {0x1112d, 0x11134},  // Chakma Vowel Sign Ai    ..Chakma Maayyaa
        {0x11173, 0x11173},  // Mahajani Sign Nukta     ..Mahajani Sign Nukta
        {0x11180, 0x11181},  // Sharada Sign Candrabindu..Sharada Sign Anusvara
        {0x111b6, 0x111be},  // Sharada Vowel Sign U    ..Sharada Vowel Sign O
        {0x111c9, 0x111cc},  // Sharada Sandhi Mark     ..Sharada Extra Short Vowe
        {0x111cf, 0x111cf},  // (nil)                   ..(nil)
        {0x1122f, 0x11231},  // Khojki Vowel Sign U     ..Khojki Vowel Sign Ai
        {0x11234, 0x11234},  // Khojki Sign Anusvara    ..Khojki Sign Anusvara
        {0x11236, 0x11237},  // Khojki Sign Nukta       ..Khojki Sign Shadda
        {0x1123e, 0x1123e},  // Khojki Sign Sukun       ..Khojki Sign Sukun
        {0x112df, 0x112df},  // Khudawadi Sign Anusvara ..Khudawadi Sign Anusvara
        {0x112e3, 0x112ea},  // Khudawadi Vowel Sign U  ..Khudawadi Sign Virama
        {0x11300, 0x11301},  // Grantha Sign Combining A..Grantha Sign Candrabindu
        {0x1133b, 0x1133c},  // Combining Bindu Below   ..Grantha Sign Nukta
        {0x11340, 0x11340},  // Grantha Vowel Sign Ii   ..Grantha Vowel Sign Ii
        {0x11366, 0x1136c},  // Combining Grantha Digit ..Combining Grantha Digit
        {0x11370, 0x11374},  // Combining Grantha Letter..Combining Grantha Letter
        {0x11438, 0x1143f},  // Newa Vowel Sign U       ..Newa Vowel Sign Ai
        {0x11442, 0x11444},  // Newa Sign Virama        ..Newa Sign Anusvara
        {0x11446, 0x11446},  // Newa Sign Nukta         ..Newa Sign Nukta
        {0x1145e, 0x1145e},  // Newa Sandhi Mark        ..Newa Sandhi Mark
        {0x114b3, 0x114b8},  // Tirhuta Vowel Sign U    ..Tirhuta Vowel Sign Vocal
        {0x114ba, 0x114ba},  // Tirhuta Vowel Sign Short..Tirhuta Vowel Sign Short
        {0x114bf, 0x114c0},  // Tirhuta Sign Candrabindu..Tirhuta Sign Anusvara
        {0x114c2, 0x114c3},  // Tirhuta Sign Virama     ..Tirhuta Sign Nukta
        {0x115b2, 0x115b5},  // Siddham Vowel Sign U    ..Siddham Vowel Sign Vocal
        {0x115bc, 0x115bd},  // Siddham Sign Candrabindu..Siddham Sign Anusvara
        {0x115bf, 0x115c0},  // Siddham Sign Virama     ..Siddham Sign Nukta
        {0x115dc, 0x115dd},  // Siddham Vowel Sign Alter..Siddham Vowel Sign Alter
        {0x11633, 0x1163a},  // Modi Vowel Sign U       ..Modi Vowel Sign Ai
        {0x1163d, 0x1163d},  // Modi Sign Anusvara      ..Modi Sign Anusvara
        {0x1163f, 0x11640},  // Modi Sign Virama        ..Modi Sign Ardhacandra
        {0x116ab, 0x116ab},  // Takri Sign Anusvara     ..Takri Sign Anusvara
        {0x116ad, 0x116ad},  // Takri Vowel Sign Aa     ..Takri Vowel Sign Aa
        {0x116b0, 0x116b5},  // Takri Vowel Sign U      ..Takri Vowel Sign Au
        {0x116b7, 0x116b7},  // Takri Sign Nukta        ..Takri Sign Nukta
        {0x1171d, 0x1171f},  // Ahom Consonant Sign Medi..Ahom Consonant Sign Medi
        {0x11722, 0x11725},  // Ahom Vowel Sign I       ..Ahom Vowel Sign Uu
        {0x11727, 0x1172b},  // Ahom Vowel Sign Aw      ..Ahom Sign Killer
        {0x1182f, 0x11837},  // Dogra Vowel Sign U      ..Dogra Sign Anusvara
        {0x11839, 0x1183a},  // Dogra Sign Virama       ..Dogra Sign Nukta
        {0x1193b, 0x1193c},  // (nil)                   ..(nil)
        {0x1193e, 0x1193e},  // (nil)                   ..(nil)
        {0x11943, 0x11943},  // (nil)                   ..(nil)
        {0x119d4, 0x119d7},  // Nandinagari Vowel Sign U..Nandinagari Vowel Sign V
        {0x119da, 0x119db},  // Nandinagari Vowel Sign E..Nandinagari Vowel Sign A
        {0x119e0, 0x119e0},  // Nandinagari Sign Virama ..Nandinagari Sign Virama
        {0x11a01, 0x11a0a},  // Zanabazar Square Vowel S..Zanabazar Square Vowel L
        {0x11a33, 0x11a38},  // Zanabazar Square Final C..Zanabazar Square Sign An
        {0x11a3b, 0x11a3e},  // Zanabazar Square Cluster..Zanabazar Square Cluster
        {0x11a47, 0x11a47},  // Zanabazar Square Subjoin..Zanabazar Square Subjoin
        {0x11a51, 0x11a56},  // Soyombo Vowel Sign I    ..Soyombo Vowel Sign Oe
        {0x11a59, 0x11a5b},  // Soyombo Vowel Sign Vocal..Soyombo Vowel Length Mar
        {0x11a8a, 0x11a96},  // Soyombo Final Consonant ..Soyombo Sign Anusvara
        {0x11a98, 0x11a99},  // Soyombo Gemination Mark ..Soyombo Subjoiner
        {0x11c30, 0x11c36},  // Bhaiksuki Vowel Sign I  ..Bhaiksuki Vowel Sign Voc
        {0x11c38, 0x11c3d},  // Bhaiksuki Vowel Sign E  ..Bhaiksuki Sign Anusvara
        {0x11c3f, 0x11c3f},  // Bhaiksuki Sign Virama   ..Bhaiksuki Sign Virama
        {0x11c92, 0x11ca7},  // Marchen Subjoined Letter..Marchen Subjoined Letter
        {0x11caa, 0x11cb0},  // Marchen Subjoined Letter..Marchen Vowel Sign Aa
        {0x11cb2, 0x11cb3},  // Marchen Vowel Sign U    ..Marchen Vowel Sign E
        {0x11cb5, 0x11cb6},  // Marchen Sign Anusvara   ..Marchen Sign Candrabindu
        {0x11d31, 0x11d36},  // Masaram Gondi Vowel Sign..Masaram Gondi Vowel Sign
        {0x11d3a, 0x11d3a},  // Masaram Gondi Vowel Sign..Masaram Gondi Vowel Sign
        {0x11d3c, 0x11d3d},  // Masaram Gondi Vowel Sign..Masaram Gondi Vowel Sign
        {0x11d3f, 0x11d45},  // Masaram Gondi Vowel Sign..Masaram Gondi Virama
        {0x11d47, 0x11d47},  // Masaram Gondi Ra-kara   ..Masaram Gondi Ra-kara
        {0x11d90, 0x11d91},  // Gunjala Gondi Vowel Sign..Gunjala Gondi Vowel Sign
        {0x11d95, 0x11d95},  // Gunjala Gondi Sign Anusv..Gunjala Gondi Sign Anusv
        {0x11d97, 0x11d97},  // Gunjala Gondi Virama    ..Gunjala Gondi Virama
        {0x11ef3, 0x11ef4},  // Makasar Vowel Sign I    ..Makasar Vowel Sign U
        {0x16af0, 0x16af4},  // Bassa Vah Combining High..Bassa Vah Combining High
        {0x16b30, 0x16b36},  // Pahawh Hmong Mark Cim Tu..Pahawh Hmong Mark Cim Ta
        {0x16f4f, 0x16f4f},  // Miao Sign Consonant Modi..Miao Sign Consonant Modi
        {0x16f8f, 0x16f92},  // Miao Tone Right         ..Miao Tone Below
        {0x16fe4, 0x16fe4},  // (nil)                   ..(nil)
        {0x1bc9d, 0x1bc9e},  // Duployan Thick Letter Se..Duployan Double Mark
        {0x1d167, 0x1d169},  // Musical Symbol Combining..Musical Symbol Combining
        {0x1d17b, 0x1d182},  // Musical Symbol Combining..Musical Symbol Combining
        {0x1d185, 0x1d18b},  // Musical Symbol Combining..Musical Symbol Combining
        {0x1d1aa, 0x1d1ad},  // Musical Symbol Combining..Musical Symbol Combining
        {0x1d242, 0x1d244},  // Combining Greek Musical ..Combining Greek Musical
        {0x1da00, 0x1da36},  // Signwriting Head Rim    ..Signwriting Air Sucking
        {0x1da3b, 0x1da6c},  // Signwriting Mouth Closed..Signwriting Excitement
        {0x1da75, 0x1da75},  // Signwriting Upper Body T..Signwriting Upper Body T
        {0x1da84, 0x1da84},  // Signwriting Location Hea..Signwriting Location Hea
        {0x1da9b, 0x1da9f},  // Signwriting Fill Modifie..Signwriting Fill Modifie
        {0x1daa1, 0x1daaf},  // Signwriting Rotation Mod..Signwriting Rotation Mod
        {0x1e000, 0x1e006},  // Combining Glagolitic Let..Combining Glagolitic Let
        {0x1e008, 0x1e018},  // Combining Glagolitic Let..Combining Glagolitic Let
        {0x1e01b, 0x1e021},  // Combining Glagolitic Let..Combining Glagolitic Let
        {0x1e023, 0x1e024},  // Combining Glagolitic Let..Combining Glagolitic Let
        {0x1e026, 0x1e02a},  // Combining Glagolitic Let..Combining Glagolitic Let
        {0x1e130, 0x1e136},  // Nyiakeng Puachue Hmong T..Nyiakeng Puachue Hmong T
        {0x1e2ec, 0x1e2ef},  // Wancho Tone Tup         ..Wancho Tone Koini
        {0x1e8d0, 0x1e8d6},  // Mende Kikakui Combining ..Mende Kikakui Combining
        {0x1e944, 0x1e94a},  // Adlam Alif Lengthener   ..Adlam Nukta
        {0xe0100, 0xe01ef},  // Variation Selector-17   ..Variation Selector-256
};

// https://github.com/jquast/wcwidth/blob/master/wcwidth/table_wide.py
// at commit b29897e5a1b403a0e36f7fc991614981cbc42475 (2020-07-14):
static struct width_interval WIDE_EASTASIAN[] = {
        {0x01100, 0x0115f},  // Hangul Choseong Kiyeok  ..Hangul Choseong Filler
        {0x0231a, 0x0231b},  // Watch                   ..Hourglass
        {0x02329, 0x0232a},  // Left-pointing Angle Brac..Right-pointing Angle Bra
        {0x023e9, 0x023ec},  // Black Right-pointing Dou..Black Down-pointing Doub
        {0x023f0, 0x023f0},  // Alarm Clock             ..Alarm Clock
        {0x023f3, 0x023f3},  // Hourglass With Flowing S..Hourglass With Flowing S
        {0x025fd, 0x025fe},  // White Medium Small Squar..Black Medium Small Squar
        {0x02614, 0x02615},  // Umbrella With Rain Drops..Hot Beverage
        {0x02648, 0x02653},  // Aries                   ..Pisces
        {0x0267f, 0x0267f},  // Wheelchair Symbol       ..Wheelchair Symbol
        {0x02693, 0x02693},  // Anchor                  ..Anchor
        {0x026a1, 0x026a1},  // High Voltage Sign       ..High Voltage Sign
        {0x026aa, 0x026ab},  // Medium White Circle     ..Medium Black Circle
        {0x026bd, 0x026be},  // Soccer Ball             ..Baseball
        {0x026c4, 0x026c5},  // Snowman Without Snow    ..Sun Behind Cloud
        {0x026ce, 0x026ce},  // Ophiuchus               ..Ophiuchus
        {0x026d4, 0x026d4},  // No Entry                ..No Entry
        {0x026ea, 0x026ea},  // Church                  ..Church
        {0x026f2, 0x026f3},  // Fountain                ..Flag In Hole
        {0x026f5, 0x026f5},  // Sailboat                ..Sailboat
        {0x026fa, 0x026fa},  // Tent                    ..Tent
        {0x026fd, 0x026fd},  // Fuel Pump               ..Fuel Pump
        {0x02705, 0x02705},  // White Heavy Check Mark  ..White Heavy Check Mark
        {0x0270a, 0x0270b},  // Raised Fist             ..Raised Hand
        {0x02728, 0x02728},  // Sparkles                ..Sparkles
        {0x0274c, 0x0274c},  // Cross Mark              ..Cross Mark
        {0x0274e, 0x0274e},  // Negative Squared Cross M..Negative Squared Cross M
        {0x02753, 0x02755},  // Black Question Mark Orna..White Exclamation Mark O
        {0x02757, 0x02757},  // Heavy Exclamation Mark S..Heavy Exclamation Mark S
        {0x02795, 0x02797},  // Heavy Plus Sign         ..Heavy Division Sign
        {0x027b0, 0x027b0},  // Curly Loop              ..Curly Loop
        {0x027bf, 0x027bf},  // Double Curly Loop       ..Double Curly Loop
        {0x02b1b, 0x02b1c},  // Black Large Square      ..White Large Square
        {0x02b50, 0x02b50},  // White Medium Star       ..White Medium Star
        {0x02b55, 0x02b55},  // Heavy Large Circle      ..Heavy Large Circle
        {0x02e80, 0x02e99},  // Cjk Radical Repeat      ..Cjk Radical Rap
        {0x02e9b, 0x02ef3},  // Cjk Radical Choke       ..Cjk Radical C-simplified
        {0x02f00, 0x02fd5},  // Kangxi Radical One      ..Kangxi Radical Flute
        {0x02ff0, 0x02ffb},  // Ideographic Description ..Ideographic Description
        {0x03000, 0x0303e},  // Ideographic Space       ..Ideographic Variation In
        {0x03041, 0x03096},  // Hiragana Letter Small A ..Hiragana Letter Small Ke
        {0x03099, 0x030ff},  // Combining Katakana-hirag..Katakana Digraph Koto
        {0x03105, 0x0312f},  // Bopomofo Letter B       ..Bopomofo Letter Nn
        {0x03131, 0x0318e},  // Hangul Letter Kiyeok    ..Hangul Letter Araeae
        {0x03190, 0x031e3},  // Ideographic Annotation L..Cjk Stroke Q
        {0x031f0, 0x0321e},  // Katakana Letter Small Ku..Parenthesized Korean Cha
        {0x03220, 0x03247},  // Parenthesized Ideograph ..Circled Ideograph Koto
        {0x03250, 0x04dbf},  // Partnership Sign        ..(nil)
        {0x04e00, 0x0a48c},  // Cjk Unified Ideograph-4e..Yi Syllable Yyr
        {0x0a490, 0x0a4c6},  // Yi Radical Qot          ..Yi Radical Ke
        {0x0a960, 0x0a97c},  // Hangul Choseong Tikeut-m..Hangul Choseong Ssangyeo
        {0x0ac00, 0x0d7a3},  // Hangul Syllable Ga      ..Hangul Syllable Hih
        {0x0f900, 0x0faff},  // Cjk Compatibility Ideogr..(nil)
        {0x0fe10, 0x0fe19},  // Presentation Form For Ve..Presentation Form For Ve
        {0x0fe30, 0x0fe52},  // Presentation Form For Ve..Small Full Stop
        {0x0fe54, 0x0fe66},  // Small Semicolon         ..Small Equals Sign
        {0x0fe68, 0x0fe6b},  // Small Reverse Solidus   ..Small Commercial At
        {0x0ff01, 0x0ff60},  // Fullwidth Exclamation Ma..Fullwidth Right White Pa
        {0x0ffe0, 0x0ffe6},  // Fullwidth Cent Sign     ..Fullwidth Won Sign
        {0x16fe0, 0x16fe4},  // Tangut Iteration Mark   ..(nil)
        {0x16ff0, 0x16ff1},  // (nil)                   ..(nil)
        {0x17000, 0x187f7},  // (nil)                   ..(nil)
        {0x18800, 0x18cd5},  // Tangut Component-001    ..(nil)
        {0x18d00, 0x18d08},  // (nil)                   ..(nil)
        {0x1b000, 0x1b11e},  // Katakana Letter Archaic ..Hentaigana Letter N-mu-m
        {0x1b150, 0x1b152},  // Hiragana Letter Small Wi..Hiragana Letter Small Wo
        {0x1b164, 0x1b167},  // Katakana Letter Small Wi..Katakana Letter Small N
        {0x1b170, 0x1b2fb},  // Nushu Character-1b170   ..Nushu Character-1b2fb
        {0x1f004, 0x1f004},  // Mahjong Tile Red Dragon ..Mahjong Tile Red Dragon
        {0x1f0cf, 0x1f0cf},  // Playing Card Black Joker..Playing Card Black Joker
        {0x1f18e, 0x1f18e},  // Negative Squared Ab     ..Negative Squared Ab
        {0x1f191, 0x1f19a},  // Squared Cl              ..Squared Vs
        {0x1f200, 0x1f202},  // Square Hiragana Hoka    ..Squared Katakana Sa
        {0x1f210, 0x1f23b},  // Squared Cjk Unified Ideo..Squared Cjk Unified Ideo
        {0x1f240, 0x1f248},  // Tortoise Shell Bracketed..Tortoise Shell Bracketed
        {0x1f250, 0x1f251},  // Circled Ideograph Advant..Circled Ideograph Accept
        {0x1f260, 0x1f265},  // Rounded Symbol For Fu   ..Rounded Symbol For Cai
        {0x1f300, 0x1f320},  // Cyclone                 ..Shooting Star
        {0x1f32d, 0x1f335},  // Hot Dog                 ..Cactus
        {0x1f337, 0x1f37c},  // Tulip                   ..Baby Bottle
        {0x1f37e, 0x1f393},  // Bottle With Popping Cork..Graduation Cap
        {0x1f3a0, 0x1f3ca},  // Carousel Horse          ..Swimmer
        {0x1f3cf, 0x1f3d3},  // Cricket Bat And Ball    ..Table Tennis Paddle And
        {0x1f3e0, 0x1f3f0},  // House Building          ..European Castle
        {0x1f3f4, 0x1f3f4},  // Waving Black Flag       ..Waving Black Flag
        {0x1f3f8, 0x1f43e},  // Badminton Racquet And Sh..Paw Prints
        {0x1f440, 0x1f440},  // Eyes                    ..Eyes
        {0x1f442, 0x1f4fc},  // Ear                     ..Videocassette
        {0x1f4ff, 0x1f53d},  // Prayer Beads            ..Down-pointing Small Red
        {0x1f54b, 0x1f54e},  // Kaaba                   ..Menorah With Nine Branch
        {0x1f550, 0x1f567},  // Clock Face One Oclock   ..Clock Face Twelve-thirty
        {0x1f57a, 0x1f57a},  // Man Dancing             ..Man Dancing
        {0x1f595, 0x1f596},  // Reversed Hand With Middl..Raised Hand With Part Be
        {0x1f5a4, 0x1f5a4},  // Black Heart             ..Black Heart
        {0x1f5fb, 0x1f64f},  // Mount Fuji              ..Person With Folded Hands
        {0x1f680, 0x1f6c5},  // Rocket                  ..Left Luggage
        {0x1f6cc, 0x1f6cc},  // Sleeping Accommodation  ..Sleeping Accommodation
        {0x1f6d0, 0x1f6d2},  // Place Of Worship        ..Shopping Trolley
        {0x1f6d5, 0x1f6d7},  // Hindu Temple            ..(nil)
        {0x1f6eb, 0x1f6ec},  // Airplane Departure      ..Airplane Arriving
        {0x1f6f4, 0x1f6fc},  // Scooter                 ..(nil)
        {0x1f7e0, 0x1f7eb},  // Large Orange Circle     ..Large Brown Square
        {0x1f90c, 0x1f93a},  // (nil)                   ..Fencer
        {0x1f93c, 0x1f945},  // Wrestlers               ..Goal Net
        {0x1f947, 0x1f978},  // First Place Medal       ..(nil)
        {0x1f97a, 0x1f9cb},  // Face With Pleading Eyes ..(nil)
        {0x1f9cd, 0x1f9ff},  // Standing Person         ..Nazar Amulet
        {0x1fa70, 0x1fa74},  // Ballet Shoes            ..(nil)
        {0x1fa78, 0x1fa7a},  // Drop Of Blood           ..Stethoscope
        {0x1fa80, 0x1fa86},  // Yo-yo                   ..(nil)
        {0x1fa90, 0x1faa8},  // Ringed Planet           ..(nil)
        {0x1fab0, 0x1fab6},  // (nil)                   ..(nil)
        {0x1fac0, 0x1fac2},  // (nil)                   ..(nil)
        {0x1fad0, 0x1fad6},  // (nil)                   ..(nil)
        {0x20000, 0x2fffd},  // Cjk Unified Ideograph-20..(nil)
        {0x30000, 0x3fffd},  // (nil)                   ..(nil)
};

static bool intable(struct width_interval* table, int table_length, int c) {
        // First quick check for Latin1 etc. characters.
        if (c < table[0].start) return false;

        // Binary search in table.
        int bot = 0;
        int top = table_length - 1;
        while (top >= bot) {
                int mid = (bot + top) / 2;
                if (table[mid].end < c) {
                        bot = mid + 1;
                } else if (table[mid].start > c) {
                        top = mid - 1;
                } else {
                        return true;
                }
        }
        return false;
}

int wcwidth(wchar_t ucs) {
	// NOTE: created by hand, there isn't anything identifiable other than
	// general Cf category code to identify these, and some characters in Cf
	// category code are of non-zero width.
        if (ucs == 0 ||
                        ucs == 0x034F ||
                        (0x200B <= ucs && ucs <= 0x200F) ||
                        ucs == 0x2028 ||
                        ucs == 0x2029 ||
                        (0x202A <= ucs && ucs <= 0x202E) ||
                        (0x2060 <= ucs && ucs <= 0x2063)) {
                return 0;
        }

        // C0/C1 control characters.
        if (ucs < 32 || (0x07F <= ucs && ucs < 0x0A0)) return -1;

        // Combining characters with zero width.
        if (intable(ZERO_WIDTH, sizeof(ZERO_WIDTH)/sizeof(struct width_interval), ucs)) return 0;

        return intable(WIDE_EASTASIAN, sizeof(WIDE_EASTASIAN)/sizeof(struct width_interval), ucs) ? 2 : 1;
}
#endif
//MLA-end


#if !NCURSES_WCWIDTH_GRAPHICS
#define is_wacs_value(ch) (_nc_wacs_width(ch) == 1 && wcwidth(ch) > 1)
#endif /* !NCURSES_WCWIDTH_GRAPHICS */

static NCURSES_INLINE void
PutAttrChar(NCURSES_SP_DCLx CARG_CH_T ch)
{
    int chlen = 1;
    NCURSES_CH_T my_ch;
#if USE_WIDEC_SUPPORT
    PUTC_DATA;
#endif
    NCURSES_CH_T tilde;
    NCURSES_CH_T attr = CHDEREF(ch);

    TR(TRACE_CHARPUT, ("PutAttrChar(%s) at (%d, %d)",
		       _tracech_t(ch),
		       SP_PARM->_cursrow, SP_PARM->_curscol));
#if USE_WIDEC_SUPPORT
    /*
     * If this is not a valid character, there is nothing more to do.
     */
    if (isWidecExt(CHDEREF(ch))) {
	TR(TRACE_CHARPUT, ("...skip"));
	return;
    }
    /*
     * Determine the number of character cells which the 'ch' value will use
     * on the screen.  It should be at least one.
     */
    if ((chlen = _nc_wacs_width(CharOf(CHDEREF(ch)))) <= 0) {
	static const NCURSES_CH_T blank = NewChar(BLANK_TEXT);

	/*
	 * If the character falls into any of these special cases, do
	 * not force the result to a blank:
	 *
	 * a) it is printable (this works around a bug in wcwidth()).
	 * b) use_legacy_coding() has been called to modify the treatment
	 *    of codes 128-255.
	 * c) the acs_map[] has been initialized to allow codes 0-31
	 *    to be rendered.  This supports Linux console's "PC"
	 *    characters.  Codes 128-255 are allowed though this is
	 *    not checked.
	 */
	if (is8bits(CharOf(CHDEREF(ch)))
	    && (isprint(CharOf(CHDEREF(ch)))
		|| (SP_PARM->_legacy_coding > 0 && CharOf(CHDEREF(ch)) >= 160)
		|| (SP_PARM->_legacy_coding > 1 && CharOf(CHDEREF(ch)) >= 128)
		|| (AttrOf(attr) & A_ALTCHARSET
		    && ((CharOfD(ch) < ACS_LEN
			 && SP_PARM->_acs_map != 0
			 && SP_PARM->_acs_map[CharOfD(ch)] != 0)
			|| (CharOfD(ch) >= 128))))) {
	    ;
	} else {
	    ch = CHREF(blank);
	    TR(TRACE_CHARPUT, ("forced to blank"));
	}
	chlen = 1;
    }
#endif

    if ((AttrOf(attr) & A_ALTCHARSET)
	&& SP_PARM->_acs_map != 0
	&& ((CharOfD(ch) < ACS_LEN)
#if !NCURSES_WCWIDTH_GRAPHICS
	    || is_wacs_value(CharOfD(ch))
#endif
	)) {
	int c8;
	my_ch = CHDEREF(ch);	/* work around const param */
	c8 = CharOf(my_ch);
#if USE_WIDEC_SUPPORT
	/*
	 * This is crude & ugly, but works most of the time.  It checks if the
	 * acs_chars string specified that we have a mapping for this
	 * character, and uses the wide-character mapping when we expect the
	 * normal one to be broken (by mis-design ;-).
	 */
	if (SP_PARM->_screen_unicode
	    && _nc_wacs[CharOf(my_ch)].chars[0]) {
	    if (SP_PARM->_screen_acs_map[CharOf(my_ch)]) {
		if (SP_PARM->_screen_acs_fix) {
		    RemAttr(attr, A_ALTCHARSET);
		    my_ch = _nc_wacs[CharOf(my_ch)];
		}
	    } else {
		RemAttr(attr, A_ALTCHARSET);
		my_ch = _nc_wacs[CharOf(my_ch)];
	    }
#if !NCURSES_WCWIDTH_GRAPHICS
	    if (!(AttrOf(attr) & A_ALTCHARSET)) {
		chlen = 1;
	    }
#endif /* !NCURSES_WCWIDTH_GRAPHICS */
	} else
#endif
	if (!SP_PARM->_screen_acs_map[c8]) {
	    /*
	     * If we found no mapping for a given alternate-character set item
	     * in the terminal description, attempt to use the ASCII fallback
	     * code which is populated in the _acs_map[] array.  If that did
	     * not correspond to a line-drawing, etc., graphics character, the
	     * array entry would be empty.
	     */
	    chtype temp = UChar(SP_PARM->_acs_map[c8]);
	    if (temp) {
		RemAttr(attr, A_ALTCHARSET);
		SetChar(my_ch, temp, AttrOf(attr));
	    }
	}

	/*
	 * If we (still) have alternate character set, it is the normal 8bit
	 * flavor.  The _screen_acs_map[] array tells if the character was
	 * really in acs_chars, needed because of the way wide/normal line
	 * drawing flavors are integrated.
	 */
	if (AttrOf(attr) & A_ALTCHARSET) {
	    int j = CharOfD(ch);
	    chtype temp = UChar(SP_PARM->_acs_map[j]);

	    if (temp != 0) {
		SetChar(my_ch, temp, AttrOf(attr));
	    } else {
		my_ch = CHDEREF(ch);
		RemAttr(attr, A_ALTCHARSET);
	    }
	}
	ch = CHREF(my_ch);
    }
#if USE_WIDEC_SUPPORT && !NCURSES_WCWIDTH_GRAPHICS
    else if (chlen > 1 && is_wacs_value(CharOfD(ch))) {
	chlen = 1;
    }
#endif
    if (tilde_glitch && (CharOfD(ch) == L('~'))) {
	SetChar(tilde, L('`'), AttrOf(attr));
	ch = CHREF(tilde);
    }

    UpdateAttrs(SP_PARM, attr);
    PUTC(CHDEREF(ch));
#if !USE_WIDEC_SUPPORT
    COUNT_OUTCHARS(1);
#endif
    SP_PARM->_curscol += chlen;
    if (char_padding) {
	NCURSES_PUTP2("char_padding", char_padding);
    }
}

static bool
check_pending(NCURSES_SP_DCL0)
/* check for pending input */
{
    bool have_pending = FALSE;

    /*
     * Only carry out this check when the flag is zero, otherwise we'll
     * have the refreshing slow down drastically (or stop) if there's an
     * unread character available.
     */
    if (SP_PARM->_fifohold != 0)
	return FALSE;

    if (SP_PARM->_checkfd >= 0) {
#if USE_FUNC_POLL
	struct pollfd fds[1];
	fds[0].fd = SP_PARM->_checkfd;
	fds[0].events = POLLIN;
	if (poll(fds, (size_t) 1, 0) > 0) {
	    have_pending = TRUE;
	}
#elif defined(__BEOS__)
	/*
	 * BeOS's select() is declared in socket.h, so the configure script does
	 * not see it.  That's just as well, since that function works only for
	 * sockets.  This (using snooze and ioctl) was distilled from Be's patch
	 * for ncurses which uses a separate thread to simulate select().
	 *
	 * FIXME: the return values from the ioctl aren't very clear if we get
	 * interrupted.
	 */
	int n = 0;
	int howmany = ioctl(0, 'ichr', &n);
	if (howmany >= 0 && n > 0) {
	    have_pending = TRUE;
	}
#elif HAVE_SELECT
	fd_set fdset;
	struct timeval ktimeout;

	ktimeout.tv_sec =
	    ktimeout.tv_usec = 0;

	FD_ZERO(&fdset);
	FD_SET(SP_PARM->_checkfd, &fdset);
	if (select(SP_PARM->_checkfd + 1, &fdset, NULL, NULL, &ktimeout) != 0) {
	    have_pending = TRUE;
	}
#endif
    }
    if (have_pending) {
	SP_PARM->_fifohold = 5;
	NCURSES_SP_NAME(_nc_flush) (NCURSES_SP_ARG);
    }
    return FALSE;
}

/* put char at lower right corner */
static void
PutCharLR(NCURSES_SP_DCLx const ARG_CH_T ch)
{
    if (!auto_right_margin) {
	/* we can put the char directly */
	PutAttrChar(NCURSES_SP_ARGx ch);
    } else if (enter_am_mode && exit_am_mode) {
	int oldcol = SP_PARM->_curscol;
	/* we can suppress automargin */
	NCURSES_PUTP2("exit_am_mode", exit_am_mode);

	PutAttrChar(NCURSES_SP_ARGx ch);
	SP_PARM->_curscol = oldcol;
	position_check(NCURSES_SP_ARGx
		       SP_PARM->_cursrow,
		       SP_PARM->_curscol,
		       "exit_am_mode");

	NCURSES_PUTP2("enter_am_mode", enter_am_mode);
    } else if ((enter_insert_mode && exit_insert_mode)
	       || insert_character || parm_ich) {
	GoTo(NCURSES_SP_ARGx
	     screen_lines(SP_PARM) - 1,
	     screen_columns(SP_PARM) - 2);
	PutAttrChar(NCURSES_SP_ARGx ch);
	GoTo(NCURSES_SP_ARGx
	     screen_lines(SP_PARM) - 1,
	     screen_columns(SP_PARM) - 2);
	InsStr(NCURSES_SP_ARGx
	       NewScreen(SP_PARM)->_line[screen_lines(SP_PARM) - 1].text +
	       screen_columns(SP_PARM) - 2, 1);
    }
}

/*
 * Wrap the cursor position, i.e., advance to the beginning of the next line.
 */
static void
wrap_cursor(NCURSES_SP_DCL0)
{
    if (eat_newline_glitch) {
	/*
	 * xenl can manifest two different ways.  The vt100 way is that, when
	 * you'd expect the cursor to wrap, it stays hung at the right margin
	 * (on top of the character just emitted) and doesn't wrap until the
	 * *next* graphic char is emitted.  The c100 way is to ignore LF
	 * received just after an am wrap.
	 *
	 * An aggressive way to handle this would be to emit CR/LF after the
	 * char and then assume the wrap is done, you're on the first position
	 * of the next line, and the terminal out of its weird state.  Here
	 * it is safe to just tell the code that the cursor is in hyperspace and
	 * let the next mvcur() call straighten things out.
	 */
	SP_PARM->_curscol = -1;
	SP_PARM->_cursrow = -1;
    } else if (auto_right_margin) {
	SP_PARM->_curscol = 0;
	SP_PARM->_cursrow++;
	/*
	 * We've actually moved - but may have to work around problems with
	 * video attributes not working.
	 */
	if (!move_standout_mode && AttrOf(SCREEN_ATTRS(SP_PARM))) {
	    TR(TRACE_CHARPUT, ("turning off (%#lx) %s before wrapping",
			       (unsigned long) AttrOf(SCREEN_ATTRS(SP_PARM)),
			       _traceattr(AttrOf(SCREEN_ATTRS(SP_PARM)))));
	    VIDPUTS(SP_PARM, A_NORMAL, 0);
	}
    } else {
	SP_PARM->_curscol--;
    }
    position_check(NCURSES_SP_ARGx
		   SP_PARM->_cursrow,
		   SP_PARM->_curscol,
		   "wrap_cursor");
}

static NCURSES_INLINE void
PutChar(NCURSES_SP_DCLx const ARG_CH_T ch)
/* insert character, handling automargin stuff */
{
    if (SP_PARM->_cursrow == screen_lines(SP_PARM) - 1 &&
	SP_PARM->_curscol == screen_columns(SP_PARM) - 1) {
	PutCharLR(NCURSES_SP_ARGx ch);
    } else {
	PutAttrChar(NCURSES_SP_ARGx ch);
    }

    if (SP_PARM->_curscol >= screen_columns(SP_PARM))
	wrap_cursor(NCURSES_SP_ARG);

    position_check(NCURSES_SP_ARGx
		   SP_PARM->_cursrow,
		   SP_PARM->_curscol, "PutChar");
}

/*
 * Check whether the given character can be output by clearing commands.  This
 * includes test for being a space and not including any 'bad' attributes, such
 * as A_REVERSE.  All attribute flags which don't affect appearance of a space
 * or can be output by clearing (A_COLOR in case of bce-terminal) are excluded.
 */
static NCURSES_INLINE bool
can_clear_with(NCURSES_SP_DCLx ARG_CH_T ch)
{
    if (!back_color_erase && SP_PARM->_coloron) {
#if NCURSES_EXT_FUNCS
	int pair;

	if (!SP_PARM->_default_color)
	    return FALSE;
	if (!(isDefaultColor(SP_PARM->_default_fg) &&
	      isDefaultColor(SP_PARM->_default_bg)))
	    return FALSE;
	if ((pair = GetPair(CHDEREF(ch))) != 0) {
	    NCURSES_COLOR_T fg, bg;
	    if (NCURSES_SP_NAME(pair_content) (NCURSES_SP_ARGx
					       (short) pair,
					       &fg, &bg) == ERR
		|| !(isDefaultColor(fg) && isDefaultColor(bg))) {
		return FALSE;
	    }
	}
#else
	if (AttrOfD(ch) & A_COLOR)
	    return FALSE;
#endif
    }
    return (ISBLANK(CHDEREF(ch)) &&
	    (AttrOfD(ch) & ~(NONBLANK_ATTR | A_COLOR)) == BLANK_ATTR);
}

/*
 * Issue a given span of characters from an array.
 * Must be functionally equivalent to:
 *	for (i = 0; i < num; i++)
 *	    PutChar(ntext[i]);
 * but can leave the cursor positioned at the middle of the interval.
 *
 * Returns: 0 - cursor is at the end of interval
 *	    1 - cursor is somewhere in the middle
 *
 * This code is optimized using ech and rep.
 */
static int
EmitRange(NCURSES_SP_DCLx const NCURSES_CH_T *ntext, int num)
{
    int i;

    TR(TRACE_CHARPUT, ("EmitRange %d:%s", num, _nc_viscbuf(ntext, num)));

    if (erase_chars || repeat_char) {
	while (num > 0) {
	    int runcount;
	    NCURSES_CH_T ntext0;

	    while (num > 1 && !CharEq(ntext[0], ntext[1])) {
		PutChar(NCURSES_SP_ARGx CHREF(ntext[0]));
		ntext++;
		num--;
	    }
	    ntext0 = ntext[0];
	    if (num == 1) {
		PutChar(NCURSES_SP_ARGx CHREF(ntext0));
		return 0;
	    }
	    runcount = 2;

	    while (runcount < num && CharEq(ntext[runcount], ntext0))
		runcount++;

	    /*
	     * The cost expression in the middle isn't exactly right.
	     * _cup_ch_cost is an upper bound on the cost for moving to the
	     * end of the erased area, but not the cost itself (which we
	     * can't compute without emitting the move).  This may result
	     * in erase_chars not getting used in some situations for
	     * which it would be marginally advantageous.
	     */
	    if (erase_chars
		&& runcount > SP_PARM->_ech_cost + SP_PARM->_cup_ch_cost
		&& can_clear_with(NCURSES_SP_ARGx CHREF(ntext0))) {
		UpdateAttrs(SP_PARM, ntext0);
		NCURSES_PUTP2("erase_chars", TIPARM_1(erase_chars, runcount));

		/*
		 * If this is the last part of the given interval,
		 * don't bother moving cursor, since it can be the
		 * last update on the line.
		 */
		if (runcount < num) {
		    GoTo(NCURSES_SP_ARGx
			 SP_PARM->_cursrow,
			 SP_PARM->_curscol + runcount);
		} else {
		    return 1;	/* cursor stays in the middle */
		}
	    } else if (repeat_char != 0 &&
#if BSD_TPUTS
		       !isdigit(UChar(CharOf(ntext0))) &&
#endif
#if USE_WIDEC_SUPPORT
		       (!SP_PARM->_screen_unicode &&
			(CharOf(ntext0) < ((AttrOf(ntext0) & A_ALTCHARSET)
					   ? ACS_LEN
					   : 256))) &&
#endif
		       runcount > SP_PARM->_rep_cost) {
		NCURSES_CH_T temp;
		bool wrap_possible = (SP_PARM->_curscol + runcount >=
				      screen_columns(SP_PARM));
		int rep_count = runcount;

		if (wrap_possible)
		    rep_count--;

		UpdateAttrs(SP_PARM, ntext0);
		temp = ntext0;
		if ((AttrOf(temp) & A_ALTCHARSET) &&
		    SP_PARM->_acs_map != 0 &&
		    (SP_PARM->_acs_map[CharOf(temp)] & A_CHARTEXT) != 0) {
		    SetChar(temp,
			    (SP_PARM->_acs_map[CharOf(ntext0)] & A_CHARTEXT),
			    AttrOf(ntext0) | A_ALTCHARSET);
		}
		NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
					TIPARM_2(repeat_char,
						 CharOf(temp),
						 rep_count),
					1,
					NCURSES_SP_NAME(_nc_outch));
		SP_PARM->_curscol += rep_count;

		if (wrap_possible)
		    PutChar(NCURSES_SP_ARGx CHREF(ntext0));
	    } else {
		for (i = 0; i < runcount; i++)
		    PutChar(NCURSES_SP_ARGx CHREF(ntext[i]));
	    }
	    ntext += runcount;
	    num -= runcount;
	}
	return 0;
    }

    for (i = 0; i < num; i++)
	PutChar(NCURSES_SP_ARGx CHREF(ntext[i]));
    return 0;
}

/*
 * Output the line in the given range [first .. last]
 *
 * If there's a run of identical characters that's long enough to justify
 * cursor movement, use that also.
 *
 * Returns: same as EmitRange
 */
static int
PutRange(NCURSES_SP_DCLx
	 const NCURSES_CH_T *otext,
	 const NCURSES_CH_T *ntext,
	 int row,
	 int first, int last)
{
    int rc;

    TR(TRACE_CHARPUT, ("PutRange(%p, %p, %p, %d, %d, %d)",
		       (void *) SP_PARM,
		       (const void *) otext,
		       (const void *) ntext,
		       row, first, last));

    if (otext != ntext
	&& (last - first + 1) > SP_PARM->_inline_cost) {
	int i, j, same;

	for (j = first, same = 0; j <= last; j++) {
	    if (!same && isWidecExt(otext[j]))
		continue;
	    if (CharEq(otext[j], ntext[j])) {
		same++;
	    } else {
		if (same > SP_PARM->_inline_cost) {
		    EmitRange(NCURSES_SP_ARGx ntext + first, j - same - first);
		    GoTo(NCURSES_SP_ARGx row, first = j);
		}
		same = 0;
	    }
	}
	i = EmitRange(NCURSES_SP_ARGx ntext + first, j - same - first);
	/*
	 * Always return 1 for the next GoTo() after a PutRange() if we found
	 * identical characters at end of interval
	 */
	rc = (same == 0 ? i : 1);
    } else {
	rc = EmitRange(NCURSES_SP_ARGx ntext + first, last - first + 1);
    }
    return rc;
}

/* leave unbracketed here so 'indent' works */
#define MARK_NOCHANGE(win,row) \
		win->_line[row].firstchar = _NOCHANGE; \
		win->_line[row].lastchar = _NOCHANGE; \
		if_USE_SCROLL_HINTS(win->_line[row].oldindex = row)

NCURSES_EXPORT(int)
TINFO_DOUPDATE(NCURSES_SP_DCL0)
{
    int i;
    int nonempty;
#if USE_TRACE_TIMES
    struct tms before, after;
#endif /* USE_TRACE_TIMES */

    T((T_CALLED("_nc_tinfo:doupdate(%p)"), (void *) SP_PARM));

    _nc_lock_global(update);

    if (SP_PARM == 0) {
	_nc_unlock_global(update);
	returnCode(ERR);
    }
#if !USE_REENTRANT
    /*
     * It is "legal" but unlikely that an application could assign a new
     * value to one of the standard windows.  Check for that possibility
     * and try to recover.
     *
     * We do not allow applications to assign new values in the reentrant
     * model.
     */
#if NCURSES_SP_FUNCS
    if (SP_PARM == CURRENT_SCREEN) {
#endif
#define SyncScreens(internal,exported) \
	if (internal == 0) internal = exported; \
	if (internal != exported) exported = internal

	SyncScreens(CurScreen(SP_PARM), curscr);
	SyncScreens(NewScreen(SP_PARM), newscr);
	SyncScreens(StdScreen(SP_PARM), stdscr);
#if NCURSES_SP_FUNCS
    }
#endif
#endif /* !USE_REENTRANT */

    if (CurScreen(SP_PARM) == 0
	|| NewScreen(SP_PARM) == 0
	|| StdScreen(SP_PARM) == 0) {
	_nc_unlock_global(update);
	returnCode(ERR);
    }
#ifdef TRACE
    if (USE_TRACEF(TRACE_UPDATE)) {
	if (CurScreen(SP_PARM)->_clear)
	    _tracef("curscr is clear");
	else
	    _tracedump("curscr", CurScreen(SP_PARM));
	_tracedump("newscr", NewScreen(SP_PARM));
	_nc_unlock_global(tracef);
    }
#endif /* TRACE */

    _nc_signal_handler(FALSE);

    if (SP_PARM->_fifohold)
	SP_PARM->_fifohold--;

#if USE_SIZECHANGE
    if ((SP_PARM->_endwin == ewSuspend)
	|| _nc_handle_sigwinch(SP_PARM)) {
	/*
	 * This is a transparent extension:  XSI does not address it,
	 * and applications need not know that ncurses can do it.
	 *
	 * Check if the terminal size has changed while curses was off
	 * (this can happen in an xterm, for example), and resize the
	 * ncurses data structures accordingly.
	 */
	_nc_update_screensize(SP_PARM);
    }
#endif

    if (SP_PARM->_endwin == ewSuspend) {

	T(("coming back from shell mode"));
	NCURSES_SP_NAME(reset_prog_mode) (NCURSES_SP_ARG);

	NCURSES_SP_NAME(_nc_mvcur_resume) (NCURSES_SP_ARG);
	NCURSES_SP_NAME(_nc_screen_resume) (NCURSES_SP_ARG);
	SP_PARM->_mouse_resume(SP_PARM);

	SP_PARM->_endwin = ewRunning;
    }
#if USE_TRACE_TIMES
    /* zero the metering machinery */
    RESET_OUTCHARS();
    (void) times(&before);
#endif /* USE_TRACE_TIMES */

    /*
     * This is the support for magic-cookie terminals.  The theory:  we scan
     * the virtual screen looking for attribute turnons.  Where we find one,
     * check to make sure it is realizable by seeing if the required number of
     * un-attributed blanks are present before and after the attributed range;
     * try to shift the range boundaries over blanks (not changing the screen
     * display) so this becomes true.  If it is, shift the beginning attribute
     * change appropriately (the end one, if we've gotten this far, is
     * guaranteed room for its cookie).  If not, nuke the added attributes out
     * of the span.
     */
#if USE_XMC_SUPPORT
    if (magic_cookie_glitch > 0) {
	int j, k;
	attr_t rattr = A_NORMAL;

	for (i = 0; i < screen_lines(SP_PARM); i++) {
	    for (j = 0; j < screen_columns(SP_PARM); j++) {
		bool failed = FALSE;
		NCURSES_CH_T *thisline = NewScreen(SP_PARM)->_line[i].text;
		attr_t thisattr = AttrOf(thisline[j]) & SP_PARM->_xmc_triggers;
		attr_t turnon = thisattr & ~rattr;

		/* is an attribute turned on here? */
		if (turnon == 0) {
		    rattr = thisattr;
		    continue;
		}

		TR(TRACE_ATTRS, ("At (%d, %d): from %s...", i, j, _traceattr(rattr)));
		TR(TRACE_ATTRS, ("...to %s", _traceattr(turnon)));

		/*
		 * If the attribute change location is a blank with a "safe"
		 * attribute, undo the attribute turnon.  This may ensure
		 * there's enough room to set the attribute before the first
		 * non-blank in the run.
		 */
#define SAFE(scr,a)	(!((a) & (scr)->_xmc_triggers))
		if (ISBLANK(thisline[j]) && SAFE(SP_PARM, turnon)) {
		    RemAttr(thisline[j], turnon);
		    continue;
		}

		/* check that there's enough room at start of span */
		for (k = 1; k <= magic_cookie_glitch; k++) {
		    if (j - k < 0
			|| !ISBLANK(thisline[j - k])
			|| !SAFE(SP_PARM, AttrOf(thisline[j - k]))) {
			failed = TRUE;
			TR(TRACE_ATTRS, ("No room at start in %d,%d%s%s",
					 i, j - k,
					 (ISBLANK(thisline[j - k])
					  ? ""
					  : ":nonblank"),
					 (SAFE(SP_PARM, AttrOf(thisline[j - k]))
					  ? ""
					  : ":unsafe")));
			break;
		    }
		}
		if (!failed) {
		    bool end_onscreen = FALSE;
		    int m, n = j;

		    /* find end of span, if it is onscreen */
		    for (m = i; m < screen_lines(SP_PARM); m++) {
			for (; n < screen_columns(SP_PARM); n++) {
			    attr_t testattr =
			    AttrOf(NewScreen(SP_PARM)->_line[m].text[n]);
			    if ((testattr & SP_PARM->_xmc_triggers) == rattr) {
				end_onscreen = TRUE;
				TR(TRACE_ATTRS,
				   ("Range attributed with %s ends at (%d, %d)",
				    _traceattr(turnon), m, n));
				goto foundit;
			    }
			}
			n = 0;
		    }
		    TR(TRACE_ATTRS,
		       ("Range attributed with %s ends offscreen",
			_traceattr(turnon)));
		  foundit:;

		    if (end_onscreen) {
			NCURSES_CH_T *lastline =
			NewScreen(SP_PARM)->_line[m].text;

			/*
			 * If there are safely-attributed blanks at the end of
			 * the range, shorten the range.  This will help ensure
			 * that there is enough room at end of span.
			 */
			while (n >= 0
			       && ISBLANK(lastline[n])
			       && SAFE(SP_PARM, AttrOf(lastline[n]))) {
			    RemAttr(lastline[n--], turnon);
			}

			/* check that there's enough room at end of span */
			for (k = 1; k <= magic_cookie_glitch; k++) {
			    if (n + k >= screen_columns(SP_PARM)
				|| !ISBLANK(lastline[n + k])
				|| !SAFE(SP_PARM, AttrOf(lastline[n + k]))) {
				failed = TRUE;
				TR(TRACE_ATTRS,
				   ("No room at end in %d,%d%s%s",
				    i, j - k,
				    (ISBLANK(lastline[n + k])
				     ? ""
				     : ":nonblank"),
				    (SAFE(SP_PARM, AttrOf(lastline[n + k]))
				     ? ""
				     : ":unsafe")));
				break;
			    }
			}
		    }
		}

		if (failed) {
		    int p, q = j;

		    TR(TRACE_ATTRS,
		       ("Clearing %s beginning at (%d, %d)",
			_traceattr(turnon), i, j));

		    /* turn off new attributes over span */
		    for (p = i; p < screen_lines(SP_PARM); p++) {
			for (; q < screen_columns(SP_PARM); q++) {
			    attr_t testattr = AttrOf(newscr->_line[p].text[q]);
			    if ((testattr & SP_PARM->_xmc_triggers) == rattr)
				goto foundend;
			    RemAttr(NewScreen(SP_PARM)->_line[p].text[q], turnon);
			}
			q = 0;
		    }
		  foundend:;
		} else {
		    TR(TRACE_ATTRS,
		       ("Cookie space for %s found before (%d, %d)",
			_traceattr(turnon), i, j));

		    /*
		     * Back up the start of range so there's room for cookies
		     * before the first nonblank character.
		     */
		    for (k = 1; k <= magic_cookie_glitch; k++)
			AddAttr(thisline[j - k], turnon);
		}

		rattr = thisattr;
	    }
	}

#ifdef TRACE
	/* show altered highlights after magic-cookie check */
	if (USE_TRACEF(TRACE_UPDATE)) {
	    _tracef("After magic-cookie check...");
	    _tracedump("newscr", NewScreen(SP_PARM));
	    _nc_unlock_global(tracef);
	}
#endif /* TRACE */
    }
#endif /* USE_XMC_SUPPORT */

    nonempty = 0;
    if (CurScreen(SP_PARM)->_clear || NewScreen(SP_PARM)->_clear) {	/* force refresh ? */
	ClrUpdate(NCURSES_SP_ARG);
	CurScreen(SP_PARM)->_clear = FALSE;	/* reset flag */
	NewScreen(SP_PARM)->_clear = FALSE;	/* reset flag */
    } else {
	int changedlines = CHECK_INTERVAL;

	if (check_pending(NCURSES_SP_ARG))
	    goto cleanup;

	nonempty = min(screen_lines(SP_PARM), NewScreen(SP_PARM)->_maxy + 1);

	if (SP_PARM->_scrolling) {
	    NCURSES_SP_NAME(_nc_scroll_optimize) (NCURSES_SP_ARG);
	}

	nonempty = ClrBottom(NCURSES_SP_ARGx nonempty);

	TR(TRACE_UPDATE, ("Transforming lines, nonempty %d", nonempty));
	for (i = 0; i < nonempty; i++) {
	    /*
	     * Here is our line-breakout optimization.
	     */
	    if (changedlines == CHECK_INTERVAL) {
		if (check_pending(NCURSES_SP_ARG))
		    goto cleanup;
		changedlines = 0;
	    }

	    /*
	     * newscr->line[i].firstchar is normally set
	     * by wnoutrefresh.  curscr->line[i].firstchar
	     * is normally set by _nc_scroll_window in the
	     * vertical-movement optimization code,
	     */
	    if (NewScreen(SP_PARM)->_line[i].firstchar != _NOCHANGE
		|| CurScreen(SP_PARM)->_line[i].firstchar != _NOCHANGE) {
		TransformLine(NCURSES_SP_ARGx i);
		changedlines++;
	    }

	    /* mark line changed successfully */
	    if (i <= NewScreen(SP_PARM)->_maxy) {
		MARK_NOCHANGE(NewScreen(SP_PARM), i);
	    }
	    if (i <= CurScreen(SP_PARM)->_maxy) {
		MARK_NOCHANGE(CurScreen(SP_PARM), i);
	    }
	}
    }

    /* put everything back in sync */
    for (i = nonempty; i <= NewScreen(SP_PARM)->_maxy; i++) {
	MARK_NOCHANGE(NewScreen(SP_PARM), i);
    }
    for (i = nonempty; i <= CurScreen(SP_PARM)->_maxy; i++) {
	MARK_NOCHANGE(CurScreen(SP_PARM), i);
    }

    if (!NewScreen(SP_PARM)->_leaveok) {
	CurScreen(SP_PARM)->_curx = NewScreen(SP_PARM)->_curx;
	CurScreen(SP_PARM)->_cury = NewScreen(SP_PARM)->_cury;

	GoTo(NCURSES_SP_ARGx CurScreen(SP_PARM)->_cury, CurScreen(SP_PARM)->_curx);
    }

  cleanup:
    /*
     * We would like to keep the physical screen in normal mode in case we get
     * other processes writing to the screen.  This goal cannot be met for
     * magic cookies since it interferes with attributes that may propagate
     * past the current position.
     */
#if USE_XMC_SUPPORT
    if (magic_cookie_glitch != 0)
#endif
	UpdateAttrs(SP_PARM, normal);

    NCURSES_SP_NAME(_nc_flush) (NCURSES_SP_ARG);
    WINDOW_ATTRS(CurScreen(SP_PARM)) = WINDOW_ATTRS(NewScreen(SP_PARM));

#if USE_TRACE_TIMES
    (void) times(&after);
    TR(TRACE_TIMES,
       ("Update cost: %ld chars, %ld clocks system time, %ld clocks user time",
	_nc_outchars,
	(long) (after.tms_stime - before.tms_stime),
	(long) (after.tms_utime - before.tms_utime)));
#endif /* USE_TRACE_TIMES */

    _nc_signal_handler(TRUE);

    _nc_unlock_global(update);
    returnCode(OK);
}

#if NCURSES_SP_FUNCS && !defined(USE_TERM_DRIVER)
NCURSES_EXPORT(int)
doupdate(void)
{
    return TINFO_DOUPDATE(CURRENT_SCREEN);
}
#endif

/*
 *	ClrBlank(win)
 *
 *	Returns the attributed character that corresponds to the "cleared"
 *	screen.  If the terminal has the back-color-erase feature, this will be
 *	colored according to the wbkgd() call.
 *
 *	We treat 'curscr' specially because it isn't supposed to be set directly
 *	in the wbkgd() call.  Assume 'stdscr' for this case.
 */
#define BCE_ATTRS (A_NORMAL|A_COLOR)
#define BCE_BKGD(sp,win) (((win) == CurScreen(sp) ? StdScreen(sp) : (win))->_nc_bkgd)

static NCURSES_INLINE NCURSES_CH_T
ClrBlank(NCURSES_SP_DCLx WINDOW *win)
{
    NCURSES_CH_T blank = blankchar;
    if (back_color_erase)
	AddAttr(blank, (AttrOf(BCE_BKGD(SP_PARM, win)) & BCE_ATTRS));
    return blank;
}

/*
**	ClrUpdate()
**
**	Update by clearing and redrawing the entire screen.
**
*/

static void
ClrUpdate(NCURSES_SP_DCL0)
{
    TR(TRACE_UPDATE, (T_CALLED("ClrUpdate")));
    if (0 != SP_PARM) {
	int i;
	NCURSES_CH_T blank = ClrBlank(NCURSES_SP_ARGx StdScreen(SP_PARM));
	int nonempty = min(screen_lines(SP_PARM),
			   NewScreen(SP_PARM)->_maxy + 1);

	ClearScreen(NCURSES_SP_ARGx blank);

	TR(TRACE_UPDATE, ("updating screen from scratch"));

	nonempty = ClrBottom(NCURSES_SP_ARGx nonempty);

	for (i = 0; i < nonempty; i++)
	    TransformLine(NCURSES_SP_ARGx i);
    }
    TR(TRACE_UPDATE, (T_RETURN("")));
}

/*
**	ClrToEOL(blank)
**
**	Clear to end of current line, starting at the cursor position
*/

static void
ClrToEOL(NCURSES_SP_DCLx NCURSES_CH_T blank, int needclear)
{
    if (CurScreen(SP_PARM) != 0
	&& SP_PARM->_cursrow >= 0) {
	int j;

	for (j = SP_PARM->_curscol; j < screen_columns(SP_PARM); j++) {
	    if (j >= 0) {
		NCURSES_CH_T *cp =
		&(CurScreen(SP_PARM)->_line[SP_PARM->_cursrow].text[j]);

		if (!CharEq(*cp, blank)) {
		    *cp = blank;
		    needclear = TRUE;
		}
	    }
	}
    }

    if (needclear) {
	UpdateAttrs(SP_PARM, blank);
	if (clr_eol && SP_PARM->_el_cost <= (screen_columns(SP_PARM) - SP_PARM->_curscol)) {
	    NCURSES_PUTP2("clr_eol", clr_eol);
	} else {
	    int count = (screen_columns(SP_PARM) - SP_PARM->_curscol);
	    while (count-- > 0)
		PutChar(NCURSES_SP_ARGx CHREF(blank));
	}
    }
}

/*
**	ClrToEOS(blank)
**
**	Clear to end of screen, starting at the cursor position
*/

static void
ClrToEOS(NCURSES_SP_DCLx NCURSES_CH_T blank)
{
    int row, col;

    row = SP_PARM->_cursrow;
    col = SP_PARM->_curscol;

    if (row < 0)
	row = 0;
    if (col < 0)
	col = 0;

    UpdateAttrs(SP_PARM, blank);
    TPUTS_TRACE("clr_eos");
    NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
			    clr_eos,
			    screen_lines(SP_PARM) - row,
			    NCURSES_SP_NAME(_nc_outch));

    while (col < screen_columns(SP_PARM))
	CurScreen(SP_PARM)->_line[row].text[col++] = blank;

    for (row++; row < screen_lines(SP_PARM); row++) {
	for (col = 0; col < screen_columns(SP_PARM); col++)
	    CurScreen(SP_PARM)->_line[row].text[col] = blank;
    }
}

/*
 *	ClrBottom(total)
 *
 *	Test if clearing the end of the screen would satisfy part of the
 *	screen-update.  Do this by scanning backwards through the lines in the
 *	screen, checking if each is blank, and one or more are changed.
 */
static int
ClrBottom(NCURSES_SP_DCLx int total)
{
    int top = total;
    int last = min(screen_columns(SP_PARM), NewScreen(SP_PARM)->_maxx + 1);
    NCURSES_CH_T blank = NewScreen(SP_PARM)->_line[total - 1].text[last - 1];

    if (clr_eos && can_clear_with(NCURSES_SP_ARGx CHREF(blank))) {
	int row;

	for (row = total - 1; row >= 0; row--) {
	    int col;
	    bool ok;

	    for (col = 0, ok = TRUE; ok && col < last; col++) {
		ok = (CharEq(NewScreen(SP_PARM)->_line[row].text[col], blank));
	    }
	    if (!ok)
		break;

	    for (col = 0; ok && col < last; col++) {
		ok = (CharEq(CurScreen(SP_PARM)->_line[row].text[col], blank));
	    }
	    if (!ok)
		top = row;
	}

	/* don't use clr_eos for just one line if clr_eol available */
	if (top < total) {
	    GoTo(NCURSES_SP_ARGx top, 0);
	    ClrToEOS(NCURSES_SP_ARGx blank);
	    if (SP_PARM->oldhash && SP_PARM->newhash) {
		for (row = top; row < screen_lines(SP_PARM); row++)
		    SP_PARM->oldhash[row] = SP_PARM->newhash[row];
	    }
	}
    }
    return top;
}

#if USE_XMC_SUPPORT
#if USE_WIDEC_SUPPORT
#define check_xmc_transition(sp, a, b)					\
    ((((a)->attr ^ (b)->attr) & ~((a)->attr) & (sp)->_xmc_triggers) != 0)
#define xmc_turn_on(sp,a,b) check_xmc_transition(sp,&(a), &(b))
#else
#define xmc_turn_on(sp,a,b) ((((a)^(b)) & ~(a) & (sp)->_xmc_triggers) != 0)
#endif

#define xmc_new(sp,r,c) NewScreen(sp)->_line[r].text[c]
#define xmc_turn_off(sp,a,b) xmc_turn_on(sp,b,a)
#endif /* USE_XMC_SUPPORT */

/*
**	TransformLine(lineno)
**
**	Transform the given line in curscr to the one in newscr, using
**	Insert/Delete Character if idcok && has_ic().
**
**		firstChar = position of first different character in line
**		oLastChar = position of last different character in old line
**		nLastChar = position of last different character in new line
**
**		move to firstChar
**		overwrite chars up to min(oLastChar, nLastChar)
**		if oLastChar < nLastChar
**			insert newLine[oLastChar+1..nLastChar]
**		else
**			delete oLastChar - nLastChar spaces
*/

static void
TransformLine(NCURSES_SP_DCLx int const lineno)
{
    int firstChar, oLastChar, nLastChar;
    NCURSES_CH_T *newLine = NewScreen(SP_PARM)->_line[lineno].text;
    NCURSES_CH_T *oldLine = CurScreen(SP_PARM)->_line[lineno].text;
    int n;
    bool attrchanged = FALSE;

    TR(TRACE_UPDATE, (T_CALLED("TransformLine(%p, %d)"), (void *) SP_PARM, lineno));

    /* copy new hash value to old one */
    if (SP_PARM->oldhash && SP_PARM->newhash)
	SP_PARM->oldhash[lineno] = SP_PARM->newhash[lineno];

    /*
     * If we have colors, there is the possibility of having two color pairs
     * that display as the same colors.  For instance, Lynx does this.  Check
     * for this case, and update the old line with the new line's colors when
     * they are equivalent.
     */
    if (SP_PARM->_coloron) {
	int oldPair;
	int newPair;

	for (n = 0; n < screen_columns(SP_PARM); n++) {
	    if (!CharEq(newLine[n], oldLine[n])) {
		oldPair = GetPair(oldLine[n]);
		newPair = GetPair(newLine[n]);
		if (oldPair != newPair
		    && unColor(oldLine[n]) == unColor(newLine[n])) {
		    if (oldPair < SP_PARM->_pair_alloc
			&& newPair < SP_PARM->_pair_alloc
			&& (isSamePair(SP_PARM->_color_pairs[oldPair],
				       SP_PARM->_color_pairs[newPair]))) {
			SetPair(oldLine[n], GetPair(newLine[n]));
		    }
		}
	    }
	}
    }

    if (ceol_standout_glitch && clr_eol) {
	firstChar = 0;
	while (firstChar < screen_columns(SP_PARM)) {
	    if (!SameAttrOf(newLine[firstChar], oldLine[firstChar])) {
		attrchanged = TRUE;
		break;
	    }
	    firstChar++;
	}
    }

    firstChar = 0;

    if (attrchanged) {		/* we may have to disregard the whole line */
	GoTo(NCURSES_SP_ARGx lineno, firstChar);
	ClrToEOL(NCURSES_SP_ARGx
		 ClrBlank(NCURSES_SP_ARGx
			  CurScreen(SP_PARM)), FALSE);
	PutRange(NCURSES_SP_ARGx
		 oldLine, newLine, lineno, 0,
		 screen_columns(SP_PARM) - 1);
#if USE_XMC_SUPPORT

	/*
	 * This is a very simple loop to paint characters which may have the
	 * magic cookie glitch embedded.  It doesn't know much about video
	 * attributes which are continued from one line to the next.  It
	 * assumes that we have filtered out requests for attribute changes
	 * that do not get mapped to blank positions.
	 *
	 * FIXME: we are not keeping track of where we put the cookies, so this
	 * will work properly only once, since we may overwrite a cookie in a
	 * following operation.
	 */
    } else if (magic_cookie_glitch > 0) {
	GoTo(NCURSES_SP_ARGx lineno, firstChar);
	for (n = 0; n < screen_columns(SP_PARM); n++) {
	    int m = n + magic_cookie_glitch;

	    /* check for turn-on:
	     * If we are writing an attributed blank, where the
	     * previous cell is not attributed.
	     */
	    if (ISBLANK(newLine[n])
		&& ((n > 0
		     && xmc_turn_on(SP_PARM, newLine[n - 1], newLine[n]))
		    || (n == 0
			&& lineno > 0
			&& xmc_turn_on(SP_PARM,
				       xmc_new(SP_PARM, lineno - 1,
					       screen_columns(SP_PARM) - 1),
				       newLine[n])))) {
		n = m;
	    }

	    PutChar(NCURSES_SP_ARGx CHREF(newLine[n]));

	    /* check for turn-off:
	     * If we are writing an attributed non-blank, where the
	     * next cell is blank, and not attributed.
	     */
	    if (!ISBLANK(newLine[n])
		&& ((n + 1 < screen_columns(SP_PARM)
		     && xmc_turn_off(SP_PARM, newLine[n], newLine[n + 1]))
		    || (n + 1 >= screen_columns(SP_PARM)
			&& lineno + 1 < screen_lines(SP_PARM)
			&& xmc_turn_off(SP_PARM,
					newLine[n],
					xmc_new(SP_PARM, lineno + 1, 0))))) {
		n = m;
	    }

	}
#endif
    } else {
	NCURSES_CH_T blank;

	/* it may be cheap to clear leading whitespace with clr_bol */
	blank = newLine[0];
	if (clr_bol && can_clear_with(NCURSES_SP_ARGx CHREF(blank))) {
	    int oFirstChar, nFirstChar;

	    for (oFirstChar = 0;
		 oFirstChar < screen_columns(SP_PARM);
		 oFirstChar++)
		if (!CharEq(oldLine[oFirstChar], blank))
		    break;
	    for (nFirstChar = 0;
		 nFirstChar < screen_columns(SP_PARM);
		 nFirstChar++)
		if (!CharEq(newLine[nFirstChar], blank))
		    break;

	    if (nFirstChar == oFirstChar) {
		firstChar = nFirstChar;
		/* find the first differing character */
		while (firstChar < screen_columns(SP_PARM)
		       && CharEq(newLine[firstChar], oldLine[firstChar]))
		    firstChar++;
	    } else if (oFirstChar > nFirstChar) {
		firstChar = nFirstChar;
	    } else {		/* oFirstChar < nFirstChar */
		firstChar = oFirstChar;
		if (SP_PARM->_el1_cost < nFirstChar - oFirstChar) {
		    if (nFirstChar >= screen_columns(SP_PARM)
			&& SP_PARM->_el_cost <= SP_PARM->_el1_cost) {
			GoTo(NCURSES_SP_ARGx lineno, 0);
			UpdateAttrs(SP_PARM, blank);
			NCURSES_PUTP2("clr_eol", clr_eol);
		    } else {
			GoTo(NCURSES_SP_ARGx lineno, nFirstChar - 1);
			UpdateAttrs(SP_PARM, blank);
			NCURSES_PUTP2("clr_bol", clr_bol);
		    }

		    while (firstChar < nFirstChar)
			oldLine[firstChar++] = blank;
		}
	    }
	} else {
	    /* find the first differing character */
	    while (firstChar < screen_columns(SP_PARM)
		   && CharEq(newLine[firstChar], oldLine[firstChar]))
		firstChar++;
	}
	/* if there wasn't one, we're done */
	if (firstChar >= screen_columns(SP_PARM)) {
	    TR(TRACE_UPDATE, (T_RETURN("")));
	    return;
	}

	blank = newLine[screen_columns(SP_PARM) - 1];

	if (!can_clear_with(NCURSES_SP_ARGx CHREF(blank))) {
	    /* find the last differing character */
	    nLastChar = screen_columns(SP_PARM) - 1;

	    while (nLastChar > firstChar
		   && CharEq(newLine[nLastChar], oldLine[nLastChar]))
		nLastChar--;

	    if (nLastChar >= firstChar) {
		GoTo(NCURSES_SP_ARGx lineno, firstChar);
		PutRange(NCURSES_SP_ARGx
			 oldLine,
			 newLine,
			 lineno,
			 firstChar,
			 nLastChar);
		memcpy(oldLine + firstChar,
		       newLine + firstChar,
		       (unsigned) (nLastChar - firstChar + 1) * sizeof(NCURSES_CH_T));
	    }
	    TR(TRACE_UPDATE, (T_RETURN("")));
	    return;
	}

	/* find last non-blank character on old line */
	oLastChar = screen_columns(SP_PARM) - 1;
	while (oLastChar > firstChar && CharEq(oldLine[oLastChar], blank))
	    oLastChar--;

	/* find last non-blank character on new line */
	nLastChar = screen_columns(SP_PARM) - 1;
	while (nLastChar > firstChar && CharEq(newLine[nLastChar], blank))
	    nLastChar--;

	if ((nLastChar == firstChar)
	    && (SP_PARM->_el_cost < (oLastChar - nLastChar))) {
	    GoTo(NCURSES_SP_ARGx lineno, firstChar);
	    if (!CharEq(newLine[firstChar], blank))
		PutChar(NCURSES_SP_ARGx CHREF(newLine[firstChar]));
	    ClrToEOL(NCURSES_SP_ARGx blank, FALSE);
	} else if ((nLastChar != oLastChar)
		   && (!CharEq(newLine[nLastChar], oldLine[oLastChar])
		       || !(SP_PARM->_nc_sp_idcok
			    && NCURSES_SP_NAME(has_ic) (NCURSES_SP_ARG)))) {
	    GoTo(NCURSES_SP_ARGx lineno, firstChar);
	    if ((oLastChar - nLastChar) > SP_PARM->_el_cost) {
		if (PutRange(NCURSES_SP_ARGx
			     oldLine,
			     newLine,
			     lineno,
			     firstChar,
			     nLastChar)) {
		    GoTo(NCURSES_SP_ARGx lineno, nLastChar + 1);
		}
		ClrToEOL(NCURSES_SP_ARGx blank, FALSE);
	    } else {
		n = max(nLastChar, oLastChar);
		PutRange(NCURSES_SP_ARGx
			 oldLine,
			 newLine,
			 lineno,
			 firstChar,
			 n);
	    }
	} else {
	    int nLastNonblank = nLastChar;
	    int oLastNonblank = oLastChar;

	    /* find the last characters that really differ */
	    /* can be -1 if no characters differ */
	    while (CharEq(newLine[nLastChar], oldLine[oLastChar])) {
		/* don't split a wide char */
		if (isWidecExt(newLine[nLastChar]) &&
		    !CharEq(newLine[nLastChar - 1], oldLine[oLastChar - 1]))
		    break;
		nLastChar--;
		oLastChar--;
		if (nLastChar == -1 || oLastChar == -1)
		    break;
	    }

	    n = min(oLastChar, nLastChar);
	    if (n >= firstChar) {
		GoTo(NCURSES_SP_ARGx lineno, firstChar);
		PutRange(NCURSES_SP_ARGx
			 oldLine,
			 newLine,
			 lineno,
			 firstChar,
			 n);
	    }

	    if (oLastChar < nLastChar) {
		int m = max(nLastNonblank, oLastNonblank);
#if USE_WIDEC_SUPPORT
		if (n) {
		    while (isWidecExt(newLine[n + 1]) && n) {
			--n;
			--oLastChar;	/* increase cost */
		    }
		} else if (n >= firstChar &&
			   isWidecBase(newLine[n])) {
		    while (isWidecExt(newLine[n + 1])) {
			++n;
			++oLastChar;	/* decrease cost */
		    }
		}
#endif
		GoTo(NCURSES_SP_ARGx lineno, n + 1);
		if ((nLastChar < nLastNonblank)
		    || InsCharCost(SP_PARM, nLastChar - oLastChar) > (m - n)) {
		    PutRange(NCURSES_SP_ARGx
			     oldLine,
			     newLine,
			     lineno,
			     n + 1,
			     m);
		} else {
		    InsStr(NCURSES_SP_ARGx &newLine[n + 1], nLastChar - oLastChar);
		}
	    } else if (oLastChar > nLastChar) {
		GoTo(NCURSES_SP_ARGx lineno, n + 1);
		if (DelCharCost(SP_PARM, oLastChar - nLastChar)
		    > SP_PARM->_el_cost + nLastNonblank - (n + 1)) {
		    if (PutRange(NCURSES_SP_ARGx oldLine, newLine, lineno,
				 n + 1, nLastNonblank)) {
			GoTo(NCURSES_SP_ARGx lineno, nLastNonblank + 1);
		    }
		    ClrToEOL(NCURSES_SP_ARGx blank, FALSE);
		} else {
		    /*
		     * The delete-char sequence will
		     * effectively shift in blanks from the
		     * right margin of the screen.  Ensure
		     * that they are the right color by
		     * setting the video attributes from
		     * the last character on the row.
		     */
		    UpdateAttrs(SP_PARM, blank);
		    DelChar(NCURSES_SP_ARGx oLastChar - nLastChar);
		}
	    }
	}
    }

    /* update the code's internal representation */
    if (screen_columns(SP_PARM) > firstChar)
	memcpy(oldLine + firstChar,
	       newLine + firstChar,
	       (unsigned) (screen_columns(SP_PARM) - firstChar) * sizeof(NCURSES_CH_T));
    TR(TRACE_UPDATE, (T_RETURN("")));
    return;
}

/*
**	ClearScreen(blank)
**
**	Clear the physical screen and put cursor at home
**
*/

static void
ClearScreen(NCURSES_SP_DCLx NCURSES_CH_T blank)
{
    int i, j;
    bool fast_clear = (clear_screen || clr_eos || clr_eol);

    TR(TRACE_UPDATE, ("ClearScreen() called"));

#if NCURSES_EXT_FUNCS
    if (SP_PARM->_coloron
	&& !SP_PARM->_default_color) {
	NCURSES_SP_NAME(_nc_do_color) (NCURSES_SP_ARGx
				       (short) GET_SCREEN_PAIR(SP_PARM),
				       0,
				       FALSE,
				       NCURSES_SP_NAME(_nc_outch));
	if (!back_color_erase) {
	    fast_clear = FALSE;
	}
    }
#endif

    if (fast_clear) {
	if (clear_screen) {
	    UpdateAttrs(SP_PARM, blank);
	    NCURSES_PUTP2("clear_screen", clear_screen);
	    SP_PARM->_cursrow = SP_PARM->_curscol = 0;
	    position_check(NCURSES_SP_ARGx
			   SP_PARM->_cursrow,
			   SP_PARM->_curscol,
			   "ClearScreen");
	} else if (clr_eos) {
	    SP_PARM->_cursrow = SP_PARM->_curscol = -1;
	    GoTo(NCURSES_SP_ARGx 0, 0);
	    UpdateAttrs(SP_PARM, blank);
	    TPUTS_TRACE("clr_eos");
	    NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				    clr_eos,
				    screen_lines(SP_PARM),
				    NCURSES_SP_NAME(_nc_outch));
	} else if (clr_eol) {
	    SP_PARM->_cursrow = SP_PARM->_curscol = -1;
	    UpdateAttrs(SP_PARM, blank);
	    for (i = 0; i < screen_lines(SP_PARM); i++) {
		GoTo(NCURSES_SP_ARGx i, 0);
		NCURSES_PUTP2("clr_eol", clr_eol);
	    }
	    GoTo(NCURSES_SP_ARGx 0, 0);
	}
    } else {
	UpdateAttrs(SP_PARM, blank);
	for (i = 0; i < screen_lines(SP_PARM); i++) {
	    GoTo(NCURSES_SP_ARGx i, 0);
	    for (j = 0; j < screen_columns(SP_PARM); j++)
		PutChar(NCURSES_SP_ARGx CHREF(blank));
	}
	GoTo(NCURSES_SP_ARGx 0, 0);
    }

    for (i = 0; i < screen_lines(SP_PARM); i++) {
	for (j = 0; j < screen_columns(SP_PARM); j++)
	    CurScreen(SP_PARM)->_line[i].text[j] = blank;
    }

    TR(TRACE_UPDATE, ("screen cleared"));
}

/*
**	InsStr(line, count)
**
**	Insert the count characters pointed to by line.
**
*/

static void
InsStr(NCURSES_SP_DCLx NCURSES_CH_T *line, int count)
{
    TR(TRACE_UPDATE, ("InsStr(%p, %p,%d) called",
		      (void *) SP_PARM,
		      (void *) line, count));

    /* Prefer parm_ich as it has the smallest cost - no need to shift
     * the whole line on each character. */
    /* The order must match that of InsCharCost. */
    if (parm_ich) {
	TPUTS_TRACE("parm_ich");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_ich, count),
				1,
				NCURSES_SP_NAME(_nc_outch));
	while (count > 0) {
	    PutAttrChar(NCURSES_SP_ARGx CHREF(*line));
	    line++;
	    count--;
	}
    } else if (enter_insert_mode && exit_insert_mode) {
	NCURSES_PUTP2("enter_insert_mode", enter_insert_mode);
	while (count > 0) {
	    PutAttrChar(NCURSES_SP_ARGx CHREF(*line));
	    if (insert_padding) {
		NCURSES_PUTP2("insert_padding", insert_padding);
	    }
	    line++;
	    count--;
	}
	NCURSES_PUTP2("exit_insert_mode", exit_insert_mode);
    } else {
	while (count > 0) {
	    NCURSES_PUTP2("insert_character", insert_character);
	    PutAttrChar(NCURSES_SP_ARGx CHREF(*line));
	    if (insert_padding) {
		NCURSES_PUTP2("insert_padding", insert_padding);
	    }
	    line++;
	    count--;
	}
    }
    position_check(NCURSES_SP_ARGx
		   SP_PARM->_cursrow,
		   SP_PARM->_curscol, "InsStr");
}

/*
**	DelChar(count)
**
**	Delete count characters at current position
**
*/

static void
DelChar(NCURSES_SP_DCLx int count)
{
    TR(TRACE_UPDATE, ("DelChar(%p, %d) called, position = (%ld,%ld)",
		      (void *) SP_PARM, count,
		      (long) NewScreen(SP_PARM)->_cury,
		      (long) NewScreen(SP_PARM)->_curx));

    if (parm_dch) {
	TPUTS_TRACE("parm_dch");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_dch, count),
				1,
				NCURSES_SP_NAME(_nc_outch));
    } else {
	int n;

	for (n = 0; n < count; n++) {
	    NCURSES_PUTP2("delete_character", delete_character);
	}
    }
}

/*
 * Physical-scrolling support
 *
 * This code was adapted from Keith Bostic's hardware scrolling
 * support for 4.4BSD curses.  I (esr) translated it to use terminfo
 * capabilities, narrowed the call interface slightly, and cleaned
 * up some convoluted tests.  I also added support for the memory_above
 * memory_below, and non_dest_scroll_region capabilities.
 *
 * For this code to work, we must have either
 * change_scroll_region and scroll forward/reverse commands, or
 * insert and delete line capabilities.
 * When the scrolling region has been set, the cursor has to
 * be at the last line of the region to make the scroll up
 * happen, or on the first line of region to scroll down.
 *
 * This code makes one aesthetic decision in the opposite way from
 * BSD curses.  BSD curses preferred pairs of il/dl operations
 * over scrolls, allegedly because il/dl looked faster.  We, on
 * the other hand, prefer scrolls because (a) they're just as fast
 * on many terminals and (b) using them avoids bouncing an
 * unchanged bottom section of the screen up and down, which is
 * visually nasty.
 *
 * (lav): added more cases, used dl/il when bot==maxy and in csr case.
 *
 * I used assumption that capabilities il/il1/dl/dl1 work inside
 * changed scroll region not shifting screen contents outside of it.
 * If there are any terminals behaving different way, it would be
 * necessary to add some conditions to scroll_csr_forward/backward.
 */

/* Try to scroll up assuming given csr (miny, maxy). Returns ERR on failure */
static int
scroll_csr_forward(NCURSES_SP_DCLx
		   int n,
		   int top,
		   int bot,
		   int miny,
		   int maxy,
		   NCURSES_CH_T blank)
{
    int i;

    if (n == 1 && scroll_forward && top == miny && bot == maxy) {
	GoTo(NCURSES_SP_ARGx bot, 0);
	UpdateAttrs(SP_PARM, blank);
	NCURSES_PUTP2("scroll_forward", scroll_forward);
    } else if (n == 1 && delete_line && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	NCURSES_PUTP2("delete_line", delete_line);
    } else if (parm_index && top == miny && bot == maxy) {
	GoTo(NCURSES_SP_ARGx bot, 0);
	UpdateAttrs(SP_PARM, blank);
	TPUTS_TRACE("parm_index");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_index, n),
				n,
				NCURSES_SP_NAME(_nc_outch));
    } else if (parm_delete_line && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	TPUTS_TRACE("parm_delete_line");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_delete_line, n),
				n,
				NCURSES_SP_NAME(_nc_outch));
    } else if (scroll_forward && top == miny && bot == maxy) {
	GoTo(NCURSES_SP_ARGx bot, 0);
	UpdateAttrs(SP_PARM, blank);
	for (i = 0; i < n; i++) {
	    NCURSES_PUTP2("scroll_forward", scroll_forward);
	}
    } else if (delete_line && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	for (i = 0; i < n; i++) {
	    NCURSES_PUTP2("delete_line", delete_line);
	}
    } else
	return ERR;

#if NCURSES_EXT_FUNCS
    if (FILL_BCE(SP_PARM)) {
	int j;
	for (i = 0; i < n; i++) {
	    GoTo(NCURSES_SP_ARGx bot - i, 0);
	    for (j = 0; j < screen_columns(SP_PARM); j++)
		PutChar(NCURSES_SP_ARGx CHREF(blank));
	}
    }
#endif
    return OK;
}

/* Try to scroll down assuming given csr (miny, maxy). Returns ERR on failure */
/* n > 0 */
static int
scroll_csr_backward(NCURSES_SP_DCLx
		    int n,
		    int top,
		    int bot,
		    int miny,
		    int maxy,
		    NCURSES_CH_T blank)
{
    int i;

    if (n == 1 && scroll_reverse && top == miny && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	NCURSES_PUTP2("scroll_reverse", scroll_reverse);
    } else if (n == 1 && insert_line && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	NCURSES_PUTP2("insert_line", insert_line);
    } else if (parm_rindex && top == miny && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	TPUTS_TRACE("parm_rindex");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_rindex, n),
				n,
				NCURSES_SP_NAME(_nc_outch));
    } else if (parm_insert_line && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	TPUTS_TRACE("parm_insert_line");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_insert_line, n),
				n,
				NCURSES_SP_NAME(_nc_outch));
    } else if (scroll_reverse && top == miny && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	for (i = 0; i < n; i++) {
	    NCURSES_PUTP2("scroll_reverse", scroll_reverse);
	}
    } else if (insert_line && bot == maxy) {
	GoTo(NCURSES_SP_ARGx top, 0);
	UpdateAttrs(SP_PARM, blank);
	for (i = 0; i < n; i++) {
	    NCURSES_PUTP2("insert_line", insert_line);
	}
    } else
	return ERR;

#if NCURSES_EXT_FUNCS
    if (FILL_BCE(SP_PARM)) {
	int j;
	for (i = 0; i < n; i++) {
	    GoTo(NCURSES_SP_ARGx top + i, 0);
	    for (j = 0; j < screen_columns(SP_PARM); j++)
		PutChar(NCURSES_SP_ARGx CHREF(blank));
	}
    }
#endif
    return OK;
}

/* scroll by using delete_line at del and insert_line at ins */
/* n > 0 */
static int
scroll_idl(NCURSES_SP_DCLx int n, int del, int ins, NCURSES_CH_T blank)
{
    int i;

    if (!((parm_delete_line || delete_line) && (parm_insert_line || insert_line)))
	return ERR;

    GoTo(NCURSES_SP_ARGx del, 0);
    UpdateAttrs(SP_PARM, blank);
    if (n == 1 && delete_line) {
	NCURSES_PUTP2("delete_line", delete_line);
    } else if (parm_delete_line) {
	TPUTS_TRACE("parm_delete_line");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_delete_line, n),
				n,
				NCURSES_SP_NAME(_nc_outch));
    } else {			/* if (delete_line) */
	for (i = 0; i < n; i++) {
	    NCURSES_PUTP2("delete_line", delete_line);
	}
    }

    GoTo(NCURSES_SP_ARGx ins, 0);
    UpdateAttrs(SP_PARM, blank);
    if (n == 1 && insert_line) {
	NCURSES_PUTP2("insert_line", insert_line);
    } else if (parm_insert_line) {
	TPUTS_TRACE("parm_insert_line");
	NCURSES_SP_NAME(tputs) (NCURSES_SP_ARGx
				TIPARM_1(parm_insert_line, n),
				n,
				NCURSES_SP_NAME(_nc_outch));
    } else {			/* if (insert_line) */
	for (i = 0; i < n; i++) {
	    NCURSES_PUTP2("insert_line", insert_line);
	}
    }

    return OK;
}

/*
 * Note:  some terminals require the cursor to be within the scrolling margins
 * before setting them.  Generally, the cursor must be at the appropriate end
 * of the scrolling margins when issuing an indexing operation (it is not
 * apparent whether it must also be at the left margin; we do this just to be
 * safe).  To make the related cursor movement a little faster, we use the
 * save/restore cursor capabilities if the terminal has them.
 */
NCURSES_EXPORT(int)
NCURSES_SP_NAME(_nc_scrolln) (NCURSES_SP_DCLx
			      int n,
			      int top,
			      int bot,
			      int maxy)
/* scroll region from top to bot by n lines */
{
    NCURSES_CH_T blank;
    int i;
    bool cursor_saved = FALSE;
    int res;

    TR(TRACE_MOVE, ("_nc_scrolln(%p, %d, %d, %d, %d)",
		    (void *) SP_PARM, n, top, bot, maxy));

    if (!IsValidScreen(SP_PARM))
	return (ERR);

    blank = ClrBlank(NCURSES_SP_ARGx StdScreen(SP_PARM));

#if USE_XMC_SUPPORT
    /*
     * If we scroll, we might remove a cookie.
     */
    if (magic_cookie_glitch > 0) {
	return (ERR);
    }
#endif

    if (n > 0) {		/* scroll up (forward) */
	/*
	 * Explicitly clear if stuff pushed off top of region might
	 * be saved by the terminal.
	 */
	res = scroll_csr_forward(NCURSES_SP_ARGx n, top, bot, 0, maxy, blank);

	if (res == ERR && change_scroll_region) {
	    if ((((n == 1 && scroll_forward) || parm_index)
		 && (SP_PARM->_cursrow == bot || SP_PARM->_cursrow == bot - 1))
		&& save_cursor && restore_cursor) {
		cursor_saved = TRUE;
		NCURSES_PUTP2("save_cursor", save_cursor);
	    }
	    NCURSES_PUTP2("change_scroll_region",
			  TIPARM_2(change_scroll_region, top, bot));
	    if (cursor_saved) {
		NCURSES_PUTP2("restore_cursor", restore_cursor);
	    } else {
		SP_PARM->_cursrow = SP_PARM->_curscol = -1;
	    }

	    res = scroll_csr_forward(NCURSES_SP_ARGx n, top, bot, top, bot, blank);

	    NCURSES_PUTP2("change_scroll_region",
			  TIPARM_2(change_scroll_region, 0, maxy));
	    SP_PARM->_cursrow = SP_PARM->_curscol = -1;
	}

	if (res == ERR && SP_PARM->_nc_sp_idlok)
	    res = scroll_idl(NCURSES_SP_ARGx n, top, bot - n + 1, blank);

	/*
	 * Clear the newly shifted-in text.
	 */
	if (res != ERR
	    && (non_dest_scroll_region || (memory_below && bot == maxy))) {
	    static const NCURSES_CH_T blank2 = NewChar(BLANK_TEXT);
	    if (bot == maxy && clr_eos) {
		GoTo(NCURSES_SP_ARGx bot - n + 1, 0);
		ClrToEOS(NCURSES_SP_ARGx blank2);
	    } else {
		for (i = 0; i < n; i++) {
		    GoTo(NCURSES_SP_ARGx bot - i, 0);
		    ClrToEOL(NCURSES_SP_ARGx blank2, FALSE);
		}
	    }
	}

    } else {			/* (n < 0) - scroll down (backward) */
	res = scroll_csr_backward(NCURSES_SP_ARGx -n, top, bot, 0, maxy, blank);

	if (res == ERR && change_scroll_region) {
	    if (top != 0
		&& (SP_PARM->_cursrow == top ||
		    SP_PARM->_cursrow == top - 1)
		&& save_cursor && restore_cursor) {
		cursor_saved = TRUE;
		NCURSES_PUTP2("save_cursor", save_cursor);
	    }
	    NCURSES_PUTP2("change_scroll_region",
			  TIPARM_2(change_scroll_region, top, bot));
	    if (cursor_saved) {
		NCURSES_PUTP2("restore_cursor", restore_cursor);
	    } else {
		SP_PARM->_cursrow = SP_PARM->_curscol = -1;
	    }

	    res = scroll_csr_backward(NCURSES_SP_ARGx
				      -n, top, bot, top, bot, blank);

	    NCURSES_PUTP2("change_scroll_region",
			  TIPARM_2(change_scroll_region, 0, maxy));
	    SP_PARM->_cursrow = SP_PARM->_curscol = -1;
	}

	if (res == ERR && SP_PARM->_nc_sp_idlok)
	    res = scroll_idl(NCURSES_SP_ARGx -n, bot + n + 1, top, blank);

	/*
	 * Clear the newly shifted-in text.
	 */
	if (res != ERR
	    && (non_dest_scroll_region || (memory_above && top == 0))) {
	    static const NCURSES_CH_T blank2 = NewChar(BLANK_TEXT);
	    for (i = 0; i < -n; i++) {
		GoTo(NCURSES_SP_ARGx i + top, 0);
		ClrToEOL(NCURSES_SP_ARGx blank2, FALSE);
	    }
	}
    }

    if (res == ERR)
	return (ERR);

    _nc_scroll_window(CurScreen(SP_PARM), n,
		      (NCURSES_SIZE_T) top,
		      (NCURSES_SIZE_T) bot,
		      blank);

    /* shift hash values too - they can be reused */
    NCURSES_SP_NAME(_nc_scroll_oldhash) (NCURSES_SP_ARGx n, top, bot);

    return (OK);
}

#if NCURSES_SP_FUNCS
NCURSES_EXPORT(int)
_nc_scrolln(int n, int top, int bot, int maxy)
{
    return NCURSES_SP_NAME(_nc_scrolln) (CURRENT_SCREEN, n, top, bot, maxy);
}
#endif

NCURSES_EXPORT(void)
NCURSES_SP_NAME(_nc_screen_resume) (NCURSES_SP_DCL0)
{
    assert(SP_PARM);

    /* make sure terminal is in a sane known state */
    SetAttr(SCREEN_ATTRS(SP_PARM), A_NORMAL);
    NewScreen(SP_PARM)->_clear = TRUE;

    /* reset color pairs and definitions */
    if (SP_PARM->_coloron || SP_PARM->_color_defs)
	NCURSES_SP_NAME(_nc_reset_colors) (NCURSES_SP_ARG);

    /* restore user-defined colors, if any */
    if (SP_PARM->_color_defs < 0 && !SP_PARM->_direct_color.value) {
	int n;
	SP_PARM->_color_defs = -(SP_PARM->_color_defs);
	for (n = 0; n < SP_PARM->_color_defs; ++n) {
	    if (SP_PARM->_color_table[n].init) {
		_nc_init_color(SP_PARM,
			       n,
			       SP_PARM->_color_table[n].r,
			       SP_PARM->_color_table[n].g,
			       SP_PARM->_color_table[n].b);
	    }
	}
    }

    if (exit_attribute_mode)
	NCURSES_PUTP2("exit_attribute_mode", exit_attribute_mode);
    else {
	/* turn off attributes */
	if (exit_alt_charset_mode)
	    NCURSES_PUTP2("exit_alt_charset_mode", exit_alt_charset_mode);
	if (exit_standout_mode)
	    NCURSES_PUTP2("exit_standout_mode", exit_standout_mode);
	if (exit_underline_mode)
	    NCURSES_PUTP2("exit_underline_mode", exit_underline_mode);
    }
    if (exit_insert_mode)
	NCURSES_PUTP2("exit_insert_mode", exit_insert_mode);
    if (enter_am_mode && exit_am_mode) {
	if (auto_right_margin) {
	    NCURSES_PUTP2("enter_am_mode", enter_am_mode);
	} else {
	    NCURSES_PUTP2("exit_am_mode", exit_am_mode);
	}
    }
}

#if NCURSES_SP_FUNCS
NCURSES_EXPORT(void)
_nc_screen_resume(void)
{
    NCURSES_SP_NAME(_nc_screen_resume) (CURRENT_SCREEN);
}
#endif

NCURSES_EXPORT(void)
NCURSES_SP_NAME(_nc_screen_init) (NCURSES_SP_DCL0)
{
    NCURSES_SP_NAME(_nc_screen_resume) (NCURSES_SP_ARG);
}

#if NCURSES_SP_FUNCS
NCURSES_EXPORT(void)
_nc_screen_init(void)
{
    NCURSES_SP_NAME(_nc_screen_init) (CURRENT_SCREEN);
}
#endif

/* wrap up screen handling */
NCURSES_EXPORT(void)
NCURSES_SP_NAME(_nc_screen_wrap) (NCURSES_SP_DCL0)
{
    if (SP_PARM != 0) {

	UpdateAttrs(SP_PARM, normal);
#if NCURSES_EXT_FUNCS
	if (SP_PARM->_coloron
	    && !SP_PARM->_default_color) {
	    static const NCURSES_CH_T blank = NewChar(BLANK_TEXT);
	    SP_PARM->_default_color = TRUE;
	    NCURSES_SP_NAME(_nc_do_color) (NCURSES_SP_ARGx
					   -1,
					   0,
					   FALSE,
					   NCURSES_SP_NAME(_nc_outch));
	    SP_PARM->_default_color = FALSE;

	    TINFO_MVCUR(NCURSES_SP_ARGx
			SP_PARM->_cursrow,
			SP_PARM->_curscol,
			screen_lines(SP_PARM) - 1,
			0);

	    ClrToEOL(NCURSES_SP_ARGx blank, TRUE);
	}
#endif
	if (SP_PARM->_color_defs) {
	    NCURSES_SP_NAME(_nc_reset_colors) (NCURSES_SP_ARG);
	}
    }
}

#if NCURSES_SP_FUNCS
NCURSES_EXPORT(void)
_nc_screen_wrap(void)
{
    NCURSES_SP_NAME(_nc_screen_wrap) (CURRENT_SCREEN);
}
#endif

#if USE_XMC_SUPPORT
NCURSES_EXPORT(void)
NCURSES_SP_NAME(_nc_do_xmc_glitch) (NCURSES_SP_DCLx attr_t previous)
{
    if (SP_PARM != 0) {
	attr_t chg = XMC_CHANGES(previous ^ AttrOf(SCREEN_ATTRS(SP_PARM)));

	while (chg != 0) {
	    if (chg & 1) {
		SP_PARM->_curscol += magic_cookie_glitch;
		if (SP_PARM->_curscol >= SP_PARM->_columns)
		    wrap_cursor(NCURSES_SP_ARG);
		TR(TRACE_UPDATE, ("bumped to %d,%d after cookie",
				  SP_PARM->_cursrow, SP_PARM->_curscol));
	    }
	    chg >>= 1;
	}
    }
}

#if NCURSES_SP_FUNCS
NCURSES_EXPORT(void)
_nc_do_xmc_glitch(attr_t previous)
{
    NCURSES_SP_NAME(_nc_do_xmc_glitch) (CURRENT_SCREEN, previous);
}
#endif

#endif /* USE_XMC_SUPPORT */

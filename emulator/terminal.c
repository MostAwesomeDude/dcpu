/*
 * Copyright (c) 2012, Matt Hellige
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met:
 *
 *   Redistributions of source code must retain the above copyright notice, 
 *   this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above copyright 
 *   notice, this list of conditions and the following disclaimer in the 
 *   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <ncurses.h>
#include <stdarg.h>
#include <string.h>

#include "dcpu.h"


struct term_t {
  WINDOW *border;
  WINDOW *vidwin;
  WINDOW *dbgwin;
  tstamp_t tickns;
  tstamp_t nexttick;
  tstamp_t keyns;
  tstamp_t nextkey;
  u16 keypos;
  u16 curborder;
};

static struct term_t term;

static inline u16 color(int fg, int bg) {
  return COLORS > 8
    ? fg * 16 + bg + 1
    : (fg % 8) * 8 + (bg % 8) + 1;
}

static u16 kbd_hwi(dcpu *dcpu) {
  (void)dcpu;
  dcpu_msg("kbd hwi!\n"); // TODO
  return 0;
}

static u16 lem_hwi(dcpu *dcpu) {
  (void)dcpu;
  dcpu_msg("lem hwi!\n"); // TODO
  return 0;
}

void dcpu_initterm(dcpu *dcpu) {
  term.tickns = 1000000000 / DISPLAY_HZ;
  term.nexttick = dcpu_now();
  term.keyns = 1000000000 / KBD_BAUD;
  term.nextkey = dcpu_now();
  term.keypos = 0;
  term.curborder = 0;

  // set up hardware descriptors
  device *kbd = dcpu_addhw(dcpu);
  kbd->id = 0x30cf7406;
  kbd->version = 1;
  kbd->mfr = 0x01220423;
  kbd->hwi = &kbd_hwi;
  device *lem = dcpu_addhw(dcpu);
  lem->id = 0x7349f615;
  lem->version = 0x1802;
  lem->mfr = 0x1c6c8b36;
  lem->hwi = &lem_hwi;

  // should be done prior to initscr, and it doesn't matter if we do it twice
  initscr();
  start_color();
  cbreak();
  keypad(stdscr, true);
  term.border = subwin(stdscr, 14, 36, 0, 0);
  term.vidwin = subwin(stdscr, 12, 32, 1, 2);
  term.dbgwin = subwin(stdscr, LINES - (SCR_HEIGHT+3), COLS, SCR_HEIGHT+2, 0);
  scrollok(term.dbgwin, true);
  keypad(term.dbgwin, true);

  // set up colors...
  if (COLORS > 8) {
    // nice terminals...
    int colors[] = {0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15};
    for (int i = 0; i < 16; i++)
      for (int j = 0; j < 16; j++)
        init_pair(color(i, j), colors[i], colors[j]);
  } else {
    // crappy terminals at least get something...
    int colors[] = {0, 4, 2, 6, 1, 5, 3, 7};
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 8; j++)
        init_pair(color(i, j), colors[i], colors[j]);
  }
  dcpu_msg("terminal colors: %d, pairs %d, %s change colors: \n", COLORS,
      COLOR_PAIRS, can_change_color() ? "*can*" : "*cannot*");
}

void dcpu_killterm(void) {
  endwin();
}

void readkey(dcpu *dcpu) {
  int c = getch();
  // always check for ctrl-d, even if kbd buf is full.
  if (c == 0x04) {
    // ctrl-d. exit.
    dcpu_die = true;
    return;
  }
  if (c != -1 && dcpu->ram[KBD_ADDR + term.keypos]) {
    ungetch(c);
    return;
  }
  switch (c) {
    case ERR: return; // no key. no problem.

    // remap bs and del to bs, just in case
    case KEY_BACKSPACE: c = 0x08; break;
    case 0x7f: c = 0x08; break;
  }
  dcpu->ram[KBD_ADDR + term.keypos] = c;
  term.keypos += 1;
  term.keypos %= KBD_BUFSIZ;
}

int dcpu_getstr(char *buf, int n) {
  return wgetnstr(term.dbgwin, buf, n) == OK;
}

void dcpu_msg(char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwprintw(term.dbgwin, fmt, args);
  wrefresh(term.dbgwin);
  va_end(args);
}

void dcpu_runterm(void) {
  curs_set(0);
  timeout(0);
  noecho();
}

void dcpu_dbgterm(void) {
  curs_set(1);
  timeout(-1);
  echo();
}

static void draw(u16 word, u16 row, u16 col) {
  wmove(term.vidwin, row, col);

  char letter = word & 0x7f;
  bool blink = word & 0x80;
  char fg = word >> 12;
  char bg = (word >> 8) & 0xf;

  if (!letter) letter = ' ';

  // use color_set() rather than COLOR_PAIR() since we may have more than 256
  // colors. unfortunately that doesn't really solve the problem, since every
  // linux that i can find still doesn't ship ncurses 6. how sad.
  wcolor_set(term.vidwin, color(fg, bg), NULL);
  if (blink) wattron(term.vidwin, A_BLINK);
  waddch(term.vidwin, letter);
  if (blink) wattroff(term.vidwin, A_BLINK);
}

static void draw_border(dcpu *dcpu) {
  if (term.curborder != (dcpu->ram[BORDER_ADDR] & 0xf)) {
    term.curborder = dcpu->ram[BORDER_ADDR] & 0xf;
    wbkgd(term.border, A_NORMAL | COLOR_PAIR(color(0, term.curborder)) | ' '); 
    wrefresh(term.border);
  }
}

void dcpu_redraw(dcpu *dcpu) {
  draw_border(dcpu);
  u16 *addr = &dcpu->ram[VRAM_ADDR];
  for (u16 i = 0; i < SCR_HEIGHT; i++) 
    for (u16 j = 0; j < SCR_WIDTH; j++)
      draw(*addr++, i, j);
  wrefresh(term.vidwin);
}

void dcpu_termtick(dcpu *dcpu, tstamp_t now) {
  if (now > term.nexttick) {
    dcpu_redraw(dcpu);
    term.nexttick += term.tickns;
  }

  if (now > term.nextkey) {
    readkey(dcpu);
    term.nextkey += term.keyns;
  }
}

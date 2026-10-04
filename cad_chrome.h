/* CAD.EXE window chrome (Win16 only, needs <windows.h>): Inventor-style
 * ribbon, model browser tree rows, navigation bar, viewport background and
 * status bar. Drawing code and its tables live here so cad.c's nearly full
 * far data segments stay untouched. No document state: cad.c passes the
 * current tab, menu (enable/check state of every command mirrors the menu
 * that UpdateUI maintains) and hot/pressed items.
 *
 * Icons are the res/icons bitmaps (IDB_* in res/icon_ids.h), colour index
 * 13 (magenta) transparent; a missing bitmap draws as a grey placeholder. */
#ifndef CAD_CHROME_H
#define CAD_CHROME_H

#define CHROME_RIBBON_HEIGHT 92
#define CHROME_TAB_HEIGHT 22
#define CHROME_TABS 6
#define CHROME_TAB_MODEL 0
#define CHROME_TAB_SKETCH 1
#define CHROME_TAB_ASSEMBLE 2
#define CHROME_TAB_CONSTRAIN 3
#define CHROME_TAB_MANAGE 4
#define CHROME_TAB_VIEW 5
/* Ribbon hit codes besides command IDs (> 0). */
#define CHROME_HIT_NONE 0
#define CHROME_HIT_TAB 10000 /* + tab index */
#define CHROME_HIT_FILE 10100

/* Navigation bar / home button hit codes (command IDs). */
#define CHROME_NAV_WIDTH 26

/* Browser rows: 16x16 icon + text, indented per level. */
#define CHROME_ROW_HEIGHT 18
#define CHROME_TREE_FOLDER 1   /* has an expander */
#define CHROME_TREE_OPEN 2     /* expanded folder */
#define CHROME_TREE_ERROR 4    /* red text */
#define CHROME_TREE_GREY 8     /* blocked / suppressed / informational */
#define CHROME_TREE_BOLD 16    /* active sketch, root */
#define CHROME_TREE_ROWS 1100
#define CHROME_NO_ICON 255
/* Folders whose expanded state chrome remembers. */
#define CHROME_FOLDER_PART 0
#define CHROME_FOLDER_ORIGIN 1
#define CHROME_FOLDER_CONSTRAINTS 2
#define CHROME_FOLDER_COMPONENTS 3
#define CHROME_FOLDER_WIREFRAME 4
#define CHROME_FOLDERS 5

void cad_chrome_init(HINSTANCE instance);
void cad_chrome_free(void);
/* Icon index = IDB_x - IDB_ICON_FIRST. */
void cad_chrome_icon(HDC dc, int icon, int x, int y, int disabled);

/* Ribbon: rows 0..CHROME_RIBBON_HEIGHT-1 of the client area. */
void cad_chrome_ribbon(HDC dc, int width, int tab, HMENU menu, int hot, int pressed);
int cad_chrome_ribbon_hit(int x, int y, int width, int tab);
/* Command enabled in the menu (commands absent from the menu are enabled). */
int cad_chrome_enabled(HMENU menu, int command);
/* Repaints only ribbon items a and b (hit codes; 0 = none) in place; tab
 * row hits (File, tabs, quick access) repaint the tab row. */
void cad_chrome_ribbon_items(HDC dc, int width, int tab, HMENU menu, int hot, int pressed, int a, int b);
/* Status bar prompt for a ribbon/nav hit (or NULL). */
const char *cad_chrome_help(int hit);
/* Screen-relative (client) point under the File button for TrackPopupMenu. */
void cad_chrome_file_anchor(POINT *point);

/* Viewport, in canvas coordinates. */
void cad_chrome_background(HDC dc, int width, int height);
/* Navigation bar under the ViewCube (opaque: redrawing it in place is
 * exact); mode is the active navigation command (ID_VIEW_PAN/ZOOM/ORBIT) or
 * 0. The Home button left of the cube is drawn over the scene: undoing its
 * hover needs the scene pixels under cad_chrome_home_rect. */
void cad_chrome_nav(HDC dc, int width, int height, int hot, int mode);
void cad_chrome_home(HDC dc, int width, int height, int hot);
void cad_chrome_home_rect(int width, int height, RECT *rect);

/* Scene cache: a compatible bitmap of the viewport held in a memory DC.
 * Returns NULL if GDI memory is short (callers then render directly);
 * *recreated is set when the bitmap is new (its content is undefined). */
HDC cad_chrome_scene_dc(HDC screen, int width, int height, int *recreated);
/* The cache DC if a bitmap exists, else NULL. */
HDC cad_chrome_scene_cached(void);
void cad_chrome_scene_free(void);
int cad_chrome_nav_hit(int x, int y, int width, int height);

/* Model browser tree (owner-drawn list box rows). */
void cad_chrome_tree_reset(void);
void cad_chrome_tree_set(int row, int level, int icon, int flags);
int cad_chrome_tree_flags(int row);
/* Folder of a row (CHROME_FOLDER_*) or -1. */
int cad_chrome_tree_folder(int row);
void cad_chrome_tree_set_folder(int row, int folder);
int cad_chrome_folder_open(int folder);
void cad_chrome_folder_set(int folder, int open);
/* Nonzero if client x of a folder row hits its +/- expander. */
int cad_chrome_tree_expander(int row, int x);
void cad_chrome_tree_draw(const DRAWITEMSTRUCT FAR *item);
/* Browser caption strip above the list box. */
void cad_chrome_browser_title(HDC dc, int top, int width, int height);

/* Status bar: prompt at the left, counts at the right. */
void cad_chrome_status(HDC dc, int top, int width, int height,
                       const char *prompt, const char *counts);

#endif

/* Feature tree glue for CAD.EXE; see cad_featureui.h. Every large object of
 * the feature workflow lives here (separate far data segments in the Win16
 * large model build), never in cad.c. */
#ifdef __WINDOWS__
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>
#include "cad_featureui.h"
#ifdef __WINDOWS__
#include "cad_ui_ids.h"
#endif

#define TEXT_BYTES (CAD_FEAT_TEXT_MAX + 1)

CadFuiDialog cad_fui_dialog;

static CadFeatures doc;
static CadFeatOutput out;
static CadUserParams *params;
static unsigned activeSketch, errorCount;
/* Exact serialized forms: saved document, dirty scratch, load backup. */
static char savedText[TEXT_BYTES];
static char scratchText[TEXT_BYTES];
static char backupText[TEXT_BYTES];
static unsigned backupLength, backupUndoCount, backupUndoNext, backupSketch;
/* Feature undo ring: slot undoNext receives the snapshot taken before an
 * edit; it becomes part of the ring only when the edit is kept. */
static char undoText[CAD_FUI_UNDO_DEPTH][TEXT_BYTES];
static unsigned undoLength[CAD_FUI_UNDO_DEPTH];
static unsigned undoCount, undoNext;
/* Polygon field split into expressions (apply only). */
static char polyText[CAD_FUI_POINTS_MAX + 1];

static const char *const fieldNames[] = {
    "", "name", "plane", "offset", "profile", "sketch", "distance",
    "direction", "axis", "segments", "sides"
};
static const char *const kindNames[] = { "", "Rectangle", "Circle", "Polygon" };

static unsigned WriteAll(char *buffer)
{
    unsigned length = 0;
    if (cad_features_write_text(&doc, buffer, TEXT_BYTES, &length) != CAD_FEAT_OK)
        length = 0;
    buffer[length] = 0;
    return length;
}
static void Snapshot(void)
{
    undoLength[undoNext] = WriteAll(undoText[undoNext]);
}
static void Keep(void)
{
    undoNext = (undoNext + 1) % CAD_FUI_UNDO_DEPTH;
    if (undoCount < CAD_FUI_UNDO_DEPTH) ++undoCount;
}
static void Rollback(void)
{
    cad_features_read_text(&doc, undoText[undoNext], undoLength[undoNext], NULL);
}

void cad_fui_init(CadUserParams *table)
{
    params = table;
    cad_fui_reset();
}
void cad_fui_reset(void)
{
    cad_features_init(&doc);
    activeSketch = 0;
    undoCount = undoNext = 0;
    cad_fui_regenerate();
}
const CadFeatures *cad_fui_features(void) { return &doc; }
const CadFeatOutput *cad_fui_output(void) { return &out; }
unsigned cad_fui_regenerate(void)
{
    unsigned errors = 0;
    if (!params || cad_features_regenerate(&doc, params, &out, &errors) != CAD_FEAT_OK) {
        memset(&out, 0, sizeof(out));
        errors = 0;
    }
    errorCount = errors;
    return errors;
}
unsigned cad_fui_errors(void) { return errorCount; }
unsigned cad_fui_count(void) { return cad_features_count(&doc); }
int cad_fui_bounds(long min[3], long max[3])
{
    return cad_features_bounds(&out, min, max);
}

unsigned cad_fui_active_sketch(void) { return activeSketch; }
void cad_fui_set_active_sketch(unsigned id)
{
    activeSketch = cad_fui_type(id) == CAD_FEAT_SKETCH ? id : 0;
}
unsigned cad_fui_validate(void)
{
    if (activeSketch && cad_fui_type(activeSketch) != CAD_FEAT_SKETCH) activeSketch = 0;
    return activeSketch;
}
unsigned cad_fui_last_sketch(void)
{
    unsigned i, id = 0;
    for (i = 0; i < doc.count; ++i)
        if (doc.items[i].type == CAD_FEAT_SKETCH) id = doc.items[i].id;
    return id;
}
const char *cad_fui_name(unsigned id)
{
    int i = cad_features_index(&doc, id);
    return i < 0 ? "" : doc.items[i].name;
}
CadFeatType cad_fui_type(unsigned id)
{
    int i = id ? cad_features_index(&doc, id) : -1;
    return i < 0 ? (CadFeatType)0 : (CadFeatType)doc.items[i].type;
}
static const CadFeatHealth *Health(unsigned id)
{
    int i = cad_features_index(&doc, id);
    return i < 0 || (unsigned)i >= out.feature_count ? NULL : &out.health[i];
}

/* ---- messages ---- */
/* "<name>: <field>: <message>." (the name only if id is nonzero). */
static void HealthText(char *text, unsigned id, const CadFeatHealth *h)
{
    char where[48];
    where[0] = 0;
    if (h->field == CAD_FEAT_FIELD_PROFILE && h->profile >= 0)
        sprintf(where, "profile %d: ", h->profile + 1);
    else if (h->field > CAD_FEAT_FIELD_NONE && h->field <= CAD_FEAT_FIELD_SIDES)
        sprintf(where, "%s: ", fieldNames[h->field]);
    text[0] = 0;
    if (id) sprintf(text, "%.31s: ", cad_fui_name(id));
    if (h->error == CAD_FEAT_EVAL)
        sprintf(text + strlen(text), "%s%.80s (column %u).", where, h->message, h->offset + 1);
    else sprintf(text + strlen(text), "%s%.100s.", where, h->message);
}
static void EditMessage(char *text, const CadFeatError *e)
{
    const char *other = cad_fui_name(e->feature_id);
    if (e->status == CAD_FEAT_IN_USE)
        sprintf(text, "In use: %.31s depends on it. Delete or edit %.31s first.", other, other);
    else if (e->status == CAD_FEAT_ORDER)
        sprintf(text, "Cannot reorder: a feature must stay after its sketch (%.31s).", other);
    else if (e->field > CAD_FEAT_FIELD_NONE && e->field <= CAD_FEAT_FIELD_SIDES)
        sprintf(text, "%s: %.100s.", fieldNames[e->field], e->message);
    else sprintf(text, "%.120s.", e->message);
}

/* ---- dialog support ---- */
static void SetField(int i, const char *text)
{
    strncpy(cad_fui_dialog.fields[i], text, CAD_FEAT_EXPR_MAX);
    cad_fui_dialog.fields[i][CAD_FEAT_EXPR_MAX] = 0;
}
static void SetNumber(int i, long value)
{
    sprintf(cad_fui_dialog.fields[i], "%ld", value);
}
static void SetProfileField(int profile)
{
    if (profile == CAD_FEAT_ALL) SetField(0, "ALL");
    else SetNumber(0, (long)profile + 1);
}
static int Fail(const char *text)
{
    strcpy(cad_fui_dialog.message, text);
    return 0;
}
static int PrepareEdit(unsigned target, int profile)
{
    CadFuiDialog *d = &cad_fui_dialog;
    CadFeatInfo info;
    CadFeatProfileInfo p;
    unsigned i;
    if (cad_features_get(&doc, target, &info) != CAD_FEAT_OK) return Fail("Select a feature in the browser first.");
    d->target = target;
    d->profile = profile;
    if (profile >= 0) {
        if (cad_features_profile(&doc, target, (unsigned)profile, &p) != CAD_FEAT_OK)
            return Fail("Profile not found.");
        d->kind = p.kind == CAD_FEAT_RECT ? CAD_FUI_RECT : p.kind == CAD_FEAT_CIRCLE ? CAD_FUI_CIRCLE : CAD_FUI_POLY;
        sprintf(d->title, "Edit %s %d (%.31s)", kindNames[p.kind], profile + 1, info.name);
        if (d->kind == CAD_FUI_POLY) {
            d->points[0] = 0;
            for (i = 0; i < p.expr_count; ++i) {
                if (i) strcat(d->points, i % 2 ? "," : ";");
                strcat(d->points, p.exprs[i]);
            }
        } else {
            for (i = 0; i < p.expr_count && i < 4; ++i) SetField((int)i, p.exprs[i]);
            if (d->kind == CAD_FUI_CIRCLE) SetNumber(3, (long)p.sides);
        }
        return 1;
    }
    d->profile = -1;
    sprintf(d->title, "Edit %.31s", info.name);
    if (info.type == CAD_FEAT_SKETCH) {
        d->kind = CAD_FUI_SKETCH; d->option = (int)info.plane; SetField(0, info.offset);
    } else if (info.type == CAD_FEAT_EXTRUDE) {
        d->kind = CAD_FUI_EXTRUDE; d->sketch = info.sketch_id; d->option = (int)info.direction;
        SetProfileField(info.profile); SetField(1, info.distance);
    } else {
        d->kind = CAD_FUI_REVOLVE; d->sketch = info.sketch_id; d->option = (int)info.axis;
        SetProfileField(info.profile); SetNumber(1, (long)info.segments);
    }
    return 1;
}
int cad_fui_prepare(int kind, unsigned target, int profile)
{
    CadFuiDialog *d = &cad_fui_dialog;
    memset(d, 0, sizeof(*d));
    d->profile = -1;
    d->created_profile = -1;
    if (!kind) return PrepareEdit(target, profile);
    d->kind = kind;
    switch (kind) {
    case CAD_FUI_SKETCH:
        strcpy(d->title, "Start 2D Sketch"); SetField(0, "0"); return 1;
    case CAD_FUI_RECT: case CAD_FUI_CIRCLE: case CAD_FUI_POLY:
        if (!cad_fui_validate())
            return Fail("No sketch is active. Use Sketch > Start 2D Sketch first, then add Rectangle, Circle or Polygon profiles.");
        d->target = activeSketch;
        sprintf(d->title, "%s (%.31s)", kindNames[kind - CAD_FUI_RECT + 1], cad_fui_name(activeSketch));
        if (kind == CAD_FUI_RECT) { SetField(0, "0"); SetField(1, "0"); SetField(2, "100"); SetField(3, "60"); }
        else if (kind == CAD_FUI_CIRCLE) { SetField(0, "0"); SetField(1, "0"); SetField(2, "50"); SetNumber(3, CAD_FEAT_CIRCLE_DEFAULT); }
        else strcpy(d->points, "0,0;100,0;50,80");
        return 1;
    case CAD_FUI_EXTRUDE: case CAD_FUI_REVOLVE:
        d->sketch = cad_fui_last_sketch();
        if (!d->sketch) return Fail("Create a sketch with a profile first (Sketch > Start 2D Sketch).");
        strcpy(d->title, kind == CAD_FUI_EXTRUDE ? "Extrude" : "Revolve");
        SetField(0, "ALL");
        if (kind == CAD_FUI_EXTRUDE) SetField(1, "10");
        else SetNumber(1, 16);
        return 1;
    case CAD_FUI_RENAME:
        if (!cad_fui_type(target)) return Fail("Select a feature in the browser first.");
        d->target = target;
        sprintf(d->title, "Rename %.31s", cad_fui_name(target));
        SetField(0, cad_fui_name(target));
        return 1;
    }
    return Fail("Unknown feature command.");
}
unsigned cad_fui_sketch_choice(unsigned n)
{
    int limit = cad_fui_dialog.target ? cad_features_index(&doc, cad_fui_dialog.target) : -1;
    unsigned i;
    for (i = 0; i < doc.count && (limit < 0 || (int)i < limit); ++i)
        if (doc.items[i].type == CAD_FEAT_SKETCH && !n--) return doc.items[i].id;
    return 0;
}

/* Trims spaces in place; returns the string. */
static char *Trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') ++s;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = 0;
    return s;
}
/* Strict small integer: digits only (after trimming), lo..hi. */
static int SmallNumber(char *text, int lo, int hi, int *value)
{
    char *s = Trim(text);
    int n = 0;
    if (!*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > 1000) return 0;
        n = n * 10 + (*s - '0');
    }
    if (n < lo || n > hi) return 0;
    *value = n;
    return 1;
}
static int ProfileSelector(char *text, int *profile)
{
    char *s = Trim(text);
    int n;
    if ((s[0] == 'A' || s[0] == 'a') && (s[1] == 'L' || s[1] == 'l') &&
        (s[2] == 'L' || s[2] == 'l') && !s[3]) { *profile = CAD_FEAT_ALL; return 1; }
    if (!SmallNumber(s, 1, CAD_FEAT_SKETCH_PROFILES, &n)) return 0;
    *profile = n - 1;
    return 1;
}
/* "x,y;x,y;..." -> 6..32 trimmed expressions in polyText. */
static int SplitPoints(const char *text, const char **exprs, unsigned *count)
{
    char *p, *start;
    unsigned n = 0;
    strcpy(polyText, text);
    p = start = polyText;
    for (;;) {
        if (*p == ',' || *p == ';' || !*p) {
            char sep = *p;
            if (n == CAD_FEAT_PROFILE_EXPRS) return 0;
            /* x must end with ',', y with ';' or the end */
            if ((n % 2 == 0) != (sep == ',')) return 0;
            *p = 0;
            exprs[n] = Trim(start);
            if (!*exprs[n]) return 0;
            ++n;
            if (!sep) break;
            start = p + 1;
        }
        ++p;
    }
    if (n < 2 * CAD_FEAT_POLY_MIN) return 0;
    *count = n;
    return 1;
}

int cad_fui_apply(void)
{
    CadFuiDialog *d = &cad_fui_dialog;
    CadFeatError error;
    CadFeatStatus status = CAD_FEAT_ARGUMENT;
    const char *exprs[CAD_FEAT_PROFILE_EXPRS];
    const CadFeatHealth *h;
    unsigned count = 0, id = 0, index = 0, check;
    int i, sides = 0, profile = CAD_FEAT_ALL, segments = 0, wasError = 0;
    CadFeatProfileKind kind = CAD_FEAT_RECT;
    d->message[0] = 0;
    d->created = 0;
    d->created_profile = -1;
    for (i = 0; i < 4; ++i) {
        char *s = Trim(d->fields[i]);
        if (s != d->fields[i]) memmove(d->fields[i], s, strlen(s) + 1);
    }
    /* Field syntax that the feature module does not see as text. */
    if (d->kind == CAD_FUI_CIRCLE && !SmallNumber(d->fields[3], CAD_FEAT_CIRCLE_MIN, CAD_FEAT_CIRCLE_MAX, &sides))
        return Fail("Sides must be a whole number 8..32.");
    if ((d->kind == CAD_FUI_EXTRUDE || d->kind == CAD_FUI_REVOLVE) && cad_fui_type(d->sketch) != CAD_FEAT_SKETCH)
        return Fail("Choose a sketch.");
    if ((d->kind == CAD_FUI_EXTRUDE || d->kind == CAD_FUI_REVOLVE) && !ProfileSelector(d->fields[0], &profile))
        return Fail("Profile must be ALL or a profile number 1..8.");
    if (d->kind == CAD_FUI_REVOLVE && !SmallNumber(d->fields[1], CAD_FEAT_REVOLVE_MIN, CAD_FEAT_REVOLVE_MAX, &segments))
        return Fail("Segments must be a whole number 12..24.");
    if (d->kind == CAD_FUI_POLY && !SplitPoints(d->points, exprs, &count))
        return Fail("Points: enter 3..16 points as x,y;x,y;... (each value an expression).");
    if (d->kind == CAD_FUI_RECT) { for (i = 0; i < 4; ++i) exprs[i] = d->fields[i]; count = 4; kind = CAD_FEAT_RECT; }
    if (d->kind == CAD_FUI_CIRCLE) { for (i = 0; i < 3; ++i) exprs[i] = d->fields[i]; count = 3; kind = CAD_FEAT_CIRCLE; }
    if (d->kind == CAD_FUI_POLY) kind = CAD_FEAT_POLY;
    if (d->kind != CAD_FUI_CIRCLE) sides = 0;
    check = d->target;
    h = check ? Health(check) : NULL;
    wasError = h && h->state == CAD_FEAT_STATE_ERROR;
    Snapshot();
    switch (d->kind) {
    case CAD_FUI_SKETCH:
        if (d->option < 0 || d->option > 2) return Fail("Choose a plane.");
        status = d->target ? cad_features_edit_sketch(&doc, d->target, (CadFeatPlane)d->option, d->fields[0], &error)
            : cad_features_add_sketch(&doc, NULL, (CadFeatPlane)d->option, d->fields[0], &id, &error);
        break;
    case CAD_FUI_RECT: case CAD_FUI_CIRCLE: case CAD_FUI_POLY:
        if (d->profile >= 0)
            status = cad_features_edit_profile(&doc, d->target, (unsigned)d->profile, kind, exprs, count,
                (unsigned)sides, &error);
        else {
            status = cad_features_add_profile(&doc, d->target, kind, exprs, count, (unsigned)sides, &index, &error);
            if (status == CAD_FEAT_OK) { d->created = d->target; d->created_profile = (int)index; }
        }
        break;
    case CAD_FUI_EXTRUDE:
        if (d->option < 0 || d->option > 2) return Fail("Choose a direction.");
        status = d->target ? cad_features_edit_extrude(&doc, d->target, d->sketch, profile,
                (CadFeatDirection)d->option, d->fields[1], &error)
            : cad_features_add_extrude(&doc, NULL, d->sketch, profile, (CadFeatDirection)d->option,
                d->fields[1], &id, &error);
        break;
    case CAD_FUI_REVOLVE:
        if (d->option < 0 || d->option > 1) return Fail("Choose an axis.");
        status = d->target ? cad_features_edit_revolve(&doc, d->target, d->sketch, profile,
                (CadFeatAxis)d->option, (unsigned)segments, &error)
            : cad_features_add_revolve(&doc, NULL, d->sketch, profile, (CadFeatAxis)d->option,
                (unsigned)segments, &id, &error);
        break;
    case CAD_FUI_RENAME:
        status = cad_features_rename(&doc, d->target, d->fields[0], &error);
        break;
    default:
        return Fail("Unknown feature command.");
    }
    if (status != CAD_FEAT_OK) {
        if (status == CAD_FEAT_REFERENCE && (d->kind == CAD_FUI_EXTRUDE || d->kind == CAD_FUI_REVOLVE))
            return Fail("Profile: the sketch has no profile with that number (use ALL or 1..n).");
        if (status == CAD_FEAT_FULL)
            return Fail("Feature capacity reached (24 features, 8 profiles per sketch, 32 profiles in all).");
        if (status == CAD_FEAT_CAPACITY)
            return Fail("The feature tree would exceed its 8 KiB text limit.");
        EditMessage(d->message, &error);
        return 0;
    }
    if (id) { d->created = id; check = id; }
    cad_fui_regenerate();
    h = Health(check);
    if (h && h->state == CAD_FEAT_STATE_ERROR && !wasError) {
        HealthText(d->message, check, h);
        Rollback();
        cad_fui_regenerate();
        d->created = 0; d->created_profile = -1;
        return 0;
    }
    Keep();
    if (d->kind == CAD_FUI_SKETCH && id) activeSketch = id;
    return 1;
}

/* ---- browser commands ---- */
static int Done(CadFeatStatus status, const CadFeatError *error, char *message)
{
    if (status != CAD_FEAT_OK) { EditMessage(message, error); return 0; }
    Keep();
    cad_fui_validate();
    cad_fui_regenerate();
    return 1;
}
int cad_fui_remove(unsigned id, int profile, char *message)
{
    CadFeatError error;
    Snapshot();
    return Done(profile >= 0 ? cad_features_remove_profile(&doc, id, (unsigned)profile, &error)
        : cad_features_remove(&doc, id, &error), &error, message);
}
int cad_fui_toggle_suppress(unsigned id, char *message)
{
    CadFeatError error;
    int i = cad_features_index(&doc, id);
    if (i < 0) { strcpy(message, "Select a feature in the browser first."); return 0; }
    Snapshot();
    return Done(cad_features_suppress(&doc, id, !doc.items[i].suppressed, &error), &error, message);
}
int cad_fui_move(unsigned id, int up, char *message)
{
    CadFeatError error;
    CadFeatStatus status;
    Snapshot();
    status = up ? cad_features_move_up(&doc, id, &error) : cad_features_move_down(&doc, id, &error);
    if (status == CAD_FEAT_ARGUMENT) {
        strcpy(message, up ? "The feature is already first." : "The feature is already last.");
        return 0;
    }
    return Done(status, &error, message);
}
int cad_fui_can_undo(void) { return undoCount != 0; }
int cad_fui_undo(char *message)
{
    unsigned slot;
    if (!undoCount) { strcpy(message, "No feature edit to undo."); return 0; }
    slot = (undoNext + CAD_FUI_UNDO_DEPTH - 1) % CAD_FUI_UNDO_DEPTH;
    if (cad_features_read_text(&doc, undoText[slot], undoLength[slot], NULL) != CAD_FEAT_OK) {
        strcpy(message, "Feature undo failed.");
        return 0;
    }
    undoNext = slot;
    --undoCount;
    cad_fui_validate();
    cad_fui_regenerate();
    return 1;
}

/* ---- browser rows ---- */
static void FeatureRow(unsigned i, char *text, unsigned capacity)
{
    CadFeatInfo info;
    const CadFeatHealth *h;
    char line[CAD_FEAT_DESC_MAX + CAD_FUI_MESSAGE + 32];
    unsigned n;
    line[0] = 0;
    if (cad_features_at(&doc, i, &info) == CAD_FEAT_OK) {
        /* Markers lead the row so the narrow browser still shows them. */
        h = Health(info.id);
        if (info.suppressed || (h && h->state == CAD_FEAT_STATE_SUPPRESSED)) strcpy(line, "(suppressed) ");
        else if (h && h->state == CAD_FEAT_STATE_BLOCKED) strcpy(line, "(blocked) ");
        else if (h && h->state == CAD_FEAT_STATE_ERROR) strcpy(line, "(!) ");
        strcat(line, info.description);
        n = strlen(line);
        if (n > 13 && !strcmp(line + n - 13, " [suppressed]")) line[n - 13] = 0;
        if (info.id == activeSketch) strcat(line, " [sketch mode]");
        if (h && h->state == CAD_FEAT_STATE_ERROR) {
            strcat(line, " - ");
            HealthText(line + strlen(line), 0, h);
        }
    }
    strncpy(text, line, capacity - 1);
    text[capacity - 1] = 0;
}
int cad_fui_row(unsigned row, char *text, unsigned capacity, unsigned *feature, int *profile)
{
    unsigned i, j, profiles;
    CadFeatProfileInfo p;
    const CadFeatHealth *h;
    char line[48];
    for (i = 0; i < doc.count; ++i) {
        if (!row--) {
            *feature = doc.items[i].id; *profile = -1;
            FeatureRow(i, text, capacity);
            return 1;
        }
        if (doc.items[i].type != CAD_FEAT_SKETCH) continue;
        profiles = 0;
        for (j = 0; j < doc.profile_count; ++j) if (doc.profiles[j].owner == doc.items[i].id) ++profiles;
        if (row >= profiles) { row -= profiles; continue; }
        *feature = doc.items[i].id; *profile = (int)row;
        line[0] = 0;
        if (cad_features_profile(&doc, *feature, row, &p) == CAD_FEAT_OK) {
            h = Health(*feature);
            sprintf(line, "  %s%s %u", h && h->state == CAD_FEAT_STATE_ERROR && h->profile == (int)row ? "(!) " : "",
                kindNames[p.kind], row + 1);
        }
        strncpy(text, line, capacity - 1);
        text[capacity - 1] = 0;
        return 1;
    }
    return 0;
}

/* ---- persistence and dirty state ---- */
unsigned cad_fui_text(char *buffer, unsigned capacity, int *overflow)
{
    unsigned length = 0;
    if (overflow) *overflow = 0;
    if (capacity) buffer[0] = 0;
    if (!doc.count) return 0;
    if (cad_features_write_text(&doc, buffer, capacity, &length) != CAD_FEAT_OK || length >= capacity) {
        if (overflow) *overflow = 1;
        if (capacity) buffer[0] = 0;
        return 0;
    }
    buffer[length] = 0;
    return length;
}
int cad_fui_read(const char *text, unsigned length, char *message)
{
    CadFeatError error;
    if (cad_features_read_text(&doc, text, length, &error) != CAD_FEAT_OK) {
        sprintf(message, "[FEATURES] line %u: %.120s", error.line, error.message);
        return 0;
    }
    activeSketch = 0;
    undoCount = undoNext = 0;
    cad_fui_regenerate();
    return 1;
}
void cad_fui_backup(void)
{
    backupLength = WriteAll(backupText);
    backupUndoCount = undoCount; backupUndoNext = undoNext; backupSketch = activeSketch;
}
void cad_fui_restore(void)
{
    cad_features_read_text(&doc, backupText, backupLength, NULL);
    undoCount = backupUndoCount; undoNext = backupUndoNext; activeSketch = backupSketch;
    cad_fui_validate();
    cad_fui_regenerate();
}
void cad_fui_mark_saved(void) { WriteAll(savedText); }
int cad_fui_dirty(void)
{
    WriteAll(scratchText);
    return strcmp(scratchText, savedText) != 0;
}

/* ---- export / drawing ---- */
static int AddPoint(const long p[3], int first, CadExportVertex *ev, CadDrawingPoint *dp,
                    int *count, int max)
{
    int i;
    for (i = first; i < *count; ++i) {
        if (ev && ev[i].x == (int)p[0] && ev[i].y == (int)p[1] && ev[i].z == (int)p[2]) return i;
        if (!ev && dp[i].x == p[0] && dp[i].y == p[1] && dp[i].z == p[2]) return i;
    }
    if (*count >= max) return -1;
    if (ev) { ev[i].x = (int)p[0]; ev[i].y = (int)p[1]; ev[i].z = (int)p[2]; }
    if (dp) { dp[i].x = p[0]; dp[i].y = p[1]; dp[i].z = p[2]; }
    return (*count)++;
}
int cad_fui_materialize(CadExportVertex *ev, CadDrawingPoint *dp, int *vertex_count, int max_vertices,
    CadExportEdge *ee, CadDrawingEdge *de, int *edge_count, int max_edges)
{
    unsigned f, s;
    int first, a, b, skipped = 0;
    const CadFeatHealth *h;
    const CadFeatSegment *g;
    if (!ev && !dp) return 0;
    for (f = 0; f < out.feature_count; ++f) {
        h = &out.health[f];
        first = *vertex_count;
        for (s = h->first_segment; s < h->first_segment + h->segment_count && s < out.segment_count; ++s) {
            g = &out.segments[s];
            if (g->kind != CAD_FEAT_SEG_EDGE) continue;
            if (*edge_count >= max_edges) { ++skipped; continue; }
            a = AddPoint(g->a, first, ev, dp, vertex_count, max_vertices);
            b = a < 0 ? -1 : AddPoint(g->b, first, ev, dp, vertex_count, max_vertices);
            if (a < 0 || b < 0) { ++skipped; continue; }
            if (ee) { ee[*edge_count].a = a; ee[*edge_count].b = b; ee[*edge_count].style = CAD_EXPORT_WALL; }
            if (de) { de[*edge_count].a = a; de[*edge_count].b = b; }
            ++*edge_count;
        }
    }
    return skipped;
}

unsigned long cad_fui_telemetry(int shaded)
{
    unsigned long t, extent = 0;
    long lo[3], hi[3];
    int bounds = cad_features_bounds(&out, lo, hi);
    if (bounds) extent = hi[0] - lo[0] > 1023 ? 1023UL : (unsigned long)(hi[0] - lo[0]);
    t = (unsigned long)(doc.count & 31) | ((unsigned long)(errorCount & 31) << 5) |
        ((unsigned long)(activeSketch ? 1 : 0) << 10) | ((unsigned long)(shaded ? 1 : 0) << 11) |
        (extent << 12) | ((unsigned long)(out.wireframe_truncated || out.shading_truncated ? 1 : 0) << 22) |
        ((unsigned long)(bounds ? 1 : 0) << 23);
    return t | (((t ^ (t >> 8) ^ (t >> 16) ^ 0x5cUL) & 255UL) << 24);
}

#ifdef __WINDOWS__
/* ---- native dialogs (IDD_FEAT_*) ---- */
static void ShowItem(HWND dlg, int id, BOOL show)
{
    ShowWindow(GetDlgItem(dlg, id), show ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(dlg, id), show);
}
static void InitEdit(HWND dlg, int id, const char *text, int limit)
{
    SendDlgItemMessage(dlg, id, EM_LIMITTEXT, limit, 0);
    SetDlgItemText(dlg, id, text);
}
static void FocusEdit(HWND dlg, int id)
{
    SetFocus(GetDlgItem(dlg, id));
    SendDlgItemMessage(dlg, id, EM_SETSEL, 0, MAKELPARAM(0, -1));
}
static void InitDialog(HWND dlg)
{
    CadFuiDialog *d = &cad_fui_dialog;
    static const char *const rectLabels[4] = { "Center &u", "Center &v", "&Width", "&Height" };
    static const char *const circleLabels[4] = { "Center &u", "Center &v", "&Radius", "&Sides (8..32)" };
    unsigned n, id;
    int i, row;
    SetWindowText(dlg, d->title);
    switch (d->kind) {
    case CAD_FUI_SKETCH:
        CheckRadioButton(dlg, IDC_FEAT_OPT1, IDC_FEAT_OPT3, IDC_FEAT_OPT1 + d->option);
        InitEdit(dlg, IDC_VALUE1, d->fields[0], CAD_FEAT_EXPR_MAX);
        FocusEdit(dlg, IDC_VALUE1);
        break;
    case CAD_FUI_RECT: case CAD_FUI_CIRCLE: case CAD_FUI_POLY:
        for (i = 0; i < 4; ++i) {
            ShowItem(dlg, IDC_LABEL1 + i, d->kind != CAD_FUI_POLY);
            ShowItem(dlg, IDC_VALUE1 + i, d->kind != CAD_FUI_POLY);
            SetDlgItemText(dlg, IDC_LABEL1 + i, d->kind == CAD_FUI_RECT ? rectLabels[i] : circleLabels[i]);
            InitEdit(dlg, IDC_VALUE1 + i, d->fields[i], CAD_FEAT_EXPR_MAX);
        }
        ShowItem(dlg, IDC_FEAT_POINTS, d->kind == CAD_FUI_POLY);
        InitEdit(dlg, IDC_FEAT_POINTS, d->points, CAD_FUI_POINTS_MAX);
        if (d->kind == CAD_FUI_POLY)
            SetDlgItemText(dlg, IDC_FEAT_HINT, "Points u,v;u,v;... (3..16 points, each value an expression). The closing edge is implicit.");
        FocusEdit(dlg, d->kind == CAD_FUI_POLY ? IDC_FEAT_POINTS : IDC_VALUE1);
        break;
    case CAD_FUI_EXTRUDE: case CAD_FUI_REVOLVE:
        SendDlgItemMessage(dlg, IDC_FEAT_SKETCH, CB_RESETCONTENT, 0, 0);
        for (n = 0; (id = cad_fui_sketch_choice(n)) != 0; ++n) {
            row = (int)SendDlgItemMessage(dlg, IDC_FEAT_SKETCH, CB_ADDSTRING, 0, (LPARAM)(LPSTR)cad_fui_name(id));
            if (row < 0) continue;
            SendDlgItemMessage(dlg, IDC_FEAT_SKETCH, CB_SETITEMDATA, row, (LPARAM)(DWORD)id);
            if (id == d->sketch) SendDlgItemMessage(dlg, IDC_FEAT_SKETCH, CB_SETCURSEL, row, 0);
        }
        InitEdit(dlg, IDC_VALUE1, d->fields[0], CAD_FEAT_EXPR_MAX);
        InitEdit(dlg, IDC_VALUE2, d->fields[1], CAD_FEAT_EXPR_MAX);
        if (d->kind == CAD_FUI_REVOLVE) {
            SetDlgItemText(dlg, IDC_LABEL2, "Se&gments (12..24)");
            SetDlgItemText(dlg, IDC_FEAT_GROUP, "Axis (sketch u or v):");
            SetDlgItemText(dlg, IDC_FEAT_OPT1, "Axis &U");
            SetDlgItemText(dlg, IDC_FEAT_OPT2, "Axis &V");
            ShowItem(dlg, IDC_FEAT_OPT3, FALSE);
        }
        CheckRadioButton(dlg, IDC_FEAT_OPT1, IDC_FEAT_OPT3, IDC_FEAT_OPT1 + d->option);
        FocusEdit(dlg, IDC_VALUE2);
        break;
    default:
        InitEdit(dlg, IDC_VALUE1, d->fields[0], CAD_FEAT_NAME_MAX);
        FocusEdit(dlg, IDC_VALUE1);
        break;
    }
}
static void ReadDialog(HWND dlg)
{
    CadFuiDialog *d = &cad_fui_dialog;
    int i, row;
    for (i = 0; i < 4; ++i)
        if (GetDlgItem(dlg, IDC_VALUE1 + i))
            GetDlgItemText(dlg, IDC_VALUE1 + i, d->fields[i], sizeof(d->fields[i]));
    if (GetDlgItem(dlg, IDC_FEAT_POINTS))
        GetDlgItemText(dlg, IDC_FEAT_POINTS, d->points, sizeof(d->points));
    for (i = 0; i < 3; ++i)
        if (GetDlgItem(dlg, IDC_FEAT_OPT1 + i) && IsDlgButtonChecked(dlg, IDC_FEAT_OPT1 + i)) d->option = i;
    if (GetDlgItem(dlg, IDC_FEAT_SKETCH)) {
        row = (int)SendDlgItemMessage(dlg, IDC_FEAT_SKETCH, CB_GETCURSEL, 0, 0);
        d->sketch = row < 0 ? 0 : (unsigned)SendDlgItemMessage(dlg, IDC_FEAT_SKETCH, CB_GETITEMDATA, row, 0);
    }
}
BOOL FAR PASCAL __export FeatureDlg(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (msg == WM_INITDIALOG) { InitDialog(dlg); return FALSE; }
    if (msg != WM_COMMAND) return FALSE;
    if (wp == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
    if (wp != IDOK) return FALSE;
    ReadDialog(dlg);
    if (!cad_fui_apply()) {
        MessageBox(dlg, cad_fui_dialog.message, "CAD", MB_OK | MB_ICONEXCLAMATION);
        return TRUE;
    }
    EndDialog(dlg, IDOK);
    return TRUE;
}

/* ---- shaded feature bodies ---- */
#define FUI_MATERIALS 6
static HBRUSH brushes[FUI_MATERIALS * 8];
static const unsigned char palette[FUI_MATERIALS][3] = {
    {170, 190, 220}, {220, 190, 140}, {160, 210, 160},
    {220, 160, 160}, {190, 170, 220}, {160, 215, 215}
};
typedef struct FuiPaint { HDC dc; BOOL flip, any; int width, height; } FuiPaint;

static int FeatureTriangle(void *context, int x1, int y1, int x2, int y2,
                           int x3, int y3, unsigned short shadeIndex)
{
    FuiPaint *c = (FuiPaint *)context;
    POINT points[3];
    HBRUSH brush = shadeIndex < FUI_MATERIALS * 8 ? brushes[shadeIndex] : NULL;
    if (c->flip) {
        x1 = max(0, min(c->width - 1, c->width - x1));
        x2 = max(0, min(c->width - 1, c->width - x2));
        x3 = max(0, min(c->width - 1, c->width - x3));
        y1 = max(0, min(c->height - 1, c->height - y1));
        y2 = max(0, min(c->height - 1, c->height - y2));
        y3 = max(0, min(c->height - 1, c->height - y3));
    }
    SelectObject(c->dc, brush ? brush : (HBRUSH)GetStockObject(LTGRAY_BRUSH));
    points[0].x = x1; points[0].y = y1;
    points[1].x = x2; points[1].y = y2;
    points[2].x = x3; points[2].y = y3;
    c->any = TRUE;
    return Polygon(c->dc, points, 3) != 0;
}
BOOL cad_fui_draw_shaded(HDC dc, const CadMeshViewCamera *camera,
    const CadMeshViewOrtho *ortho, BOOL flip)
{
    CadMeshViewSink sink;
    FuiPaint paint;
    HGDIOBJ oldPen, oldBrush;
    CadMeshViewStatus result;
    int material, level, factor;
    if (!out.face_count) return FALSE;
    for (material = 0; material < FUI_MATERIALS; ++material)
        for (level = 0; level < 8; ++level) if (!brushes[material * 8 + level]) {
            factor = 45 + level * 7;
            brushes[material * 8 + level] = CreateSolidBrush(RGB(
                (int)((long)palette[material][0] * factor / 100),
                (int)((long)palette[material][1] * factor / 100),
                (int)((long)palette[material][2] * factor / 100)));
        }
    paint.dc = dc; paint.flip = flip; paint.any = FALSE;
    paint.width = camera->width; paint.height = camera->height;
    sink.context = &paint; sink.triangle = FeatureTriangle;
    oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    oldBrush = SelectObject(dc, GetStockObject(LTGRAY_BRUSH));
    result = cad_meshview_render_orthographic(out.vertices, (int)out.vertex_count,
        out.faces, (int)out.face_count, camera, &sink, ortho);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    return result == CAD_MESHVIEW_OK && paint.any;
}
void cad_fui_free_brushes(void)
{
    int i;
    for (i = 0; i < FUI_MATERIALS * 8; ++i)
        if (brushes[i]) { DeleteObject(brushes[i]); brushes[i] = NULL; }
}
#endif

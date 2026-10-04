/* Standalone integration test; run in a scratch directory (exclusive exports).
 * cc -std=c89 -pedantic -Wall -Wextra -Werror cad_workspace.c cad_model.c
 *    cad_params.c cad_sketch.c cad_export.c cad_drawing.c
 *    tests/workspace_test.c -o /tmp/opencode/workspace_test
 * All large objects intentionally static, never on the Win16 stack.
 * Watcom controller syntax check: -bt=windows -zW -ms -2 -ox -w4 -we.
 * Watcom test/storage and integrated pointer ABI: -ml -zt=1 instead of -ms.
 */
#include "../cad_workspace.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>

static CadWorkspace w, saved;
static CadWorkspaceStage stage;
static CadWorkspaceView view, before_view;
static CadSketchPoint polygon[CAD_SKETCH_MAX_POINTS];
static int model_lines, labels;

static int sink_line(void *context, long x1, long y1, long x2, long y2,
                     unsigned flags)
{
    int *abort_sink = (int *)context;
    assert(x1 >= 0 && y1 >= 0 && x2 >= 0 && y2 >= 0);
    if (flags & CAD_DRAWING_MODEL_LINE) ++model_lines;
    return !*abort_sink;
}
static int sink_text(void *context, long x, long y, const char *text,
                     unsigned flags)
{
    (void)context; (void)flags;
    assert(x >= 0 && y >= 0 && text && *text); ++labels; return 1;
}
static void preserve(void) { memcpy(&saved, &w, sizeof(w)); }
static void unchanged(void) { assert(!memcmp(&saved, &w, sizeof(w))); }

static void export_counts(const char *path, int format, int nv, int ne)
{
    FILE *f;
    char line[256], value[256];
    int vertices = 0, edges = 0, a, b, code;
    long x, y, z;
    assert(cad_workspace_export(&w, &stage, path, format, 0) == CAD_EXPORT_OK);
    f = fopen(path, "r"); assert(f);
    if (format == CAD_EXPORT_OBJ) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "v ", 2)) {
                assert(sscanf(line, "v %ld %ld %ld", &x, &y, &z) == 3);
                assert(vertices < stage.view.vertex_count);
                assert(x == stage.view.vertices[vertices].x);
                assert(y == stage.view.vertices[vertices].y);
                assert(z == stage.view.vertices[vertices].z);
                ++vertices;
            } else if (!strncmp(line, "l ", 2)) {
                assert(sscanf(line, "l %d %d", &a, &b) == 2);
                assert(edges < stage.view.edge_count);
                assert(a == stage.view.edges[edges].a + 1);
                assert(b == stage.view.edges[edges].b + 1);
                ++edges;
            }
        }
        assert(vertices == nv);
    } else {
        while (fgets(line, sizeof(line), f)) {
            assert(sscanf(line, "%d", &code) == 1);
            assert(fgets(value, sizeof(value), f));
            if (code == 0 && !strcmp(value, "LINE\n")) ++edges;
        }
    }
    assert(!ferror(f)); assert(fclose(f) == 0); assert(edges == ne);
    preserve();
    assert(cad_workspace_export(&w, &stage, path, format, 0) == CAD_EXPORT_EXISTS);
    unchanged(); assert(remove(path) == 0);
}
static void roundtrip(void)
{
    FILE *f = tmpfile();
    CadId old = w.model.vertices[0].id;
    unsigned long revision = w.revision;
    assert(f);
    assert(cad_workspace_write(&w, f) == CAD_WORKSPACE_OK);
    assert(cad_workspace_materialize(&w, &view) == CAD_WORKSPACE_OK);
    rewind(f);
    assert(cad_workspace_read(&w, &stage, f) == CAD_WORKSPACE_OK);
    assert(fclose(f) == 0);
    assert(w.revision == revision + 1 && !cad_workspace_modified(&w));
    assert(w.model.vertices[0].id > old);
    assert(cad_workspace_materialize(&w, &stage.view) == CAD_WORKSPACE_OK);
    assert(view.vertex_count == stage.view.vertex_count);
    assert(view.edge_count == stage.view.edge_count);
    assert(!memcmp(view.vertices, stage.view.vertices,
                   (size_t)view.vertex_count * sizeof(view.vertices[0])));
    assert(!memcmp(view.edges, stage.view.edges,
                   (size_t)view.edge_count * sizeof(view.edges[0])));
}
static void draw(void)
{
    CadDrawingSheet sheet;
    CadDrawingSink sink;
    CadDrawingLayout layout;
    int abort_sink = 0;
    sheet.width = 4000; sheet.height = 4000; sheet.margin = 20; sheet.gap = 50;
    sheet.scale.numerator = sheet.scale.denominator = 1;
    sink.context = &abort_sink; sink.line = sink_line; sink.text = sink_text;
    model_lines = labels = 0;
    assert(cad_workspace_draw(&w, &stage, &sheet, &sink, &layout) == CAD_DRAWING_OK);
    assert(model_lines == 3 * w.model.edge_count && labels > 0);
    assert(layout.model_bounds.width > 0);
    abort_sink = 1;
    assert(cad_workspace_draw(&w, &stage, &sheet, &sink, 0) == CAD_DRAWING_SINK_FAILED);
}
static void malformed(const char *text)
{
    FILE *f = tmpfile();
    assert(f); assert(fputs(text, f) != EOF); rewind(f); preserve();
    assert(cad_workspace_read(&w, &stage, f) != CAD_WORKSPACE_OK);
    unchanged(); assert(fclose(f) == 0);
}
static void house(void)
{
    CadModelScale scale;
    CadValue value;
    CadId v, e;
    unsigned long revision;
    scale.unit = CAD_UNIT_MM; scale.ticks_per_model_unit = 100;
    preserve();
    assert(cad_workspace_create_house(&w, &stage, CAD_UNITS_UNSPECIFIED, scale, 0)
           == CAD_WORKSPACE_ARGUMENT); unchanged();
    scale.unit = CAD_UNIT_LEGACY;
    assert(cad_workspace_create_house(&w, &stage, CAD_UNITS_UNSPECIFIED, scale,
           CAD_WORKSPACE_MAP_DEFAULTS_TO_LEGACY) == CAD_WORKSPACE_OK);
    assert(w.model.vertex_count == 30 && w.model.edge_count == 40);
    assert(w.model.units == CAD_UNITS_UNSPECIFIED);
    assert(w.intent.parameters.params[0].value.unit == CAD_UNIT_LEGACY);
    assert(w.model.vertices[0].x == -100 && w.model.vertices[29].y == 120);
    assert(cad_workspace_modified(&w)); cad_workspace_mark_saved(&w);
    assert(!cad_workspace_modified(&w));
    v = w.model.vertices[0].id; e = w.model.edges[0].id;
    assert(cad_workspace_change_house_parameter(&w, &stage, "Width", "Depth * 2")
           == CAD_WORKSPACE_OK);
    assert(w.model.vertices[0].x == -140 && w.model.vertices[1].x == 140);
    assert(w.model.vertices[0].id == v && w.model.edges[0].id == e);
    revision = w.revision;
    assert(cad_workspace_change_house_parameter(&w, &stage, "Width", "Depth * 2")
           == CAD_WORKSPACE_OK); assert(w.revision == revision);
    preserve();
    assert(cad_workspace_change_house_parameter(&w, &stage, "Width", "2mm")
           == CAD_WORKSPACE_PARAMETER); unchanged();
    assert(cad_workspace_change_house_parameter(&w, &stage, "Width", "Width")
           == CAD_WORKSPACE_PARAMETER); unchanged();
    assert(cad_workspace_change_house_parameter(&w, &stage, "Depth", "1 / 0")
           == CAD_WORKSPACE_PARAMETER); unchanged();
    assert(cad_workspace_change_house_parameter(&w, &stage, "Width", "Depth\n*2")
           == CAD_WORKSPACE_PARAMETER); unchanged();
    assert(cad_workspace_change_house_parameter(&w, 0, "Width", "Depth * 2")
           == CAD_WORKSPACE_ARGUMENT); unchanged();
    value.type = CAD_LENGTH; value.unit = CAD_UNIT_LEGACY; value.ticks = 90000;
    assert(cad_workspace_set_house_value(&w, &stage, "Width", value)
           == CAD_WORKSPACE_PARAMETER); unchanged();
    roundtrip();
    assert(!strcmp(w.intent.parameters.params[0].expression, "Depth * 2"));
    assert(w.intent.parameters.params[0].value.unit == CAD_UNIT_LEGACY);
    export_counts("HOUSE.OBJ", CAD_EXPORT_OBJ, 30, 40);
    export_counts("HOUSE.DXF", CAD_EXPORT_DXF, 30, 40); draw();
    value.ticks = 20000;
    assert(cad_workspace_set_house_value(&w, &stage, "Width", value) == CAD_WORKSPACE_OK);
    assert(w.model.vertices[0].x == -100 && !w.intent.parameters.params[0].expression[0]);
    scale.unit = CAD_UNIT_MM;
    assert(cad_workspace_create_house(&w, &stage, CAD_UNITS_MM, scale, 0) == CAD_WORKSPACE_OK);
    assert(w.model.units == CAD_UNITS_MM);
    assert(cad_workspace_change_house_parameter(&w, &stage, "Width", "240mm")
           == CAD_WORKSPACE_OK); assert(w.model.vertices[0].x == -120);
    roundtrip();
    scale.ticks_per_model_unit = 2540;
    assert(cad_workspace_create_house(&w, &stage, CAD_UNITS_INCHES, scale, 0)
           == CAD_WORKSPACE_OK);
    assert(w.model.vertices[0].x == -3 && w.model.units == CAD_UNITS_INCHES);
    scale.ticks_per_model_unit = 100; preserve();
    assert(cad_workspace_create_house(&w, &stage, CAD_UNITS_INCHES, scale, 0)
           == CAD_WORKSPACE_ARGUMENT); unchanged();
}
static void extrusion(void)
{
    CadSketchPoint square[4];
    CadId old = w.model.vertices[0].id, id;
    long volume;
    int j;
    square[0].x = 0; square[0].y = 0;
    square[1].x = 20; square[1].y = 0;
    square[2].x = 20; square[2].y = 10;
    square[3].x = 0; square[3].y = 10;
    assert(cad_workspace_create_extrusion(&w, &stage, square, 4, 30,
                                         CAD_UNITS_UNSPECIFIED) == CAD_WORKSPACE_OK);
    assert(w.model.vertices[0].id > old);
    assert(w.model.vertex_count == 8 && w.model.edge_count == 18);
    assert(w.mesh.triangle_count == 12);
    assert(cad_sketch_mesh_validate(&w.mesh) == CAD_SKETCH_OK);
    assert(cad_sketch_mesh_volume6(&w.mesh, &volume) == CAD_SKETCH_OK);
    assert(volume == 36000); id = w.model.vertices[0].id;
    assert(cad_workspace_change_height(&w, &stage, 40) == CAD_WORKSPACE_OK);
    assert(w.model.vertices[0].id == id && w.model.vertices[4].z == 40);
    assert(cad_sketch_mesh_volume6(&w.mesh, &volume) == CAD_SKETCH_OK && volume == 48000);
    preserve();
    assert(cad_workspace_change_height(&w, &stage, 401) == CAD_WORKSPACE_SKETCH); unchanged();
    square[2] = square[0];
    assert(cad_workspace_create_extrusion(&w, &stage, square, 4, 10,
                                         CAD_UNITS_MM) == CAD_WORKSPACE_SKETCH); unchanged();
    roundtrip(); assert(w.intent.height == 40 && w.intent.profile[1].x == 20);
    export_counts("EXTR.OBJ", CAD_EXPORT_OBJ, 8, 18);
    export_counts("EXTR.DXF", CAD_EXPORT_DXF, 8, 18); draw();
    /* 64-point rectangle perimeter with retained collinear points: maximum
     * profile, 128 vertices, 252 triangles, 378 edges, many core batches. */
    for (j = 0; j < 16; ++j) {
        polygon[j].x = j * 10; polygon[j].y = 0;
        polygon[16+j].x = 160; polygon[16+j].y = j * 10;
        polygon[32+j].x = 160-j*10; polygon[32+j].y = 160;
        polygon[48+j].x = 0; polygon[48+j].y = 160-j*10;
    }
    assert(cad_workspace_create_extrusion(&w, &stage, polygon, 64, 100,
                                         CAD_UNITS_MM) == CAD_WORKSPACE_OK);
    assert(w.model.vertex_count == 128 && w.model.edge_count == 378);
    assert(cad_sketch_mesh_volume6(&w.mesh, &volume) == CAD_SKETCH_OK);
    assert(volume == 15360000);
    id = w.model.vertices[127].id;
    assert(cad_workspace_change_height(&w, &stage, 200) == CAD_WORKSPACE_OK);
    assert(w.model.vertices[127].id == id);
    export_counts("MAX.OBJ", CAD_EXPORT_OBJ, 128, 378); draw();
}
static void id_mapping(void)
{
    CadVertex vertex;
    CadId id;
    /* Deliberately use standalone model API on a test-only EMPTY feature to
     * exercise materialization of sparse IDs and dense deletion reorder. */
    assert(cad_workspace_init(&w) == CAD_WORKSPACE_OK);
    assert(cad_model_begin(&w.model) == CAD_OK);
    assert(cad_model_add_vertex(&w.model, 1, 2, 3, &id) == CAD_OK);
    assert(cad_model_delete_vertex(&w.model, id) == CAD_OK);
    assert(cad_model_add_vertex(&w.model, 10, 20, 30, &id) == CAD_OK);
    assert(cad_model_add_vertex(&w.model, 40, 50, 60, &id) == CAD_OK);
    assert(cad_model_add_edge(&w.model, 2, 3, &id) == CAD_OK);
    assert(cad_model_commit(&w.model) == CAD_OK);
    assert(cad_workspace_materialize(&w, &view) == CAD_WORKSPACE_OK);
    assert(view.edges[0].a == 0 && view.edges[0].b == 1 && view.ids[0] == 2);
    vertex = w.model.vertices[0]; w.model.vertices[0] = w.model.vertices[1];
    w.model.vertices[1] = vertex;
    assert(cad_workspace_materialize(&w, &view) == CAD_WORKSPACE_OK);
    assert(view.edges[0].a == 1 && view.edges[0].b == 0);
    memcpy(&before_view, &view, sizeof(view));
    w.model.edges[0].a = 65535U;
    assert(cad_workspace_materialize(&w, &view) == CAD_WORKSPACE_TOPOLOGY);
    assert(!memcmp(&before_view, &view, sizeof(view)));
}
static void full_view(void)
{
    int j;
    assert(cad_workspace_init(&w) == CAD_WORKSPACE_OK);
    /* Test-only read fixture at the renderer's full 256/512 bounds. */
    w.model.vertex_count = CAD_MAX_VERTICES;
    w.model.edge_count = CAD_MAX_EDGES;
    for (j = 0; j < CAD_MAX_VERTICES; ++j) {
        w.model.vertices[j].id = (CadId)(2*j+1);
        w.model.vertices[j].x = j-128;
        w.model.vertices[j].y = j % 7;
        w.model.vertices[j].z = j % 11;
    }
    for (j = 0; j < CAD_MAX_EDGES; ++j) {
        w.model.edges[j].id = (CadId)(j+1);
        w.model.edges[j].a = w.model.vertices[j % 256].id;
        w.model.edges[j].b = w.model.vertices[(j+1) % 256].id;
    }
    assert(cad_workspace_materialize(&w, &view) == CAD_WORKSPACE_OK);
    assert(view.vertex_count == 256 && view.edge_count == 512);
    assert(view.edges[511].a == 255 && view.edges[511].b == 0);
    export_counts("FULL.OBJ", CAD_EXPORT_OBJ, 256, 512); draw();
    memcpy(&before_view, &view, sizeof(view));
    w.model.vertices[255].id = w.model.vertices[0].id;
    assert(cad_workspace_materialize(&w, &view) == CAD_WORKSPACE_TOPOLOGY);
    assert(!memcmp(&before_view, &view, sizeof(view)));
    w.model.vertices[255].id = 511;
    w.model.vertices[0].x = 401;
    assert(cad_workspace_materialize(&w, &view) == CAD_WORKSPACE_TOPOLOGY);
    assert(!memcmp(&before_view, &view, sizeof(view)));
}
int main(void)
{
#ifndef __WATCOMC__
    assert(sizeof(w) < 65536UL && sizeof(stage) < 65536UL);
#endif
    assert(cad_workspace_init(&w) == CAD_WORKSPACE_OK);
    house(); extrusion();
    malformed("CWS 2\n");
    malformed("CWS 1\n2\n0\n3\n999999999999999999999\n");
    malformed("CWS 1\n2\n0\n3\n10\n0\n0\n10\n0\n0\n10\nEND\njunk\n");
    preserve();
    assert(cad_workspace_undo(&w) == CAD_WORKSPACE_UNSUPPORTED);
    assert(cad_workspace_constraints(&w) == CAD_WORKSPACE_UNSUPPORTED);
    assert(cad_workspace_assembly(&w) == CAD_WORKSPACE_UNSUPPORTED);
    assert(cad_workspace_macro(&w) == CAD_WORKSPACE_UNSUPPORTED); unchanged();
    w.model.next_vertex_id = 65536L; preserve();
    assert(cad_workspace_create_extrusion(&w, &stage, polygon, 64, 10,
                                         CAD_UNITS_MM) == CAD_WORKSPACE_MODEL); unchanged();
    w.revision = 2147483647UL; preserve();
    assert(cad_workspace_change_height(&w, &stage, 12) == CAD_WORKSPACE_REVISION); unchanged();
    id_mapping(); full_view();
    printf("workspace integration OK; host workspace=%lu stage=%lu combined=%lu bytes\n",
           (unsigned long)sizeof(w), (unsigned long)sizeof(stage),
           (unsigned long)sizeof(w) + (unsigned long)sizeof(stage));
    return 0;
}

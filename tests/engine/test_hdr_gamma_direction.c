/* HDR Toning gamma below 1 darkens midtones, above 1 lightens them (#710): gamma is applied as
 * 1/gamma like the crate's other gamma controls (port of
 * crates/engine/tests/hdr_gamma_direction.rs). */
#include "engine/engine.h"
#include "harness.h"

/* document.pixel at (x,y) into out[4]; returns false on failure. */
static bool pixel(pc_engine_session *s, int x, int y, double out[4])
{
    char p[48];
    (void)snprintf(p, sizeof p, "{\"x\":%d,\"y\":%d}", x, y);
    pc_json_value v = PC_JSON_VALUE_INIT;
    if (pc_engine_session_execute_json(s, "document.pixel", p, &v, NULL) != PC_OK) {
        return false;
    }
    yyjson_mut_val *arr = pc_json_value_root(&v);
    bool ok = arr != NULL && yyjson_mut_is_arr(arr) && yyjson_mut_arr_size(arr) >= 3;
    if (ok) {
        for (size_t i = 0; i < 4 && i < yyjson_mut_arr_size(arr); i++) {
            out[i] = yyjson_mut_get_real(yyjson_mut_arr_get(arr, i));
        }
    }
    pc_json_value_fini(&v);
    return ok;
}

static double midtone_luma(double gamma)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new",
                                         "{\"width\":8,\"height\":8,\"background\":\"#808080\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    char p[160];
    (void)snprintf(p, sizeof p,
                   "{\"gamma\":%g,\"strength\":0.1,\"detail\":0,\"saturation\":0,\"radius\":30}", gamma);
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "image.adjustments.hdrToning", p, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    double px[4] = {0, 0, 0, 0};
    CHECK(pixel(s, 4, 4, px));
    pc_engine_session_free(s);
    return 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
}

static void test_hdr_toning_gamma_direction(void)
{
    double base = midtone_luma(1.0);
    double low = midtone_luma(0.5);
    double high = midtone_luma(2.0);
    if (!(low < base)) {
        PC_T_FAIL("gamma 0.5 must darken the midtone: base=%g low=%g", base, low);
    }
    if (!(high > base)) {
        PC_T_FAIL("gamma 2.0 must lighten the midtone: base=%g high=%g", base, high);
    }
}

int main(int argc, char **argv)
{
    RUN(test_hdr_toning_gamma_direction);
    return pc_t_finish();
}

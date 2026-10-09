/* Gamut checks against a proof profile: View > Gamut Warning (port of crates/cms/src/gamut.rs).
 *
 * A colour is out of gamut when sending it to the proof device (relative colorimetric,
 * clipping) and back to Lab moves it by more than the threshold (dE76). The dE is tabulated
 * over the source device space (256 for 1 channel, 33^3 for 3, 17^4 for 4, exact otherwise).
 * Immutable once built; usable from any thread. Release with pc_gamut_check_free. */
#ifndef PC_CMS_GAMUT_H
#define PC_CMS_GAMUT_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "profile.h"

#define PC_GAMUT_DEFAULT_THRESHOLD 4.0f

typedef struct pc_gamut_check pc_gamut_check;

pc_status pc_gamut_check_new(const pc_profile *src, const pc_profile *proof, float threshold, pc_gamut_check **out,
                             pc_error *err);
void pc_gamut_check_free(pc_gamut_check *g);
size_t pc_gamut_check_inputs(const pc_gamut_check *g);
float pc_gamut_check_threshold(const pc_gamut_check *g);
/* dE76 between the colour and its proof round trip. */
float pc_gamut_check_delta_e(const pc_gamut_check *g, const float *px);
bool pc_gamut_check_out_of_gamut(const pc_gamut_check *g, const float *px);

#endif

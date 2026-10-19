/* Colour-management state and primitives (port of crates/engine/src/color_cmds.rs, E8).
 *
 * This header defines the session-wide colour state (pc_color_state, Rust ColorState) embedded in
 * the Session, the colour-settings model (Rust ColorSettings) and the conversion primitives that
 * other engine chunks depend on: pc_engine_color_convert_mode (E3 Image > Mode), and the
 * document/working/composite profile resolvers (E3/E4). Commands (Edit > Assign/Convert/Color
 * Settings, View > Proof Setup/Proof Colors/Gamut Warning) live in color_cmds.c.
 *
 * Conventions: src/doc/adjust.h. Owned strings are plain char* (free()); owned profiles are
 * pc_profile by value (pc_profile_fini). Nothing here is synchronised: one owner at a time.
 *
 * Canvas transforms, proof simulations and HDR preview are in display_color.h. Monitor status
 * currently reports the explicit Color Settings profile or sRGB fallback; platform profile
 * discovery belongs to the desktop services layer. */
#ifndef PC_ENGINE_COLOR_CMDS_H
#define PC_ENGINE_COLOR_CMDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cms/cms.h"
#include "cms/profile.h"
#include "color/color.h"
#include "doc/doc.h"

/* Edit > Color Settings policy for documents whose embedded profile differs from the working
 * space (Rust color_cmds::Policy). */
typedef enum pc_color_policy {
    PC_COLOR_POLICY_PRESERVE = 0,
    PC_COLOR_POLICY_CONVERT,
    PC_COLOR_POLICY_OFF
} pc_color_policy;

const char *pc_color_policy_id(pc_color_policy p);        /* "preserve" | "convert" | "off" */
bool pc_color_policy_parse(const char *s, pc_color_policy *out); /* accepts the dialog aliases */

/* Edit > Color Settings: working spaces, policies and conversion options (Rust ColorSettings).
 * All string specs are owned (free()); none is ever NULL after pc_color_settings_default. */
typedef struct pc_color_settings {
    char *working_rgb;
    char *working_cmyk;
    char *working_gray;
    pc_color_policy policy_rgb;
    pc_color_policy policy_cmyk;
    pc_color_policy policy_gray;
    bool ask_on_mismatch;
    bool ask_on_paste;
    bool ask_on_missing;
    char *intent; /* rendering-intent id ("relative" ...) */
    bool bpc;
    bool dither;
    float blend_text_gamma;
    char *monitor_profile; /* "auto" | built-in id | .icc path */
} pc_color_settings;

pc_status pc_color_settings_default(pc_color_settings *out, pc_error *err);
void pc_color_settings_fini(pc_color_settings *c);
pc_status pc_color_settings_clone(const pc_color_settings *c, pc_color_settings *out, pc_error *err);
bool pc_color_settings_eq(const pc_color_settings *a, const pc_color_settings *b);
pc_intent pc_color_settings_intent(const pc_color_settings *c);
/* Validate that each working space resolves to the right colour space and RGB can be a source and
 * destination for the intent. PC_OK when valid; otherwise fills err with the reference wording. */
pc_status pc_color_settings_validate(const pc_color_settings *c, pc_error *err);

/* View > Proof Setup (Rust ProofSetup); kind 0 is profile, 1..5 plates, 6 legacy Mac,
 * 7 protanopia and 8 deuteranopia (display_color.h). */
typedef struct pc_proof_setup {
    char *name;          /* owned: what the user asked for (built-in id or path) */
    pc_profile profile;  /* owned */
    pc_intent intent;
    bool bpc;
    bool simulate_paper;
    int kind;            /* 0 = plain profile proof */
} pc_proof_setup;

/* View > Proof Setup / Proof Colors / Gamut Warning per document (Rust ProofView). */
typedef struct pc_proof_view {
    pc_proof_setup setup;
    bool enabled;
    bool gamut_warning;
    float gamut_threshold;
} pc_proof_view;

typedef struct pc_color_proof_entry {
    pc_doc_id doc;
    pc_proof_view view;
} pc_color_proof_entry;

/* Session-wide colour settings, per-document proofing and HDR previews (Rust ColorState). */
typedef struct pc_hdr_preview { bool highlight_compression;float exposure,gamma; } pc_hdr_preview;
typedef struct pc_hdr_entry { pc_doc_id doc;pc_hdr_preview view; } pc_hdr_entry;

typedef struct pc_color_state {
    pc_color_settings settings;
    pc_color_proof_entry *proofs;
    size_t nproofs;
    size_t cap_proofs;
    pc_hdr_entry *hdr;size_t nhdr,cap_hdr;
} pc_color_state;

void pc_engine_color_init(pc_color_state *cs);  /* defaults; never fails fatally */
void pc_engine_color_fini(pc_color_state *cs);  /* NULL-safe */

#endif /* PC_ENGINE_COLOR_CMDS_H */

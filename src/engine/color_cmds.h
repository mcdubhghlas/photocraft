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
 * SCOPE (E8, 2026-10-14): the colour-managed canvas display path (display_color.rs, proof_sim.rs:
 * monitor-profile caching, canvas LUTs, 32-bit preview, plate/CVD proof simulation) is deferred;
 * it is a UI-canvas concern not linked by any engine command. The commands here report a headless
 * monitor status (no platform display reader -> sRGB), which matches the reference in a headless /
 * test session. See the chunk's REWRITE_PLAN.md §8 entry. */
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

/* View > Proof Setup (Rust ProofSetup). The simulation `kind` beyond the plain profile proof
 * (plates / CVD / RGB display) is deferred to proof_sim.rs; this chunk only sets kind = 0. */
typedef struct pc_proof_setup {
    char *name;          /* owned: what the user asked for (built-in id or path) */
    pc_profile profile;  /* owned */
    pc_intent intent;
    bool bpc;
    bool simulate_paper;
    int kind;            /* 0 = plain profile proof (proof_sim kinds deferred) */
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

/* Session-wide colour settings and per-document proofing (Rust ColorState). The display caches,
 * monitor detection and 32-bit preview state are deferred (see the header note). */
typedef struct pc_color_state {
    pc_color_settings settings;
    pc_color_proof_entry *proofs;
    size_t nproofs;
    size_t cap_proofs;
} pc_color_state;

void pc_engine_color_init(pc_color_state *cs);  /* defaults; never fails fatally */
void pc_engine_color_fini(pc_color_state *cs);  /* NULL-safe */

#endif /* PC_ENGINE_COLOR_CMDS_H */

/* image.detectFaces (#3603; port of crates/engine/src/face_cmds.rs): report the faces in the
 * document, with their box and five landmarks.
 *
 * The reference reads faces from photocraft_algo::faces (libfacedetection). The C tree has no face
 * detector yet (no src/algo faces backend), so — exactly as the reference does in its browser
 * build where `faces::AVAILABLE` is false — the command is registered but reports a clear
 * "unsupported" error instead of crashing (REWRITE_PLAN.md rule 9, "unfinished features return an
 * unsupported error"). Wiring a face detector into src/algo and lighting this up is a follow-up
 * (REWRITE_PLAN.md §8 R0 feature track). */
#include <stddef.h>

#include "engine_internal.h"
#include "face_cmds.h"

#define CMD "image.detectFaces"

#define UNAVAILABLE "face detection isn't available in this build of PhotoCraft (no face detector)"

static pc_status face_enabled(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    return pc_fail(reason, PC_ERR_UNSUPPORTED, UNAVAILABLE);
}

static pc_status run_detect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED, UNAVAILABLE);
}

#define FACE_PARAMS                                                                                \
    "{\"minScore\":0..1=0.5,\"sampleAllLayers\":bool=true} -> {\"faces\":[{score,rect:[x,y,w,h],"  \
    "landmarks:{rightEye,leftEye,nose,mouthRight,mouthLeft}}]} (best first; unsupported in this "  \
    "build: no face detector)"

static const pc_engine_command SPECS[] = {
    {CMD, "Detect Faces", NULL, 0, NULL, FACE_PARAMS, face_enabled, run_detect, false},
};

const pc_engine_command *pc_engine_face_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}

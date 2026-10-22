/* Channel Mixer readout core (headless; no GUI). Port of the Total% display in
 * crates/ui-egui/src/adjust_editors.rs channel_mixer. */
#ifndef PC_UI_CHANNEL_MIXER_H
#define PC_UI_CHANNEL_MIXER_H

#include <stdbool.h>

/* Sum of the source-channel percentages that form one output channel (the "Total:" readout). */
double pc_ui_channel_mixer_total(double red, double green, double blue);

/* Whether `total` exceeds 100% (Photoshop warns: the output channel will clip). */
bool pc_ui_channel_mixer_overload(double total);

#endif

#ifndef CLI_H
#define CLI_H

#include "bluebus.h"

cli_t *cli_create(audio_output_t *audio, avrcp_controller_t *avrcp, a2dp_sink_t *a2dp, hfp_client_t *hfp);
void cli_destroy(cli_t *cli);
void cli_start(cli_t *cli);

#endif

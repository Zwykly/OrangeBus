#ifndef CLI_H
#define CLI_H

#include "orangebus.h"

cli_t *cli_create(audio_output_t *audio, avrcp_controller_t *avrcp, a2dp_sink_t *a2dp, hfp_client_t *hfp, eq_processor_t *eq, ibus_t *ibus, cdc_t *cdc, tel_t *tel, ibus_config_t *config);
void cli_destroy(cli_t *cli);
void cli_start(cli_t *cli);

#endif

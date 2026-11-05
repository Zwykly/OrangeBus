#include "ibus_private.h"
#include "ibus_protocol.h"

const char *ibus_describe_src(uint8_t src)
{
    switch (src) {
    case ORANGEBUS_IBUS_DEV_CDC: return "CDC";
    case ORANGEBUS_IBUS_DEV_TEL: return "TEL";
    case ORANGEBUS_IBUS_DEV_RAD: return "RAD";
    case ORANGEBUS_IBUS_DEV_GT:  return "GT";
    case ORANGEBUS_IBUS_DEV_MID: return "MID";
    case ORANGEBUS_IBUS_DEV_DSP: return "DSP";
    default: return "???";
    }
}

const char *ibus_describe_dst(uint8_t dst)
{
    switch (dst) {
    case ORANGEBUS_IBUS_DEV_RAD:  return "RAD";
    case ORANGEBUS_IBUS_DEV_MID:  return "MID";
    case ORANGEBUS_IBUS_DEV_BMBT: return "BMBT";
    case ORANGEBUS_IBUS_DEV_GT:   return "GT";
    case ORANGEBUS_IBUS_DEV_DSP:  return "DSP";
    case ORANGEBUS_IBUS_DEV_IKE:  return "IKE";
    case ORANGEBUS_IBUS_DEV_LCM:  return "LCM";
    case ORANGEBUS_IBUS_DEV_GM:   return "GM";
    default: return "???";
    }
}

const char *ibus_describe_cmd(uint8_t src, uint8_t dst, uint8_t cmd)
{
    if (src == ORANGEBUS_IBUS_DEV_CDC && dst == ORANGEBUS_IBUS_DEV_RAD && cmd == ORANGEBUS_IBUS_CMD_CDC_RESPONSE)
        return "CDC_STATUS";
    if (src == ORANGEBUS_IBUS_DEV_TEL && cmd == ORANGEBUS_IBUS_TEL_CMD_STATUS)
        return "TEL_STATUS";
    if (src == ORANGEBUS_IBUS_DEV_TEL && cmd == ORANGEBUS_IBUS_TEL_CMD_LED_STATUS)
        return "TEL_LED";
    if (src == ORANGEBUS_IBUS_DEV_TEL && cmd == ORANGEBUS_IBUS_TEL_CMD_TITLE_TEXT)
        return "TEL_TITLE";
    if (src == ORANGEBUS_IBUS_DEV_MID && cmd == ORANGEBUS_IBUS_MID_CMD_SET_MODE)
        return "MID_SET_MODE";
    if (src == ORANGEBUS_IBUS_DEV_MID && dst == ORANGEBUS_IBUS_DEV_RAD)
        return "MID_TEXT";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_TITLE)
        return "GT_TITLE";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_ZONE)
        return "GT_ZONE";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_INDEX)
        return "GT_INDEX";
    if (src == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_CLEAR)
        return "GT_CLEAR";
    if (src == ORANGEBUS_IBUS_DEV_RAD && dst == ORANGEBUS_IBUS_DEV_GT && cmd == ORANGEBUS_IBUS_CMD_GT_WRITE_TITLE)
        return "RAD->GT_TITLE(BUS_NAV)";
    if (src == ORANGEBUS_IBUS_DEV_RAD && dst == ORANGEBUS_IBUS_DEV_DSP && cmd == ORANGEBUS_IBUS_DSP_CMD_CONFIG_SET)
        return "DSP_CONFIG";
    return "OTHER";
}

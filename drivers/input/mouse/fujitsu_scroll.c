// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujitsu Scroll Devices PS/2 mouse driver
 *
 *   2021 Sam Mertens <smertens.public@gmail.com>
 *     Used the original synaptics.c source as a framework to support
 *     the Fujitsu scroll devices in the Fujitsu Lifebook T901 laptop
 *
 *
 * Trademarks are the property of their respective owners.
 */

#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/serio.h>
#include <linux/libps2.h>
#include <linux/rmi.h>
#include <linux/slab.h>
#include "psmouse.h"
#include "fujitsu_scroll.h"

#ifdef CONFIG_MOUSE_PS2_FUJITSU_SCROLL

#if defined(CONFIG_DMI) && defined(CONFIG_X86)
static const struct dmi_system_id present_dmi_table[] = {

	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LIFEBOOK T901"),
		},
	},
	{
	        .matches = {
	                DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
	                DMI_MATCH(DMI_PRODUCT_NAME, "LifeBook T901"),
	        },
	},
	
	{ }
};
#endif


int fujitsu_scroll_detect(struct psmouse *psmouse, bool set_properties)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	u8 param[4] = { 0 };

#if defined(CONFIG_DMI) && defined(CONFIG_X86)	
	if (!dmi_check_system(present_dmi_table)) {
	  return -ENODEV;
	}
#endif

	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO);

	/*
	 * Scroll wheel returns  04 43 07
	 * Scroll sensor returns 00 43 07
	 */
	if (param[1] != FUJITSU_SCROLL_ID) {
	     return -ENODEV;
	}

	if (set_properties) {
	    psmouse->vendor = "Fujitsu";
	    switch (param[0]) {
	    case FUJITSU_SCROLL_WHEEL_ID:
	      psmouse->name = "Scroll Wheel";
	      __set_bit(FJS_WHEEL_AXIS, psmouse->dev->relbit);
	      break;
	    case FUJITSU_SCROLL_SENSOR_ID:
	      psmouse->name = "Scroll Sensor";
	      __set_bit(FJS_SENSOR_AXIS, psmouse->dev->relbit);
	      break;
	    default:
	      psmouse->name = "Unknown";
	    }
	}

	return 0;
}

void fujitsu_scroll_init_sequence(struct psmouse *psmouse)
{
  struct ps2dev *ps2dev = &psmouse->ps2dev;
  u8 param[4] = { 0 };
  int error;

  error = ps2_sliced_command(ps2dev, FJS_INIT_MODE);
  param[0] = 0x14;
  ps2_command(ps2dev, param, PSMOUSE_CMD_SETRATE);  
}

int fujitsu_scroll_query_hardware(struct psmouse *psmouse)
{
    struct ps2dev *ps2dev = &psmouse->ps2dev;
    struct fujitsu_scroll_data *priv = psmouse->private;
    u8 param[4];

    ps2_sliced_command(ps2dev, 0);
    ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO);
	
    if (param[0] == FUJITSU_SCROLL_WHEEL_ID) {
       priv->type = FUJITSU_SCROLL_WHEEL;
       priv->axis = FJS_WHEEL_AXIS;
    } else {
       priv->type = FUJITSU_SCROLL_SENSOR;
       priv->axis = FJS_SENSOR_AXIS;
    }
	
    return 0;
}

/*****************************************************************************
 *	Functions to interpret the packets
 ****************************************************************************/
  
/*
 *  called for each full received packet from the device
 */
static void fujitsu_scroll_process_packet(struct psmouse *psmouse)
{
	struct input_dev *dev = psmouse->dev;
	struct fujitsu_scroll_data *priv = psmouse->private;

	unsigned int weight;
	unsigned int position;
	
	int movement;

	position = ((psmouse->packet[1] & 0x0f) << 8) +
	  psmouse->packet[2];
	weight = psmouse->packet[0] & 0x3f;

	if (weight >= FJS_WEIGHT_THRESHOLD) {
	  if (!priv->finger_down) {
	    priv->finger_down = 1;
	    priv->last_event_position = position;
	  } else {
	    if (priv->type == FUJITSU_SCROLL_WHEEL) {
	        if (position > priv->last_event_position) {
		  movement = position - priv->last_event_position;
		  if (movement > MAX_POSITION_CHANGE) {
		    movement = -(FUJITSU_SCROLL_RANGE - movement);
		  }
		} else {
		  movement = -(priv->last_event_position - position);
		  if (movement < -MAX_POSITION_CHANGE) {
		    movement += FUJITSU_SCROLL_RANGE;
		  }
		}
	    } else {  // scroll sensor
	      movement = position - priv->last_event_position;
	    }

	    if (movement > FJS_POSITION_CHANGE_THRESHOLD ||
		movement < -FJS_POSITION_CHANGE_THRESHOLD) {
	      input_report_rel(dev, priv->axis, -(movement >> FJS_MOVEMENT_BITSHIFT));
	      priv->last_event_position = position;
	    }
	  }
	} else if (1 == priv->finger_down) {
	  priv->finger_down = 0;
	}
	
	input_sync(dev);
}


static psmouse_ret_t fujitsu_scroll_process_byte(struct psmouse *psmouse)
{
  if (psmouse->pktcnt >= 6) { /* Full packet received */
    fujitsu_scroll_process_packet(psmouse);
    return PSMOUSE_FULL_PACKET;
  }
	
  return PSMOUSE_GOOD_DATA;
}

/*****************************************************************************
 *	Driver initialization/cleanup functions
 ****************************************************************************/

static void fujitsu_scroll_disconnect(struct psmouse *psmouse)
{
	struct fujitsu_scroll_data *priv = psmouse->private;

	psmouse_reset(psmouse);
	kfree(priv);
	psmouse->private = NULL;
}

static int fujitsu_scroll_reconnect(struct psmouse *psmouse)
{
	psmouse_reset(psmouse);
	fujitsu_scroll_init_sequence(psmouse);
	
	return 0;
}


void __init fujitsu_scroll_module_init(void)
{

}

int fujitsu_scroll_init(struct psmouse *psmouse)
{
	struct fujitsu_scroll_data *priv;

	psmouse_reset(psmouse);
	
	psmouse->private = priv = kzalloc(sizeof(struct fujitsu_scroll_data),
					  GFP_KERNEL);
	if (!priv) {
		return -ENOMEM;
	}
	
	psmouse->protocol_handler = fujitsu_scroll_process_byte;
	psmouse->pktsize = 6;

	psmouse->disconnect = fujitsu_scroll_disconnect;
	psmouse->reconnect = fujitsu_scroll_reconnect;
	/* TODO: see if resync_time needs to be adjusted */
	psmouse->resync_time = 0;

	fujitsu_scroll_query_hardware(psmouse);
	input_set_capability(psmouse->dev, EV_REL, priv->axis);
	fujitsu_scroll_init_sequence(psmouse);

	return 0;
}

#endif /* CONFIG_MOUSE_PS2_FUJITSU_SCROLL */

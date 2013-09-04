#include <platform.h>
#include <print.h>
#include <xccompat.h>
#include <stdio.h>
#include <string.h>
#include <xscope.h>
#include "audio_i2s.h"
#include "avb_xscope.h"
#include "i2c.h"
#include "avb.h"
#include "audio_clock_CS2300CP.h"
#include "audio_clock_CS2100CP.h"
#include "audio_codec_CS4270.h"
#include "simple_printf.h"
#include "media_fifo.h"
#include "ethernet_board_support.h"
#include "simple_demo_controller.h"
#include "avb_1722_1_adp.h"
#include "app_config.h"
#include "avb_ethernet.h"
#include "avb_1722.h"
#include "gptp.h"
#include "media_clock_server.h"
#include "avb_1722_1.h"
#include "avb_srp.h"
#include "ethernet_phy_reset.h"

// This is the number of master clocks in a word clock
#define MASTER_TO_WORDCLOCK_RATIO 512

// Timeout for debouncing buttons
#define BUTTON_TIMEOUT_PERIOD (20000000)

// Buttons on reference board
enum gpio_cmd
{
  STREAM_SEL=1, REMOTE_SEL=2, CHAN_SEL=4
};

// Note that this port must be at least declared to ensure it
// drives the mute low
out port p_mute_led_remote = PORT_MUTE_LED_REMOTE; // mute, led remote;
out port p_chan_leds = PORT_LEDS;
in port p_buttons = PORT_BUTTONS;

on ETHERNET_DEFAULT_TILE: otp_ports_t otp_ports = OTP_PORTS_INITIALIZER;

smi_interface_t smi1 = ETHERNET_DEFAULT_SMI_INIT;

// Circle slot
mii_interface_t mii1 = ETHERNET_DEFAULT_MII_INIT;

// Square slot
on tile[1]: mii_interface_t mii2 = {
  XS1_CLKBLK_3,
  XS1_CLKBLK_4,
  XS1_PORT_1B,
  XS1_PORT_4D,
  XS1_PORT_4A,
  XS1_PORT_1C,
  XS1_PORT_1G,
  XS1_PORT_1F,
  XS1_PORT_4B      
};

ethernet_reset_interface_t p_phy_reset = PORT_ETH_RST_N;

//***** AVB audio ports ****
#if I2C_COMBINE_SCL_SDA
on tile[AVB_I2C_TILE]: port r_i2c = PORT_I2C;
#else
on tile[AVB_I2C_TILE]: struct r_i2c r_i2c = { PORT_I2C_SCL, PORT_I2C_SDA };
#endif

on tile[0]: out buffered port:32 p_fs[1] = { PORT_SYNC_OUT };
on tile[0]: i2s_ports_t i2s_ports =
{
  XS1_CLKBLK_3,
  XS1_CLKBLK_4,
  PORT_MCLK,
  PORT_SCLK,
  PORT_LRCLK
};

#if AVB_DEMO_ENABLE_LISTENER
on tile[0]: out buffered port:32 p_aud_dout[AVB_DEMO_NUM_CHANNELS/2] = PORT_SDATA_OUT;
#else
  #define p_aud_dout null
#endif

#if AVB_DEMO_ENABLE_TALKER
on tile[0]: in buffered port:32 p_aud_din[AVB_DEMO_NUM_CHANNELS/2] = PORT_SDATA_IN;
#else
  #define p_aud_din null
#endif

#if AVB_XA_SK_AUDIO_SLICE
on tile[0]: out port p_audio_shared = PORT_AUDIO_SHARED;
#endif

// PTP sync port
on tile[0]: port ptp_sync_port = XS1_PORT_1C;

#if AVB_DEMO_ENABLE_LISTENER
media_output_fifo_data_t ofifo_data[AVB_NUM_MEDIA_OUTPUTS];
media_output_fifo_t ofifos[AVB_NUM_MEDIA_OUTPUTS];
#else
  #define ofifos null
#endif

#if AVB_DEMO_ENABLE_TALKER
media_input_fifo_data_t ififo_data[AVB_NUM_MEDIA_INPUTS];
media_input_fifo_t ififos[AVB_NUM_MEDIA_INPUTS];
#else
  #define ififos null
#endif

[[combinable]] void demo_task(client interface avb_interface avb, chanend c_gpio_ctl);
void gpio_task(chanend c_gpio_ctl);

void xscope_user_init(void)
{
  // xscope_register_no_probes();
  xscope_register(1, XSCOPE_CONTINUOUS, "null", XSCOPE_UINT, "value");
  // Enable XScope printing
  xscope_config_io(XSCOPE_IO_BASIC);
}

void audio_hardware_setup(void)
{
#if PLL_TYPE_CS2100
  audio_clock_CS2100CP_init(r_i2c, MASTER_TO_WORDCLOCK_RATIO);
#elif PLL_TYPE_CS2300
  audio_clock_CS2300CP_init(r_i2c, MASTER_TO_WORDCLOCK_RATIO);
#endif
#if AVB_XA_SK_AUDIO_SLICE
  audio_codec_CS4270_init(p_audio_shared, 0xff, 0x90, r_i2c);
  audio_codec_CS4270_init(p_audio_shared, 0xff, 0x92, r_i2c);
#endif
}

enum mac_rx_chans {
  MAC_RX_TO_MEDIA_CLOCK = 0,
#if AVB_DEMO_ENABLE_LISTENER
  MAC_RX_TO_LISTENER,
#endif
  MAC_RX_TO_SRP,
  MAC_RX_TO_1722_1,
  NUM_MAC_RX_CHANS
};

enum mac_tx_chans {
  MAC_TX_TO_MEDIA_CLOCK = 0,
#if AVB_DEMO_ENABLE_TALKER
  MAC_TX_TO_TALKER,
#endif
  MAC_TX_TO_SRP,
  MAC_TX_TO_1722_1,
  MAC_TX_TO_AVB_MANAGER,
  NUM_MAC_TX_CHANS
};

enum avb_manager_chans {
  AVB_MANAGER_TO_SRP = 0,
  AVB_MANAGER_TO_1722_1,
  AVB_MANAGER_TO_DEMO,
  NUM_AVB_MANAGER_CHANS
};

enum ptp_chans {
  PTP_TO_AVB_MANAGER = 0,
#if AVB_DEMO_ENABLE_TALKER
  PTP_TO_TALKER,
#endif
  PTP_TO_1722_1,
  PTP_TO_TEST_CLOCK,
  NUM_PTP_CHANS
};

int main(void)
{
  // Ethernet channels
  chan c_mac_tx[NUM_MAC_TX_CHANS];
  chan c_mac_rx[NUM_MAC_RX_CHANS];

  // PTP channels
  chan c_ptp[NUM_PTP_CHANS];

  // AVB unit control
#if AVB_DEMO_ENABLE_TALKER
  chan c_talker_ctl[AVB_NUM_TALKER_UNITS];
#else
  #define c_talker_ctl null
#endif

#if AVB_DEMO_ENABLE_LISTENER
  chan c_listener_ctl[AVB_NUM_LISTENER_UNITS];
  chan c_buf_ctl[AVB_NUM_LISTENER_UNITS];
#else
  #define c_listener_ctl null
  #define c_buf_ctl null
#endif

  // Media control
  chan c_media_ctl[AVB_NUM_MEDIA_UNITS];
  chan c_media_clock_ctl;

  chan c_gpio_ctl;

  interface avb_interface i_avb[NUM_AVB_MANAGER_CHANS];
  interface srp_interface i_srp;

  par
  {
    on ETHERNET_DEFAULT_TILE:
    {
      char mac_address[6];
      otp_board_info_get_mac(otp_ports, 0, mac_address);
      eth_phy_reset(p_phy_reset);
      smi_init(smi1);
      eth_phy_config(1, smi1);
      smi_reg(smi1, 0x4, 0x0020, 0); // Don't advertise as 10 mb capable
      smi_reg(smi1, 0x0, 0x3000, 0); // Restart LDS
      smi_reg(smi1, 0x17, 0xf0e, 0); // Write expansion reg 0xE
      smi_reg(smi1, 0x15, 0x800, 0); // 0xE is written via register 0x15, Enable MII-Lite

      ethernet_server_full_two_port(mii1,
                                    mii2,
                                    smi1,
                                    null,
                                    mac_address,
                                    c_mac_rx, NUM_MAC_RX_CHANS,
                                    c_mac_tx, NUM_MAC_TX_CHANS);
    }

    on tile[0]: media_clock_server(c_media_clock_ctl,
                                   null,
                                   c_buf_ctl,
                                   AVB_NUM_LISTENER_UNITS,
                                   p_fs,
                                   c_mac_rx[MAC_RX_TO_MEDIA_CLOCK],
                                   c_mac_tx[MAC_TX_TO_MEDIA_CLOCK],
                                   c_ptp, NUM_PTP_CHANS,
                                   PTP_GRANDMASTER_CAPABLE);


    // AVB - Audio
    on tile[0]:
    {
#if (AVB_I2C_TILE == 0)
      audio_hardware_setup();
#endif
#if AVB_DEMO_ENABLE_TALKER
      media_input_fifo_data_t ififo_data[AVB_NUM_MEDIA_INPUTS];
      media_input_fifo_t ififos[AVB_NUM_MEDIA_INPUTS];
      init_media_input_fifos(ififos, ififo_data, AVB_NUM_MEDIA_INPUTS);
#endif

#if AVB_DEMO_ENABLE_LISTENER
      media_output_fifo_data_t ofifo_data[AVB_NUM_MEDIA_OUTPUTS];
      media_output_fifo_t ofifos[AVB_NUM_MEDIA_OUTPUTS];
      init_media_output_fifos(ofifos, ofifo_data, AVB_NUM_MEDIA_OUTPUTS);
#endif

      i2s_master(i2s_ports,
                 p_aud_din, AVB_NUM_MEDIA_INPUTS,
                 p_aud_dout, AVB_NUM_MEDIA_OUTPUTS,
                 MASTER_TO_WORDCLOCK_RATIO,
                 ififos,
                 ofifos,
                 c_media_ctl[0],
                 0);
    }

#if AVB_DEMO_ENABLE_TALKER
    // AVB Talker - must be on the same tile as the audio interface
    on tile[0]: avb_1722_talker(c_ptp[PTP_TO_TALKER],
                                c_mac_tx[MAC_TX_TO_TALKER],
                                c_talker_ctl[0],
                                AVB_NUM_SOURCES);
#endif

#if AVB_DEMO_ENABLE_LISTENER
    // AVB Listener
    on tile[0]: avb_1722_listener(c_mac_rx[MAC_RX_TO_LISTENER],
                                  c_buf_ctl[0],
                                  null,
                                  c_listener_ctl[0],
                                  AVB_NUM_SINKS);
#endif

    // on tile[AVB_GPIO_TILE]: gpio_task(c_gpio_ctl);

    // Application
    on tile[1]:
    {
#if (AVB_I2C_TILE == 1)
      audio_hardware_setup();
#endif
      [[combine]] par {
        avb_manager(i_avb, NUM_AVB_MANAGER_CHANS,
                   i_srp,
                   c_media_ctl,
                   c_listener_ctl,
                   c_talker_ctl,
                   c_mac_tx[MAC_TX_TO_AVB_MANAGER],
                   c_media_clock_ctl,
                   c_ptp[PTP_TO_AVB_MANAGER]);
        demo_task(i_avb[AVB_MANAGER_TO_DEMO], c_gpio_ctl);
        avb_srp_task(i_avb[AVB_MANAGER_TO_SRP],
                     i_srp,
                     c_mac_rx[MAC_RX_TO_SRP],
                     c_mac_tx[MAC_TX_TO_SRP]);
      }

    }

    on tile[0]: avb_1722_1_task(i_avb[AVB_MANAGER_TO_1722_1],
                                c_mac_rx[MAC_RX_TO_1722_1],
                                c_mac_tx[MAC_TX_TO_1722_1],
                                c_ptp[PTP_TO_1722_1]);

    on tile[0]: ptp_output_test_clock(c_ptp[PTP_TO_TEST_CLOCK],
                                      ptp_sync_port, 100000000);

  }

    return 0;
}

void gpio_task(chanend c_gpio_ctl)
{
  int button_val;
  int buttons_active = 1;
  int toggle_remote = 0;
  unsigned buttons_timeout;
  int selected_chan = 0;
  timer button_tmr;

  p_mute_led_remote <: ~0;
  p_chan_leds <: ~(1 << selected_chan);
  p_buttons :> button_val;

  while (1)
  {
    select
    {
      case buttons_active => p_buttons when pinsneq(button_val) :> unsigned new_button_val:
        if ((button_val & STREAM_SEL) == STREAM_SEL && (new_button_val & STREAM_SEL) == 0)
        {
          c_gpio_ctl <: STREAM_SEL;
          buttons_active = 0;
        }
        if ((button_val & REMOTE_SEL) == REMOTE_SEL && (new_button_val & REMOTE_SEL) == 0)
        {
          c_gpio_ctl <: REMOTE_SEL;
          toggle_remote = !toggle_remote;
          buttons_active = 0;
          p_mute_led_remote <: (~0) & ~(toggle_remote<<1);
        }
        if ((button_val & CHAN_SEL) == CHAN_SEL && (new_button_val & CHAN_SEL) == 0)
        {
          selected_chan++;
          if (selected_chan > ((AVB_NUM_MEDIA_OUTPUTS>>1)-1))
          {
            selected_chan = 0;
          }
          p_chan_leds <: ~(1 << selected_chan);
          c_gpio_ctl <: CHAN_SEL;
          c_gpio_ctl <: selected_chan;
          buttons_active = 0;
        }
        if (!buttons_active)
        {
          button_tmr :> buttons_timeout;
          buttons_timeout += BUTTON_TIMEOUT_PERIOD;
        }
        button_val = new_button_val;
        break;
      case !buttons_active => button_tmr when timerafter(buttons_timeout) :> void:
        buttons_active = 1;
        p_buttons :> button_val;
        break;
    }
  }

}


/** The main application control task **/
[[combinable]]
void demo_task(client interface avb_interface avb, chanend c_gpio_ctl)
{
#if AVB_DEMO_ENABLE_TALKER
  int map[AVB_NUM_MEDIA_INPUTS];
#endif
  unsigned sample_rate = 48000;
  int change_stream = 1;
  int toggle_remote = 0;

  // Initialize the media clock
  avb.set_device_media_clock_type(0, DEVICE_MEDIA_CLOCK_INPUT_STREAM_DERIVED);
  avb.set_device_media_clock_rate(0, sample_rate);
  avb.set_device_media_clock_state(0, DEVICE_MEDIA_CLOCK_STATE_ENABLED);

#if AVB_DEMO_ENABLE_TALKER
  avb.set_source_channels(0, AVB_NUM_MEDIA_INPUTS);
  for (int i = 0; i < AVB_NUM_MEDIA_INPUTS; i++)
    map[i] = i;
  avb.set_source_map(0, map, AVB_NUM_MEDIA_INPUTS);
  avb.set_source_format(0, AVB_SOURCE_FORMAT_MBLA_24BIT, sample_rate);
  avb.set_source_sync(0, 0); // use the media_clock defined above
#endif

  avb.set_sink_format(0, AVB_SOURCE_FORMAT_MBLA_24BIT, sample_rate);

  while (1)
  {
    select
    {
      // Receive any events from user button presses from the GPIO task
      case c_gpio_ctl :> int cmd:
      {
#if 0
        switch (cmd)
        {
          case STREAM_SEL:
          {
            change_stream = 1;
            break;
          }
          case CHAN_SEL:
          {
            int selected_chan;
            c_gpio_ctl :> selected_chan;
#if AVB_DEMO_ENABLE_LISTENER
            if (AVB_NUM_MEDIA_OUTPUTS > 2)
            {
              enum avb_sink_state_t cur_state;
              int channel;
              int map[AVB_NUM_MEDIA_OUTPUTS];

              channel = selected_chan*2;
              get_avb_sink_state(0, &cur_state);
              set_avb_sink_state(0, AVB_SINK_STATE_DISABLED);
              for (int j=0;j<AVB_NUM_MEDIA_OUTPUTS;j++)
              {
                map[j] = channel;
                channel++;
                if (channel > AVB_NUM_MEDIA_OUTPUTS-1)
                {
                  channel = 0;
                }
              }
              set_avb_sink_map(0, map, AVB_NUM_MEDIA_OUTPUTS);
              if (cur_state != AVB_SINK_STATE_DISABLED)
                set_avb_sink_state(0, AVB_SINK_STATE_POTENTIAL);
            }
#endif
            break;
          }
          case REMOTE_SEL:
          {
            toggle_remote = !toggle_remote;
            break;
          }
          break;
        }
#endif
        break;
      }
    } // end select
  } // end while
}

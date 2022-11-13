#include "_Plugin_Helper.h"
#ifdef USES_P143

// #######################################################################################################
// ############################## Plugin 143 Switch input - I2C Rotary encoders ##########################
// #######################################################################################################

/** Changelog:
 * 2022-11-12 tonhuisman: Implement counter to color mapping, button support
 * 2022-11-10 tonhuisman: Implement Adafruit I2C Encoder support, with NeoPixel and pushbutton
 * 2022-11-04 tonhuisman: Initial plugin creation
 *
 */

# define PLUGIN_143
# define PLUGIN_ID_143         143
# define PLUGIN_NAME_143       "Switch input - I2C Rotary encoders [TESTING]" // TODO: Remove Testing tag
# define PLUGIN_VALUENAME1_143 "Counter"
# define PLUGIN_VALUENAME2_143 "State"

# include "./src/PluginStructs/P143_data_struct.h"

boolean Plugin_143(uint8_t function, struct EventStruct *event, String& string)
{
  boolean success = false;

  switch (function)
  {
    case PLUGIN_DEVICE_ADD:
    {
      Device[++deviceCount].Number           = PLUGIN_ID_143;
      Device[deviceCount].Type               = DEVICE_TYPE_I2C;
      Device[deviceCount].VType              = Sensor_VType::SENSOR_TYPE_DUAL;
      Device[deviceCount].Ports              = 0;
      Device[deviceCount].PullUpOption       = false;
      Device[deviceCount].InverseLogicOption = false;
      Device[deviceCount].FormulaOption      = true;
      Device[deviceCount].ValueCount         = 2;
      Device[deviceCount].SendDataOption     = true;
      Device[deviceCount].TimerOption        = false;
      Device[deviceCount].TimerOptional      = true;
      Device[deviceCount].GlobalSyncOption   = true;
      break;
    }

    case PLUGIN_GET_DEVICENAME:
    {
      string = F(PLUGIN_NAME_143);
      break;
    }

    case PLUGIN_GET_DEVICEVALUENAMES:
    {
      strcpy_P(ExtraTaskSettings.TaskDeviceValueNames[0], PSTR(PLUGIN_VALUENAME1_143));
      strcpy_P(ExtraTaskSettings.TaskDeviceValueNames[1], PSTR(PLUGIN_VALUENAME2_143));
      break;
    }

    case PLUGIN_I2C_HAS_ADDRESS:
    case PLUGIN_WEBFORM_SHOW_I2C_PARAMS:
    {
      // Adafruit 0x36..0x3D, M5Stack 0x40, DFRobot 0x54..0x57
      const uint8_t i2cAddressValues[] = { 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D,
                                           # if P143_FEATURE_INCLUDE_M5STACK
                                           0x40,
                                           # endif // if P143_FEATURE_INCLUDE_M5STACK
                                           # if P143_FEATURE_INCLUDE_DFROBOT
                                           0x54, 0x55, 0x56, 0x57
                                           # endif // if P143_FEATURE_INCLUDE_DFROBOT
      };

      if (function == PLUGIN_WEBFORM_SHOW_I2C_PARAMS) {
        int addrOffset = 0;
        int addrLength = 13;

        switch (static_cast<P143_DeviceType_e>(P143_ENCODER_TYPE)) {
          case P143_DeviceType_e::AdafruitEncoder:
            addrLength = 8;
            break;
          # if P143_FEATURE_INCLUDE_M5STACK
          case P143_DeviceType_e::M5StackEncoder:
            addrOffset = 8;
            addrLength = 1;
            break;
          # endif // if P143_FEATURE_INCLUDE_M5STACK
          # if P143_FEATURE_INCLUDE_DFROBOT
          case P143_DeviceType_e::DFRobotEncoder:
            addrOffset = 9;
            addrLength = 4;
            break;
          # endif // if P143_FEATURE_INCLUDE_DFROBOT
        }
        addFormSelectorI2C(F("pi2c"), addrLength, &i2cAddressValues[addrOffset], P143_I2C_ADDR);
      } else {
        int addrCount = 8;
        # if P143_FEATURE_INCLUDE_M5STACK
        addrCount++;
        # endif // if P143_FEATURE_INCLUDE_M5STACK
        # if P143_FEATURE_INCLUDE_DFROBOT
        addrCount += 4;
        # endif // if P143_FEATURE_INCLUDE_DFROBOT
        success = intArrayContains(addrCount, i2cAddressValues, event->Par1);
      }
      break;
    }

    case PLUGIN_WEBFORM_SHOW_GPIO_DESCR:
    {
      string  = F("Encoder: ");
      string += toString(static_cast<P143_DeviceType_e>(P143_ENCODER_TYPE));
      string += F(" (");
      string += formatToHex(P143_I2C_ADDR);
      string += ')';
      success = true;
      break;
    }

    case PLUGIN_SET_DEFAULTS:
    {
      P143_I2C_ADDR      = 0x36;
      P143_ENCODER_TYPE  = static_cast<int16_t>(P143_DeviceType_e::AdafruitEncoder);
      P143_PREVIOUS_TYPE = -1;                          // 'Changed' to get defaults

      ExtraTaskSettings.TaskDeviceValueDecimals[0] = 0; // Count doesn't have decimals
      ExtraTaskSettings.TaskDeviceValueDecimals[1] = 0; // State doesn't have decimals
      break;
    }

    case PLUGIN_WEBFORM_LOAD:
    {
      P143_CheckEncoderDefaultSettings(event); // Update defaults after changing Encoder type

      {
        const __FlashStringHelper *selectModeOptions[] = {
          toString(P143_DeviceType_e::AdafruitEncoder),
          # if P143_FEATURE_INCLUDE_M5STACK
          toString(P143_DeviceType_e::M5StackEncoder),
          # endif // if P143_FEATURE_INCLUDE_M5STACK
          # if P143_FEATURE_INCLUDE_DFROBOT
          toString(P143_DeviceType_e::DFRobotEncoder),
          # endif // if P143_FEATURE_INCLUDE_DFROBOT
        };
        const int selectModeValues[] = {
          static_cast<int>(P143_DeviceType_e::AdafruitEncoder),
          # if P143_FEATURE_INCLUDE_M5STACK
          static_cast<int>(P143_DeviceType_e::M5StackEncoder),
          # endif // if P143_FEATURE_INCLUDE_M5STACK
          # if P143_FEATURE_INCLUDE_DFROBOT
          static_cast<int>(P143_DeviceType_e::DFRobotEncoder)
          # endif // if P143_FEATURE_INCLUDE_DFROBOT
        };
        addFormSelector(F("Encoder type"),
                        F("pdevice"),
                        sizeof(selectModeValues) / sizeof(int),
                        selectModeOptions,
                        selectModeValues,
                        P143_ENCODER_TYPE,
                        true);
        addFormNote(F("Changing the Encoder type will reload the page and reset Encoder specific settings to default!"));
      }

      P143_DeviceType_e device = static_cast<P143_DeviceType_e>(P143_ENCODER_TYPE);

      addFormSubHeader(concat(F("Encoder specific: "), toString(device)));

      switch (device) {
        case P143_DeviceType_e::AdafruitEncoder:
        {
          {
            addRowLabel(F("Neopixel initial color"));
            addHtml(F("<table style='padding-left:0;'>")); // remove left padding 2x to align vertically with other inputs
            html_TD(F("padding-left:0"));
            addHtml('R');
            addNumericBox(F("pred"), P143_ADAFRUIT_COLOR_RED, 0, 255);
            html_TD();
            addHtml('G');
            addNumericBox(F("pgreen"), P143_ADAFRUIT_COLOR_GREEN, 0, 255);
            html_TD();
            addHtml('B');
            addNumericBox(F("pblue"), P143_ADAFRUIT_COLOR_BLUE, 0, 255);
            addUnit(F("0..255"));
            html_end_table();
          }
          addFormNumericBox(F("Initial brightness"), F("pbright"), P143_ADAFRUIT_BRIGHTNESS, 1, 255);
          addUnit(F("1..255"));
          break;
        }
        # if P143_FEATURE_INCLUDE_M5STACK
        case P143_DeviceType_e::M5StackEncoder:
        {
          // TODO
          break;
        }
        # endif // if P143_FEATURE_INCLUDE_M5STACK
        # if P143_FEATURE_INCLUDE_DFROBOT
        case P143_DeviceType_e::DFRobotEncoder:
        {
          // TODO
          break;
        }
        # endif // if P143_FEATURE_INCLUDE_DFROBOT
      }

      addFormSubHeader(F("Generic settings"));

      addFormNumericBox(F("Initial encoder position"), F("pinitpos"), P143_INITIAL_POSITION);
      addFormNumericBox(F("Lowest encoder position"),  F("pminpos"),  P143_MINIMAL_POSITION);
      addFormNumericBox(F("Highest encoder position"), F("pmaxpos"),  P143_MAXIMAL_POSITION);
      addFormNote(F("Not checked if Lowest = Highest."));

      {
        const __FlashStringHelper *selectButtonOptions[] = {
          toString(P143_ButtonAction_e::PushButton),
          toString(P143_ButtonAction_e::PushButtonInverted),
          toString(P143_ButtonAction_e::ToggleSwitch),
        };
        const int selectButtonValues[] = {
          static_cast<int>(P143_ButtonAction_e::PushButton),
          static_cast<int>(P143_ButtonAction_e::PushButtonInverted),
          static_cast<int>(P143_ButtonAction_e::ToggleSwitch),
        };
        addFormSelector(F("Button action"),
                        F("pbutton"),
                        sizeof(selectButtonValues) / sizeof(int),
                        selectButtonOptions,
                        selectButtonValues,
                        P143_PLUGIN_BUTTON_ACTION);
      }

      # if P143_FEATURE_COUNTER_COLORMAPPING
      {
        const __FlashStringHelper *selectCounterOptions[] = {
          toString(P143_CounterMapping_e::None),
          toString(P143_CounterMapping_e::ColorMapping),
          toString(P143_CounterMapping_e::ColorGradient),
        };
        const int selectCounterValues[] = {
          static_cast<int>(P143_CounterMapping_e::None),
          static_cast<int>(P143_CounterMapping_e::ColorMapping),
          static_cast<int>(P143_CounterMapping_e::ColorGradient),
        };
        addFormSelector(F("Counter color mapping"),
                        F("pmap"),
                        sizeof(selectCounterValues) / sizeof(int),
                        selectCounterOptions,
                        selectCounterValues,
                        P143_PLUGIN_COUNTER_MAPPING);
      }
      {
        String strings[P143_STRINGS];
        LoadCustomTaskSettings(event->TaskIndex, strings, P143_STRINGS, 0);

        addRowLabel(F("Colormap"));
        html_table(EMPTY_STRING);

        for (int varNr = 0; varNr < P143_STRINGS; varNr++) {
          html_TR_TD();

          // if (varNr < 9) { addHtml(F("&nbsp;")); }
          addHtml('#');
          addHtmlInt(varNr + 1);
          html_TD();
          addTextBox(getPluginCustomArgName(varNr), strings[varNr], P143_STRING_LEN, false, false, EMPTY_STRING, F("xwide"));
        }
        html_end_table();
      }
      # endif // if P143_FEATURE_COUNTER_COLORMAPPING

      success = true;
      break;
    }

    case PLUGIN_WEBFORM_SAVE:
    {
      P143_I2C_ADDR         = getFormItemInt(F("pi2c"));
      P143_ENCODER_TYPE     = getFormItemInt(F("pdevice"));
      P143_INITIAL_POSITION = getFormItemInt(F("pinitpos"));
      P143_MINIMAL_POSITION = getFormItemInt(F("pminpos"));
      P143_MAXIMAL_POSITION = getFormItemInt(F("pmaxpos"));

      uint32_t lSettings = 0u;

      switch (static_cast<P143_DeviceType_e>(P143_ENCODER_TYPE)) {
        case P143_DeviceType_e::AdafruitEncoder:
        {
          // Color settings
          set8BitToUL(lSettings, P143_ADAFRUIT_OFFSET_RED,        getFormItemInt(F("pred")) & 0xFF);
          set8BitToUL(lSettings, P143_ADAFRUIT_OFFSET_GREEN,      getFormItemInt(F("pgreen")) & 0xFF);
          set8BitToUL(lSettings, P143_ADAFRUIT_OFFSET_BLUE,       getFormItemInt(F("pblue")) & 0xFF);
          set8BitToUL(lSettings, P143_ADAFRUIT_OFFSET_BRIGHTNESS, getFormItemInt(F("pbright")) & 0xFF);
          P143_ADAFRUIT_COLOR_AND_BRIGHTNESS = lSettings;

          break;
        }
        # if P143_FEATURE_INCLUDE_M5STACK
        case P143_DeviceType_e::M5StackEncoder:
        {
          // TODO
          break;
        }
        # endif // if P143_FEATURE_INCLUDE_M5STACK
        # if P143_FEATURE_INCLUDE_DFROBOT
        case P143_DeviceType_e::DFRobotEncoder:
        {
          // TODO
          break;
        }
        # endif // if P143_FEATURE_INCLUDE_DFROBOT
      }

      // Flags
      lSettings = 0u;
      # if P143_FEATURE_COUNTER_COLORMAPPING
      set4BitToUL(lSettings, 0, getFormItemInt(F("pmap")) & 0x0F);
      # endif // if P143_FEATURE_COUNTER_COLORMAPPING
      set4BitToUL(lSettings, 4, getFormItemInt(F("pbutton")) & 0x0F);
      P143_PLUGIN_FLAGS = lSettings;

      # if P143_FEATURE_COUNTER_COLORMAPPING

      // colormap
      String strings[P143_STRINGS];

      for (int varNr = 0; varNr < P143_STRINGS; varNr++) {
        strings[varNr] = webArg(getPluginCustomArgName(varNr));
      }
      String error = SaveCustomTaskSettings(event->TaskIndex, strings, P143_STRINGS, 0);

      if (error.length() > 0) {
        addHtmlError(error);
      }
      # endif // if P143_FEATURE_COUNTER_COLORMAPPING

      success = true;
      break;
    }

    case PLUGIN_INIT:
    {
      initPluginTaskData(event->TaskIndex, new (std::nothrow) P143_data_struct(event));
      P143_data_struct *P143_data = static_cast<P143_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr != P143_data) {
        success = P143_data->plugin_init(event);
      }
      break;
    }

    case PLUGIN_EXIT:
    {
      P143_data_struct *P143_data = static_cast<P143_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr != P143_data) {
        success = P143_data->plugin_exit(event);
      }
      break;
    }

    case PLUGIN_READ:
    {
      P143_data_struct *P143_data = static_cast<P143_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr != P143_data) {
        success = P143_data->plugin_read(event);
      }

      break;
    }

    case PLUGIN_TEN_PER_SECOND:
    {
      P143_data_struct *P143_data = static_cast<P143_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr != P143_data) {
        success = P143_data->plugin_ten_per_second(event);
      }

      break;
    }

    case PLUGIN_FIFTY_PER_SECOND:
    {
      P143_data_struct *P143_data = static_cast<P143_data_struct *>(getPluginTaskData(event->TaskIndex));

      if (nullptr != P143_data) {
        success = P143_data->plugin_fifty_per_second(event);
      }

      break;
    }
  }
  return success;
}

#endif // ifdef USES_P143

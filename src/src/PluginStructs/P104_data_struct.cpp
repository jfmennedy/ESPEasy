#include "../PluginStructs/P104_data_struct.h"

#ifdef USES_P104

# include "../Helpers/ESPEasy_Storage.h"
# include "../Helpers/Numerical.h"
# include "../WebServer/Markup_Forms.h"
# include "../WebServer/ESPEasy_WebServer.h"
# include "../WebServer/Markup.h"
# include "../WebServer/HTML_wrappers.h"
# include "../ESPEasyCore/ESPEasyRules.h"
# include "../Globals/ESPEasy_time.h"
# include "../Globals/RTC.h"

# include <vector>
# include <MD_Parola.h>
# include <MD_MAX72xx.h>

// Needed also here for PlatformIO's library finder as the .h file
// is in a directory which is excluded in the src_filter

# if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
void createHString(String& string); // Forward definition
# endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
void reverseStr(String& str);       // Forward definition

/****************************************************************
 * Constructor
 ***************************************************************/
P104_data_struct::P104_data_struct(MD_MAX72XX::moduleType_t _mod,
                                   taskIndex_t              _taskIndex,
                                   int8_t                   _cs_pin,
                                   uint8_t                  _modules,
                                   uint8_t                  _zonesCount)
  : mod(_mod), taskIndex(_taskIndex), cs_pin(_cs_pin), modules(_modules), expectedZones(_zonesCount) {
  if (Settings.isSPI_valid()) {
    P = new (std::nothrow) MD_Parola(mod, cs_pin, modules);
  } else {
    addLog(LOG_LEVEL_ERROR, F("DOTMATRIX: Required SPI not enabled. Initialization aborted!"));
  }
}

/*******************************
 * Destructor
 ******************************/
P104_data_struct::~P104_data_struct() {
  # if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)

  if (nullptr != pM) {
    pM = nullptr; // Not created here, only reset
  }
  # endif // if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)

  if (nullptr != P) {
    // P->~MD_Parola(); // Call destructor directly, as delete of the object fails miserably
    // do not: delete P; // Warning: the MD_Parola object doesn't have a virtual destructor, and when changed,
    // a reboot uccurs when the object is deleted here!
    P = nullptr; // Reset only
  }
}

/*******************************
 * Initializer/starter
 ******************************/
bool P104_data_struct::begin() {
  if (!initialized) {
    loadSettings();
    initialized = true;
  }

  if ((P != nullptr) && validGpio(cs_pin)) {
    # ifdef P104_DEBUG
    addLog(LOG_LEVEL_INFO, F("dotmatrix: begin() called"));
    # endif // ifdef P104_DEBUG
    P->begin(expectedZones);
    # if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)
    pM = P->getGraphicObject();
    # endif // if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)
    return true;
  }
  return false;
}

# define P104_ZONE_SEP   '\x02'
# define P104_FIELD_SEP  '\x01'
# define P104_ZONE_DISP  ';'
# define P104_FIELD_DISP ','

# define P104_CONFIG_VERSION_V2  0xF000 // Marker in first uint16_t to to indicate second version config settings, anything else if first
                                        // version.
                                        // Any third version or later could use 0xE000, etc. The 'version' is stored in the first uint16_t
                                        // stored in the custom settings
# define P104_CONFIG_VERSION_V3  0xE000 // Marker to indicate we're using V3 of the settings, same base-format as V2, but using the
                                        // CustomTaskSettings Extension file only, by inserting an offset of DAT_TASKS_CUSTOM_SIZE
                                        // ATTENTION: V3 is _only_ activated for FEATURE_EXTENDED_CUSTOM_SETTINGS, ESP32 & USE_LITTLEFS !!!

/*
   Settings layout:
   Version 1:
   - uint16_t : size of the next blob holding all settings
   - char[x]  : Blob with settings, with csv-like strings, using P104_FIELD_SEP and P104_ZONE_SEP separators
   Version 2:
   - uint16_t : marker with content P104_CONFIG_VERSION_V2
   - uint16_t : size of next blob holding 1 zone settings string
   - char[y]  : Blob holding 1 zone settings string, with csv like string, using P104_FIELD_SEP separators
   - uint16_t : next size, if 0 then no more blobs
   - char[x]  : Blob
   - ...
   - Max. allowed total custom settings size = 1024
   Version 3:
   - uint16_t : marker with content P104_CONFIG_VERSION_V2
   - empty space, size of DAT_TASKS_CUSTOM_SIZE - 2 so the actual storage is in the extension file
   - uint16_t : size of next blob holding 1 zone settings string
   - char[y]  : Blob holding 1 zone settings string, with csv like string, using P104_FIELD_SEP separators
   - uint16_t : next size, if 0 then no more blobs
   - char[x]  : Blob
   - ...
   - Max. allowed total custom settings size = 4096
 */
/**************************************
 * loadSettings
 *************************************/
void P104_data_struct::loadSettings() {
  uint16_t bufferSize;
  char    *settingsBuffer;

  if (validTaskIndex(taskIndex)) {
    int loadOffset = 0;

    // Read size of the used buffer, could be the settings-version marker
    LoadFromFile(SettingsType::Enum::CustomTaskSettings_Type, taskIndex, (uint8_t *)&bufferSize, sizeof(bufferSize), loadOffset);
    bool settingsVersionV2  = (bufferSize == P104_CONFIG_VERSION_V2) || (bufferSize == 0u);
    bool settingsVersionV3  = (bufferSize == P104_CONFIG_VERSION_V3) || (bufferSize == 0u);
    uint16_t structDataSize = 0;
    uint16_t reservedBuffer = 0;

    if (!settingsVersionV2 && !settingsVersionV3) {
      reservedBuffer = bufferSize + 1;                         // just add 1 for storing a string-terminator
      addLog(LOG_LEVEL_INFO, F("dotmatrix: Reading Settings V1, will be stored as Settings V2/V3."));
    } else {
      reservedBuffer = P104_SETTINGS_BUFFER_V2 + 1;            // just add 1 for storing a string-terminator
    }
    reservedBuffer++;                                          // Add 1 for 0..size use
    settingsBuffer = new (std::nothrow)char[reservedBuffer](); // Allocate buffer and reset to all zeroes
    # if P104_FEATURE_STORAGE_V3

    if (settingsVersionV3) {
      loadOffset = DAT_TASKS_CUSTOM_SIZE; // Skip storage in config.dat
    } else {
      loadOffset += sizeof(bufferSize);
    }
    # else // if P104_FEATURE_STORAGE_V3
    loadOffset += sizeof(bufferSize);
    # endif // if P104_FEATURE_STORAGE_V3

    if (settingsVersionV2 || settingsVersionV3) {
      LoadFromFile(SettingsType::Enum::CustomTaskSettings_Type, taskIndex, (uint8_t *)&bufferSize, sizeof(bufferSize), loadOffset);
      loadOffset += sizeof(bufferSize); // Skip the size
    }
    structDataSize = bufferSize;
    # ifdef P104_DEBUG_DEV

    if (loglevelActiveFor(LOG_LEVEL_INFO)) {
      addLogMove(LOG_LEVEL_INFO, strformat(F("P104: loadSettings stored Size: %d taskindex: %d"), structDataSize, taskIndex));
    }
    # endif // ifdef P104_DEBUG_DEV

    // Read actual data
    if (structDataSize > 0) {              // Reading 0 bytes logs an error, so lets avoid that
      LoadFromFile(SettingsType::Enum::CustomTaskSettings_Type, taskIndex, (uint8_t *)settingsBuffer, structDataSize, loadOffset);
    }
    settingsBuffer[bufferSize + 1] = '\0'; // Terminate string

    uint8_t zoneIndex = 0;

    {
      String buffer(settingsBuffer);
      # ifdef P104_DEBUG_DEV

      String log;

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        log = strformat(F("P104: loadSettings bufferSize: %d untrimmed: %d"), bufferSize, buffer.length());
      }
      # endif // ifdef P104_DEBUG_DEV
      buffer.trim();
      # ifdef P104_DEBUG_DEV

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        log += concat(F(" trimmed: "), buffer.length());
        addLogMove(LOG_LEVEL_INFO, log);
      }
      # endif // ifdef P104_DEBUG_DEV

      if (zones.size() > 0) {
        zones.clear();
      }
      zones.reserve(P104_MAX_ZONES);
      numDevices = 0;

      String   tmp;
      String   fld;
      int32_t  tmp_int;
      uint16_t prev2   = 0;
      int16_t  offset2 = buffer.indexOf(P104_ZONE_SEP);

      if ((offset2 == -1) && (buffer.length() > 0)) {
        offset2 = buffer.length();
      }

      while (offset2 > -1) {
        tmp = buffer.substring(prev2, offset2);
        # ifdef P104_DEBUG_DEV

        if (loglevelActiveFor(LOG_LEVEL_INFO)) {
          log  = F("P104: reading string: ");
          log += tmp;
          log.replace(P104_FIELD_SEP, P104_FIELD_DISP);
          addLogMove(LOG_LEVEL_INFO, log);
        }
        # endif // ifdef P104_DEBUG_DEV

        zones.push_back(P104_zone_struct(zoneIndex + 1));

        tmp_int = 0;

        // WARNING: Order of parsing these values should match the numeric order of P104_OFFSET_* values
        for (uint8_t i = 0; i < P104_OFFSET_COUNT; ++i) {
          if (i == P104_OFFSET_TEXT) {
            zones[zoneIndex].text = parseStringKeepCaseNoTrim(tmp, 1 + P104_OFFSET_TEXT, P104_FIELD_SEP);
          } else {
            if (validIntFromString(parseString(tmp, 1 + i, P104_FIELD_SEP), tmp_int)) {
              zones[zoneIndex].setIntValue(i, tmp_int);
            }
          }
        }

        delay(0);

        numDevices += zones[zoneIndex].size + zones[zoneIndex].offset;

        if (!settingsVersionV2 && !settingsVersionV3) { // V1 check
          prev2   = offset2 + 1;
          offset2 = buffer.indexOf(P104_ZONE_SEP, prev2);
        } else {
          loadOffset    += bufferSize;
          structDataSize = sizeof(bufferSize);
          LoadFromFile(SettingsType::Enum::CustomTaskSettings_Type, taskIndex, (uint8_t *)&bufferSize, structDataSize, loadOffset);
          offset2 = bufferSize;  // Length

          if (bufferSize == 0) { // End of zones reached
            offset2 = -1;        // fall out of while loop
          } else {
            structDataSize = bufferSize;
            loadOffset    += sizeof(bufferSize);
            LoadFromFile(SettingsType::Enum::CustomTaskSettings_Type, taskIndex, (uint8_t *)settingsBuffer, structDataSize, loadOffset);
            settingsBuffer[bufferSize + 1] = '\0'; // Terminate string
            buffer                         = String(settingsBuffer);
          }
        }
        zoneIndex++;

        # ifdef P104_DEBUG

        if (loglevelActiveFor(LOG_LEVEL_INFO)) {
          addLogMove(LOG_LEVEL_INFO, concat(F("dotmatrix: parsed zone: "), zoneIndex));
        }
        # endif // ifdef P104_DEBUG
      }

      buffer = String();     // Free some memory
    }

    delete[] settingsBuffer; // Release allocated buffer
    # ifdef P104_DEBUG_DEV

    if (loglevelActiveFor(LOG_LEVEL_INFO)) {
      addLogMove(LOG_LEVEL_INFO, concat(F("P104: read zones from config: "), zoneIndex));
    }
    # endif // ifdef P104_DEBUG_DEV

    if (expectedZones == -1) { expectedZones = zoneIndex; }

    if (expectedZones == 0) { expectedZones++; } // Guarantee at least 1 zone to be displayed

    while (zoneIndex < expectedZones) {
      zones.push_back(P104_zone_struct(zoneIndex + 1));

      if (equals(zones[zoneIndex].text, F("\"\""))) { // Special case
        zones[zoneIndex].text.clear();
      }

      zoneIndex++;
      delay(0);
    }
    # ifdef P104_DEBUG_DEV

    if (loglevelActiveFor(LOG_LEVEL_INFO)) {
      addLogMove(LOG_LEVEL_INFO, strformat(F("P104: total zones initialized: %d expected: %d"), zoneIndex, expectedZones));
    }
    # endif // ifdef P104_DEBUG_DEV
  }
}

/****************************************************
 * configureZones: initialize Zones setup
 ***************************************************/
void P104_data_struct::configureZones() {
  if (!initialized) {
    loadSettings();
    initialized = true;
  }

  uint8_t currentZone = 0;
  uint8_t zoneOffset  = 0;

  # ifdef P104_DEBUG_DEV

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    addLogMove(LOG_LEVEL_INFO, concat(F("P104: configureZones to do: "), zones.size()));
  }
  # endif // ifdef P104_DEBUG_DEV

  if (nullptr == P) { return; }

  P->displayClear();

  for (auto it = zones.begin(); it != zones.end(); ++it) {
    if (it->zone <= expectedZones) {
      zoneOffset += it->offset;
      P->setZone(currentZone, zoneOffset, zoneOffset + it->size - 1);
      # if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)
      it->_startModule = zoneOffset;
      P->getDisplayExtent(currentZone, it->_lower, it->_upper);
      # endif // if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)
      zoneOffset += it->size;

      switch (it->font) {
        # ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
        case P104_DOUBLE_HEIGHT_FONT_ID: {
          P->setFont(currentZone, numeric7SegDouble);
          P->setCharSpacing(currentZone, P->getCharSpacing() * 2); // double spacing as well
          break;
        }
        # endif // ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
        # ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
        case P104_FULL_DOUBLEHEIGHT_FONT_ID: {
          P->setFont(currentZone, BigFont);
          P->setCharSpacing(currentZone, P->getCharSpacing() * 2); // double spacing as well
          break;
        }
        # endif // ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
        # ifdef P104_USE_VERTICAL_FONT
        case P104_VERTICAL_FONT_ID: {
          P->setFont(currentZone, _fontVertical);
          break;
        }
        # endif // ifdef P104_USE_VERTICAL_FONT
        # ifdef P104_USE_EXT_ASCII_FONT
        case P104_EXT_ASCII_FONT_ID: {
          P->setFont(currentZone, ExtASCII);
          break;
        }
        # endif // ifdef P104_USE_EXT_ASCII_FONT
        # ifdef P104_USE_ARABIC_FONT
        case P104_ARABIC_FONT_ID: {
          P->setFont(currentZone, fontArabic);
          break;
        }
        # endif // ifdef P104_USE_ARABIC_FONT
        # ifdef P104_USE_GREEK_FONT
        case P104_GREEK_FONT_ID: {
          P->setFont(currentZone, fontGreek);
          break;
        }
        # endif // ifdef P104_USE_GREEK_FONT
        # ifdef P104_USE_KATAKANA_FONT
        case P104_KATAKANA_FONT_ID: {
          P->setFont(currentZone, fontKatakana);
          break;
        }
        # endif // ifdef P104_USE_KATAKANA_FONT

        // Extend above this comment with more fonts if/when available,
        // case P104_DEFAULT_FONT_ID: and default: clauses should be the last options.
        // This should also make sure the default font is set if a no longer available font was selected
        case P104_DEFAULT_FONT_ID:
        default: {
          P->setFont(currentZone, nullptr); // default font
          break;
        }
      }

      // Inverted
      P->setInvert(currentZone, it->inverted);

      // Special Effects
      P->setZoneEffect(currentZone, (it->specialEffect & P104_SPECIAL_EFFECT_UP_DOWN) == P104_SPECIAL_EFFECT_UP_DOWN,       PA_FLIP_UD);
      P->setZoneEffect(currentZone, (it->specialEffect & P104_SPECIAL_EFFECT_LEFT_RIGHT) == P104_SPECIAL_EFFECT_LEFT_RIGHT, PA_FLIP_LR);

      // Brightness
      P->setIntensity(currentZone, it->brightness);

      # ifdef P104_DEBUG_DEV

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        addLogMove(LOG_LEVEL_INFO, strformat(F("P104: configureZones #%d/%d offset: %d"), currentZone + 1, expectedZones, zoneOffset));
      }
      # endif // ifdef P104_DEBUG_DEV

      delay(0);

      // Content == text && text != ""
      if (((it->content == P104_CONTENT_TEXT) ||
           (it->content == P104_CONTENT_TEXT_REV))
          && (!it->text.isEmpty())) {
        displayOneZoneText(currentZone, *it, it->text);
      }

      # ifdef P104_USE_BAR_GRAPH

      // Content == Bar-graph && text != ""
      if ((it->content == P104_CONTENT_BAR_GRAPH)
          && (!it->text.isEmpty())) {
        displayBarGraph(currentZone, *it, it->text);
      }
      # endif // ifdef P104_USE_BAR_GRAPH

      if (it->repeatDelay > -1) {
        it->_repeatTimer = millis();
      }
      currentZone++;
      delay(0);
    }
  }

  // Synchronize the start
  P->synchZoneStart();
}

/**********************************************************
 * Display the text with attributes for a specific zone
 *********************************************************/
void P104_data_struct::displayOneZoneText(uint8_t                 zone,
                                          const P104_zone_struct& zstruct,
                                          const String          & text) {
  if ((nullptr == P) || (zone >= P104_MAX_ZONES)) { return; } // double check
  sZoneInitial[zone].reserve(text.length());
  sZoneInitial[zone] = text; // Keep the original string for future use
  sZoneBuffers[zone].reserve(text.length());
  sZoneBuffers[zone] = text; // We explicitly want a copy here so it can be modified by parseTemplate()

  sZoneBuffers[zone] = parseTemplate(sZoneBuffers[zone]);

  # if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)

  if (zstruct.layout == P104_LAYOUT_DOUBLE_UPPER) {
    createHString(sZoneBuffers[zone]);
  }
  # endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)

  if (zstruct.content == P104_CONTENT_TEXT_REV) {
    reverseStr(sZoneBuffers[zone]);
  }

  String log;

  if (loglevelActiveFor(LOG_LEVEL_INFO) &&
      logAllText &&
      log.reserve(28 + text.length() + sZoneBuffers[zone].length())) {
    log  = strformat(F("dotmatrix: ZoneText: %d, '"), zone + 1); // UI-number
    log += text;
    log += F("' -> '");
    log += sZoneBuffers[zone];
    log += '\'';
    addLogMove(LOG_LEVEL_INFO, log);
  }

  P->displayZoneText(zone,
                     sZoneBuffers[zone].c_str(),
                     static_cast<textPosition_t>(zstruct.alignment),
                     zstruct.speed,
                     zstruct.pause,
                     static_cast<textEffect_t>(zstruct.animationIn),
                     static_cast<textEffect_t>(zstruct.animationOut));
}

/*********************************************
 * Update all or the specified zone
 ********************************************/
void P104_data_struct::updateZone(uint8_t                 zone,
                                  const P104_zone_struct& zstruct) {
  if (nullptr == P) { return; }

  if (zone == 0) {
    for (auto it = zones.begin(); it != zones.end(); ++it) {
      if ((it->zone > 0) &&
          ((it->content == P104_CONTENT_TEXT) ||
           (it->content == P104_CONTENT_TEXT_REV))) {
        displayOneZoneText(it->zone - 1, *it, sZoneInitial[it->zone - 1]); // Re-send last displayed text
        P->displayReset(it->zone - 1);
      }
      # ifdef P104_USE_BAR_GRAPH

      if ((it->zone > 0) &&
          (it->content == P104_CONTENT_BAR_GRAPH)) {
        displayBarGraph(it->zone - 1, *it, sZoneInitial[it->zone - 1]); // Re-send last displayed bar graph
      }
      # endif // ifdef P104_USE_BAR_GRAPH

      if ((zstruct.content == P104_CONTENT_TEXT)
          || zstruct.content == P104_CONTENT_TEXT_REV
          # ifdef P104_USE_BAR_GRAPH
          || zstruct.content == P104_CONTENT_BAR_GRAPH
          # endif // ifdef P104_USE_BAR_GRAPH
          ) {
        if (it->repeatDelay > -1) { // Restart repeat timer
          it->_repeatTimer = millis();
        }
      }
    }
  } else {
    if ((zstruct.zone > 0) &&
        ((zstruct.content == P104_CONTENT_TEXT) ||
         (zstruct.content == P104_CONTENT_TEXT_REV))) {
      displayOneZoneText(zstruct.zone - 1, zstruct, sZoneInitial[zstruct.zone - 1]); // Re-send last displayed text
      P->displayReset(zstruct.zone - 1);
    }
    # ifdef P104_USE_BAR_GRAPH

    if ((zstruct.zone > 0) &&
        (zstruct.content == P104_CONTENT_BAR_GRAPH)) {
      displayBarGraph(zstruct.zone - 1, zstruct, sZoneInitial[zstruct.zone - 1]); // Re-send last displayed bar graph
    }
    # endif // ifdef P104_USE_BAR_GRAPH

    // Repeat timer is/should be started elsewhere
  }
}

# if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)

/***********************************************
 * Enable/Disable updating a range of modules
 **********************************************/
void P104_data_struct::modulesOnOff(uint8_t start, uint8_t end, MD_MAX72XX::controlValue_t on_off) {
  for (uint8_t m = start; m <= end; m++) {
    pM->control(m, MD_MAX72XX::UPDATE, on_off);
  }
}

# endif // if defined(P104_USE_BAR_GRAPH) || defined(P104_USE_DOT_SET)

# ifdef P104_USE_BAR_GRAPH

/********************************************************
 * draw a single bar-graph, arguments already adjusted for direction
 *******************************************************/
void P104_data_struct::drawOneBarGraph(uint16_t lower,
                                       uint16_t upper,
                                       int16_t  pixBottom,
                                       int16_t  pixTop,
                                       uint16_t zeroPoint,
                                       uint8_t  barWidth,
                                       uint8_t  barType,
                                       uint8_t  row) {
  bool on_off;

  for (uint8_t r = 0; r < barWidth; r++) {
    for (uint8_t col = lower; col <= upper; col++) {
      on_off = (col >= pixBottom && col <= pixTop); // valid area

      if ((zeroPoint != 0) &&
          (barType == P104_BARTYPE_STANDARD) &&
          (barWidth > 2) &&
          ((r == 0) || (r == barWidth - 1)) &&
          (col == lower + zeroPoint)) {
        on_off = false; // when bar wider than 2, turn off zeropoint top and bottom led
      }

      if ((barType == P104_BARTYPE_SINGLE) && (r > 0)) {
        on_off = false; // barType 1 = only a single line is drawn, independent of the width
      }

      if ((barType == P104_BARTYPE_ALT_DOT) && (barWidth > 1) && on_off) {
        on_off = ((r % 2) == (col % 2)); // barType 2 = dotted line when bar is wider than 1 pixel
      }
      pM->setPoint(row + r, col, on_off);

      if (col % 16 == 0) { delay(0); }
    }
    delay(0); // Leave some breathingroom
  }
}

/********************************************************************
 * Process a graph-string to display in a zone, format:
 * value,max-value,min-value,direction,bartype|...
 *******************************************************************/
void P104_data_struct::displayBarGraph(uint8_t                 zone,
                                       const P104_zone_struct& zstruct,
                                       const String          & graph) {
  if ((nullptr == P) || (nullptr == pM) || graph.isEmpty()) { return; }
  sZoneInitial[zone] = graph; // Keep the original string for future use

  #  define NOT_A_COMMA 0x02  // Something else than a comma, or the parseString function will get confused
  String parsedGraph(graph);  // Extra copy created so we don't mess up the incoming String
  parsedGraph = parseTemplate(parsedGraph);
  parsedGraph.replace(',', NOT_A_COMMA);

  std::vector<P104_bargraph_struct> barGraphs;
  uint8_t currentBar = 0;
  bool    loop       = true;

  // Parse the graph-string
  while (loop && currentBar < 8) { // Maximum 8 valuesets possible
    String graphpart = parseString(parsedGraph, currentBar + 1, '|');
    graphpart.trim();
    graphpart.replace(NOT_A_COMMA, ',');

    if (graphpart.isEmpty()) {
      loop = false;
    } else {
      barGraphs.push_back(P104_bargraph_struct(currentBar));
    }

    if (loop && validDoubleFromString(parseString(graphpart, 1), barGraphs[currentBar].value)) { // value
      String datapart = parseString(graphpart, 2);                                               // max (default: 100.0)

      if (datapart.isEmpty()) {
        barGraphs[currentBar].max = 100.0;
      } else {
        validDoubleFromString(datapart, barGraphs[currentBar].max);
      }
      datapart = parseString(graphpart, 3); // min (default: 0.0)

      if (datapart.isEmpty()) {
        barGraphs[currentBar].min = 0.0;
      } else {
        validDoubleFromString(datapart, barGraphs[currentBar].min);
      }
      datapart = parseString(graphpart, 4); // direction

      if (datapart.isEmpty()) {
        barGraphs[currentBar].direction = 0;
      } else {
        int32_t value = 0;
        validIntFromString(datapart, value);
        barGraphs[currentBar].direction = value;
      }
      datapart = parseString(graphpart, 5); // barType

      if (datapart.isEmpty()) {
        barGraphs[currentBar].barType = 0;
      } else {
        int32_t value = 0;
        validIntFromString(datapart, value);
        barGraphs[currentBar].barType = value;
      }

      if (definitelyGreaterThan(barGraphs[currentBar].min, barGraphs[currentBar].max)) {
        std::swap(barGraphs[currentBar].min, barGraphs[currentBar].max);
      }
    }
    #  ifdef P104_DEBUG

    if (logAllText && loglevelActiveFor(LOG_LEVEL_INFO)) {
      String log;

      if (log.reserve(70)) {
        log = F("dotmatrix: Bar-graph: ");

        if (loop) {
          log += currentBar;
          log += F(" in: ");
          log += graphpart;
          log += F(" value: ");
          log += barGraphs[currentBar].value;
          log += F(" max: ");
          log += barGraphs[currentBar].max;
          log += F(" min: ");
          log += barGraphs[currentBar].min;
          log += F(" dir: ");
          log += barGraphs[currentBar].direction;
          log += F(" typ: ");
          log += barGraphs[currentBar].barType;
        } else {
          log += F(" bsize: ");
          log += barGraphs.size();
        }
        addLogMove(LOG_LEVEL_INFO, log);
      }
    }
    #  endif // ifdef P104_DEBUG
    currentBar++; // next
    delay(0);     // Leave some breathingroom
  }
  #  undef NOT_A_COMMA

  if (barGraphs.size() > 0) {
    uint8_t  barWidth = 8 / barGraphs.size(); // Divide the 8 pixel width per number of bars to show
    int16_t  pixTop, pixBottom;
    uint16_t zeroPoint;
    #  ifdef P104_DEBUG
    String log;

    if (logAllText &&
        loglevelActiveFor(LOG_LEVEL_INFO) &&
        log.reserve(64)) {
      log  = F("dotmatrix: bar Width: ");
      log += barWidth;
      log += F(" low: ");
      log += zstruct._lower;
      log += F(" high: ");
      log += zstruct._upper;
    }
    #  endif // ifdef P104_DEBUG
    modulesOnOff(zstruct._startModule, zstruct._startModule + zstruct.size - 1, MD_MAX72XX::MD_OFF); // Stop updates on modules
    P->setIntensity(zstruct.zone - 1, zstruct.brightness);                                           // don't forget to set the brightness
    uint8_t row = 0;

    if ((barGraphs.size() == 3) || (barGraphs.size() == 5) || (barGraphs.size() == 6)) {             // Center within the rows a bit
      for (; row < (barGraphs.size() == 5 ? 2 : 1); row++) {
        for (uint8_t col = zstruct._lower; col <= zstruct._upper; col++) {
          pM->setPoint(row, col, false);                                                             // all off

          if (col % 16 == 0) { delay(0); }
        }
        delay(0); // Leave some breathingroom
      }
    }

    for (auto it = barGraphs.begin(); it != barGraphs.end(); ++it) {
      if (essentiallyZero(it->min)) {
        pixTop    = zstruct._lower - 1 + (((zstruct._upper + 1) - zstruct._lower) / it->max) * it->value;
        pixBottom = zstruct._lower - 1;
        zeroPoint = 0;
      } else {
        if (definitelyLessThan(it->min, 0.0) &&
            definitelyGreaterThan(it->max,           0.0) &&
            definitelyGreaterThan(it->max - it->min, 0.01)) { // Zero-point is used
          zeroPoint = (it->min * -1.0) / ((it->max - it->min) / (1.0 * ((zstruct._upper + 1) - zstruct._lower)));
        } else {
          zeroPoint = 0;
        }
        pixTop    = zstruct._lower + zeroPoint + (((zstruct._upper + 1) - zstruct._lower) / (it->max - it->min)) * it->value;
        pixBottom = zstruct._lower + zeroPoint;

        if (definitelyLessThan(it->value, 0.0)) {
          std::swap(pixTop, pixBottom);
        }
      }

      if (it->direction == 1) { // Left to right display: Flip values within the lower/upper range
        pixBottom = zstruct._upper - (pixBottom - zstruct._lower);
        pixTop    = zstruct._lower + (zstruct._upper - pixTop);
        std::swap(pixBottom, pixTop);
        zeroPoint = zstruct._upper - zstruct._lower - zeroPoint + (zeroPoint == 0 ? 1 : 0);
      }
      #  ifdef P104_DEBUG_DEV

      if (logAllText && loglevelActiveFor(LOG_LEVEL_INFO)) {
        log += F(" B: ");
        log += pixBottom;
        log += F(" T: ");
        log += pixTop;
        log += F(" Z: ");
        log += zeroPoint;
      }
      #  endif // ifdef P104_DEBUG_DEV
      drawOneBarGraph(zstruct._lower, zstruct._upper, pixBottom, pixTop, zeroPoint, barWidth, it->barType, row);
      row += barWidth;                 // Next set of rows
      delay(0);                        // Leave some breathingroom
    }

    for (; row < 8; row++) {           // Clear unused rows
      for (uint8_t col = zstruct._lower; col <= zstruct._upper; col++) {
        pM->setPoint(row, col, false); // all off

        if (col % 16 == 0) { delay(0); }
      }
      delay(0); // Leave some breathingroom
    }
    #  ifdef P104_DEBUG

    if (logAllText && loglevelActiveFor(LOG_LEVEL_INFO)) {
      addLogMove(LOG_LEVEL_INFO, log);
    }
    #  endif // ifdef P104_DEBUG
    modulesOnOff(zstruct._startModule, zstruct._startModule + zstruct.size - 1, MD_MAX72XX::MD_ON); // Continue updates on modules
  }
}

# endif // ifdef P104_USE_BAR_GRAPH

# ifdef P104_USE_DOT_SET
void P104_data_struct::displayDots(uint8_t                 zone,
                                   const P104_zone_struct& zstruct,
                                   const String          & dots) {
  if ((nullptr == P) || (nullptr == pM) || dots.isEmpty()) { return; }
  {
    uint8_t idx = 0;
    String  sRow;
    String  sCol;
    String  sOn_off;
    bool    on_off = true;
    modulesOnOff(zstruct._startModule, zstruct._startModule + zstruct.size - 1, MD_MAX72XX::MD_OFF); // Stop updates on modules
    P->setIntensity(zstruct.zone - 1, zstruct.brightness);                                           // don't forget to set the brightness
    sRow    = parseString(dots, idx + 1);
    sCol    = parseString(dots, idx + 2);
    sOn_off = parseString(dots, idx + 3);

    while (!sRow.isEmpty() && !sCol.isEmpty()) {
      on_off = true; // Default On

      int32_t row;
      int32_t col;

      if (validIntFromString(sRow, row) &&
          validIntFromString(sCol, col) &&
          (row > 0) && ((row - 1) < 8) &&
          (col > 0) && ((col - 1) <= (zstruct._upper - zstruct._lower))) { // Valid coordinates?
        if (equals(sOn_off, F("0"))) {                                     // Dot On is the default
          on_off = false;
          idx++;                                                           // 3rd argument used
        }
        pM->setPoint(row - 1, zstruct._upper - (col - 1), on_off);         // Reverse layout
      }
      idx += 2;                                                            // Skip to next argument set

      if (idx % 16 == 0) { delay(0); }
      sRow    = parseString(dots, idx + 1);
      sCol    = parseString(dots, idx + 2);
      sOn_off = parseString(dots, idx + 3);
    }

    modulesOnOff(zstruct._startModule, zstruct._startModule + zstruct.size - 1, MD_MAX72XX::MD_ON); // Continue updates on modules
  }
}

# endif // ifdef P104_USE_DOT_SET

/**************************************************
 * Check if an animation is available in the current build
 *************************************************/
bool isAnimationAvailable(uint8_t animation, bool noneIsAllowed = false) {
  textEffect_t selection = static_cast<textEffect_t>(animation);

  switch (selection) {
    case PA_NO_EFFECT:
    {
      return noneIsAllowed;
    }
    case PA_PRINT:
    case PA_SCROLL_UP:
    case PA_SCROLL_DOWN:
    case PA_SCROLL_LEFT:
    case PA_SCROLL_RIGHT:
    {
      return true;
    }
    # if ENA_SPRITE
    case PA_SPRITE:
    {
      return true;
    }
    # endif // ENA_SPRITE
    # if ENA_MISC
    case PA_SLICE:
    case PA_MESH:
    case PA_FADE:
    case PA_DISSOLVE:
    case PA_BLINDS:
    case PA_RANDOM:
    {
      return true;
    }
    # endif // ENA_MISC
    # if ENA_WIPE
    case PA_WIPE:
    case PA_WIPE_CURSOR:
    {
      return true;
    }
    # endif // ENA_WIPE
    # if ENA_SCAN
    case PA_SCAN_HORIZ:
    case PA_SCAN_HORIZX:
    case PA_SCAN_VERT:
    case PA_SCAN_VERTX:
    {
      return true;
    }
    # endif // ENA_SCAN
    # if ENA_OPNCLS
    case PA_OPENING:
    case PA_OPENING_CURSOR:
    case PA_CLOSING:
    case PA_CLOSING_CURSOR:
    {
      return true;
    }
    # endif // ENA_OPNCLS
    # if ENA_SCR_DIA
    case PA_SCROLL_UP_LEFT:
    case PA_SCROLL_UP_RIGHT:
    case PA_SCROLL_DOWN_LEFT:
    case PA_SCROLL_DOWN_RIGHT:
    {
      return true;
    }
    # endif // ENA_SCR_DIA
    # if ENA_GROW
    case PA_GROW_UP:
    case PA_GROW_DOWN:
    {
      return true;
    }
    # endif // ENA_GROW
    default:
      return false;
  }
}

const char p104_subcommands[] PROGMEM =
  "clear"
  "|update"

  "|txt"
  "|settxt"

# ifdef P104_USE_BAR_GRAPH
  "|bar"
  "|setbar"
# endif // ifdef P104_USE_BAR_GRAPH

# ifdef P104_USE_DOT_SET
  "|dot"
# endif // ifdef P104_USE_DOT_SET

# ifdef P104_USE_COMMANDS
  "|alignment"
  "|anim.in"
  "|anim.out"
  "|brightness"
  "|content"
  "|font"
  "|inverted"
#  if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
  "|layout"
#  endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
  "|offset"
  "|pause"
  "|repeat"
  "|size"
  "|specialeffect"
  "|speed"
# endif // ifdef P104_USE_COMMANDS
;

// Subcommands prefixed by "dotmatrix,"
enum class p104_subcommands_e {
  clear,  // subcommand: clear,<zone> / clear[,all]
  update, // subcommand: update,<zone> / update[,all]

  txt,    // subcommand: [set]txt,<zone>,<text> (only
  settxt, // subcommand: settxt,<zone>,<text> (stores

# ifdef P104_USE_BAR_GRAPH
  bar,    // subcommand: [set]bar,<zone>,<graph-string> (only allowed for zones
  setbar, // subcommand: setbar,<zone>,<graph-string> (stores the graph-string
# endif // ifdef P104_USE_BAR_GRAPH

# ifdef P104_USE_DOT_SET
  dot, // subcommand: dot,<zone>,<r>,<c>[,0][,<r>,<c>[,0]...] to draw
# endif // ifdef P104_USE_DOT_SET

# ifdef P104_USE_COMMANDS
  alignment,     // subcommand: alignment,<zone>,<alignment> (0..3)
  anim_in,       // subcommand: anim.in,<zone>,<animation> (1..)
  anim_out,      // subcommand: anim.out,<zone>,<animation> (0..)
  brightness,    // subcommand: brightness,<zone>,<brightness> (0..15)
  content,       // subcommand: content,<zone>,<contenttype> (0..<P104_CONTENT_count>-1)
  font,          // subcommand: font,<zone>,<font id> (only for incuded font id's)
  inverted,      // subcommand: inverted,<zone>,<invertedstate> (disable/enable)
#  if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
  layout,        // subcommand: layout,<zone>,<layout> (0..2), only when double-height font is available
#  endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
  offset,        // subcommand: offset,<zone>,<size> (0..<size>-1)
  pause,         // subcommand: pause,<zone>,<pause_ms> (0..P104_MAX_SPEED_PAUSE_VALUE)
  repeat,        // subcommand: repeat,<zone>,<repeat_sec> (-1..86400 = 24h)
  size,          // subcommand: size,<zone>,<size> (1..)
  specialeffect, // subcommand: specialeffect,<zone>,<effect> (0..3)
  speed,         // subcommand: speed,<zone>,<speed_ms> (0..P104_MAX_SPEED_PAUSE_VALUE)
# endif // ifdef P104_USE_COMMANDS
};

/*******************************************************
 * handlePluginWrite : process commands
 ******************************************************/
bool P104_data_struct::handlePluginWrite(taskIndex_t   taskIndex,
                                         const String& string) {
  # ifdef P104_USE_COMMANDS
  bool reconfigure = false;
  # endif // ifdef P104_USE_COMMANDS
  bool success         = false;
  const String command = parseString(string, 1);

  if ((nullptr != P) && equals(command, F("dotmatrix"))) { // main command: dotmatrix
    const String subCommand   = parseString(string, 2);
    const int    subCommand_i = GetCommandCode(subCommand.c_str(), p104_subcommands);

    if (subCommand_i != -1) {
      const p104_subcommands_e subcommands_e = static_cast<p104_subcommands_e>(subCommand_i);

      int32_t zoneIndex{};
      const String string4 = parseStringKeepCaseNoTrim(string, 4);
    # ifdef P104_USE_COMMANDS
      int32_t value4{};
      validIntFromString(string4, value4);
    # endif // ifdef P104_USE_COMMANDS

      // Global subcommands

      if ((subcommands_e == p104_subcommands_e::clear) && // subcommand: clear[,all]
          (string4.isEmpty() ||
           string4.equalsIgnoreCase(F("all")))) {
        P->displayClear();
        success = true;
      } else

      if ((subcommands_e == p104_subcommands_e::update) && // subcommand: update[,all]
          (string4.isEmpty() ||
           string4.equalsIgnoreCase(F("all")))) {
        updateZone(0, P104_zone_struct(0));
        success = true;
      }

      // Zone-specific subcommands
      if (validIntFromString(parseString(string, 3), zoneIndex) &&
          (zoneIndex > 0) &&
          (static_cast<size_t>(zoneIndex) <= zones.size())) {
        // subcommands are processed in the same order as they are presented in the UI
        for (auto it = zones.begin(); it != zones.end() && !success; ++it) {
          if ((it->zone == zoneIndex)) { // This zone
            switch (subcommands_e) {
              case p104_subcommands_e::clear:
                // subcommand: clear,<zone>
              {
                P->displayClear(zoneIndex - 1);
                success = true;
                break;
              }

              case p104_subcommands_e::update:
                // subcommand: update,<zone>
              {
                updateZone(zoneIndex, *it);
                success = true;
                break;
              }

          # ifdef P104_USE_COMMANDS

              case p104_subcommands_e::size:
                // subcommand: size,<zone>,<size> (1..)
              {
                if ((value4 > 0) &&
                    (value4 <= P104_MAX_MODULES_PER_ZONE))
                {
                  reconfigure = (it->size != value4);
                  it->size    = value4;
                  success     = true;
                }
                break;
              }
          # endif // ifdef P104_USE_COMMANDS

              case p104_subcommands_e::txt:                                  // subcommand: [set]txt,<zone>,<text> (only
              case p104_subcommands_e::settxt:                               // allowed for zones with Text content)
              {
                if ((it->content == P104_CONTENT_TEXT) ||
                    (it->content == P104_CONTENT_TEXT_REV)) {                // no length check, so longer than the UI allows is made
                                                                             // possible
                  if ((subcommands_e == p104_subcommands_e::settxt) &&       // subcommand: settxt,<zone>,<text> (stores
                      (string4.length() <= P104_MAX_TEXT_LENGTH_PER_ZONE)) { // the text in the settings, is not saved)
                    it->text = string4;                                      // Only if not too long, could 'blow up' the
                  }                                                          // settings when saved
                  displayOneZoneText(zoneIndex - 1, *it, string4);
                  success = true;
                }

                break;
              }

          # ifdef P104_USE_COMMANDS

              case p104_subcommands_e::content:
                // subcommand: content,<zone>,<contenttype> (0..<P104_CONTENT_count>-1)
              {
                if ((value4 >= 0) &&
                    (value4 < P104_CONTENT_count))
                {
                  reconfigure = (it->content != value4);
                  it->content = value4;
                  success     = true;
                }
                break;
              }

              case p104_subcommands_e::alignment:
                // subcommand: alignment,<zone>,<alignment> (0..3)
              {
                if ((value4 >= 0) &&
                    (value4 <= static_cast<int>(textPosition_t::PA_RIGHT))) // last item in the enum
                {
                  it->alignment = value4;
                  success       = true;
                }
                break;
              }

              case p104_subcommands_e::anim_in:
                // subcommand: anim.in,<zone>,<animation> (1..)
              {
                if (isAnimationAvailable(value4)) {
                  it->animationIn = value4;
                  success         = true;
                }
                break;
              }

              case p104_subcommands_e::speed:
                // subcommand: speed,<zone>,<speed_ms> (0..P104_MAX_SPEED_PAUSE_VALUE)
              {
                if ((value4 >= 0) &&
                    (value4 <= P104_MAX_SPEED_PAUSE_VALUE))
                {
                  it->speed = value4;
                  success   = true;
                }
                break;
              }

              case p104_subcommands_e::anim_out:
                // subcommand: anim.out,<zone>,<animation> (0..)
              {
                if (isAnimationAvailable(value4, true))
                {
                  it->animationOut = value4;
                  success          = true;
                }
                break;
              }

              case p104_subcommands_e::pause:
                // subcommand: pause,<zone>,<pause_ms> (0..P104_MAX_SPEED_PAUSE_VALUE)
              {
                if ((value4 >= 0) &&
                    (value4 <= P104_MAX_SPEED_PAUSE_VALUE))
                {
                  it->pause = value4;
                  success   = true;
                }
                break;
              }

              case p104_subcommands_e::font:
                // subcommand: font,<zone>,<font id> (only for incuded font id's)
              {
                if (
                  (value4 == 0)
                #  ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
                  || (value4 == P104_DOUBLE_HEIGHT_FONT_ID)
                #  endif // ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
                #  ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
                  || (value4 == P104_FULL_DOUBLEHEIGHT_FONT_ID)
                #  endif // ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
                #  ifdef P104_USE_VERTICAL_FONT
                  || (value4 == P104_VERTICAL_FONT_ID)
                #  endif // ifdef P104_USE_VERTICAL_FONT
                #  ifdef P104_USE_EXT_ASCII_FONT
                  || (value4 == P104_EXT_ASCII_FONT_ID)
                #  endif // ifdef P104_USE_EXT_ASCII_FONT
                #  ifdef P104_USE_ARABIC_FONT
                  || (value4 == P104_ARABIC_FONT_ID)
                #  endif // ifdef P104_USE_ARABIC_FONT
                #  ifdef P104_USE_GREEK_FONT
                  || (value4 == P104_GREEK_FONT_ID)
                #  endif // ifdef P104_USE_GREEK_FONT
                #  ifdef P104_USE_KATAKANA_FONT
                  || (value4 == P104_KATAKANA_FONT_ID)
                #  endif // ifdef P104_USE_KATAKANA_FONT
                  )
                {
                  reconfigure = (it->font != value4);
                  it->font    = value4;
                  success     = true;
                }
                break;
              }

              case p104_subcommands_e::inverted:
                // subcommand: inverted,<zone>,<invertedstate> (disable/enable)
              {
                if ((value4 >= 0) &&
                    (value4 <= 1))
                {
                  reconfigure  = (it->inverted != value4);
                  it->inverted = value4;
                  success      = true;
                }
                break;
              }

          #  if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)

              case p104_subcommands_e::layout:
                // subcommand: layout,<zone>,<layout> (0..2), only when double-height font is available
              {
                if ((value4 >= 0) &&
                    (value4 <= P104_LAYOUT_DOUBLE_LOWER))
                {
                  reconfigure = (it->layout != value4);
                  it->layout  = value4;
                  success     = true;
                }
                break;
              }
          #  endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)

              case p104_subcommands_e::specialeffect:
                // subcommand: specialeffect,<zone>,<effect> (0..3)
              {
                if ((value4 >= 0) &&
                    (value4 <= P104_SPECIAL_EFFECT_BOTH))
                {
                  reconfigure       = (it->specialEffect != value4);
                  it->specialEffect = value4;
                  success           = true;
                }
                break;
              }

              case p104_subcommands_e::offset:
                // subcommand: offset,<zone>,<size> (0..<size>-1)
              {
                if ((value4 >= 0) &&
                    (value4 < P104_MAX_MODULES_PER_ZONE) &&
                    (value4 < it->size))
                {
                  reconfigure = (it->offset != value4);
                  it->offset  = value4;
                  success     = true;
                }
                break;
              }

              case p104_subcommands_e::brightness:
                // subcommand: brightness,<zone>,<brightness> (0..15)
              {
                if ((value4 >= 0) &&
                    (value4 <= P104_BRIGHTNESS_MAX))
                {
                  it->brightness = value4;
                  P->setIntensity(zoneIndex - 1, it->brightness); // Change brightness immediately
                  success = true;
                }
                break;
              }

              case p104_subcommands_e::repeat:
                // subcommand: repeat,<zone>,<repeat_sec> (-1..86400 = 24h)
              {
                if ((value4 >= -1) &&
                    (value4 <= P104_MAX_REPEATDELAY_VALUE))
                {
                  it->repeatDelay = value4;
                  success         = true;

                  if (it->repeatDelay > -1) {
                    it->_repeatTimer = millis();
                  }
                }
                break;
              }
          # endif // ifdef P104_USE_COMMANDS

          # ifdef P104_USE_BAR_GRAPH

              case p104_subcommands_e::bar:                                  // subcommand: [set]bar,<zone>,<graph-string> (only allowed for
              // zones
              case p104_subcommands_e::setbar:                               // with Bargraph content) no length check, so longer than the
                                                                             // UI allows is made possible
              {
                if (it->content == P104_CONTENT_BAR_GRAPH) {
                  if ((subcommands_e == p104_subcommands_e::setbar) &&       // subcommand: setbar,<zone>,<graph-string> (stores the
                                                                             // graph-string
                      (string4.length() <= P104_MAX_TEXT_LENGTH_PER_ZONE)) { // in the settings, is not saved)
                    it->text = string4;                                      // Only if not too long, could 'blow up' the settings when
                                                                             // saved
                  }
                  displayBarGraph(zoneIndex - 1, *it, string4);
                  success = true;
                }
                break;
              }
          # endif // ifdef P104_USE_BAR_GRAPH

          # ifdef P104_USE_DOT_SET

              case p104_subcommands_e::dot:
                // subcommand: dot,<zone>,<r>,<c>[,0][,<r>,<c>[,0]...] to draw
              {
                displayDots(zoneIndex - 1, *it, parseStringToEnd(string, 4)); // dots at row/column, add ,0 to turn a dot off
                success = true;
                break;
              }
          # endif // ifdef P104_USE_DOT_SET
            }

            // FIXME TD-er: success is always false here. Maybe this must be done outside the for-loop?
            if (success) { // Reset the repeat timer
              if (it->repeatDelay > -1) {
                it->_repeatTimer = millis();
              }
            }
          }
        }
      }
    }
  }

  # ifdef P104_USE_COMMANDS

  if (reconfigure) {
    configureZones(); // Re-initialize
    success = true;   // Successful
  }
  # endif // ifdef P104_USE_COMMANDS

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    String log;

    if (log.reserve(34 + string.length())) {
      log = F("dotmatrix: command ");

      if (!success) { log += F("NOT "); }
      log += F("succesful: ");
      log += string;
      addLogMove(LOG_LEVEL_INFO, log);
    }
  }

  return success; // Default: unknown command
}

int8_t P104_data_struct::getTime(char *psz,
                                 bool  seconds,
                                 bool  colon,
                                 bool  time12h,
                                 bool  timeAmpm) {
  uint16_t h, M, s;
  String   ampm;

  # ifdef P104_USE_DATETIME_OPTIONS

  if (time12h) {
    if (timeAmpm) {
      ampm = (node_time.hour() >= 12 ? F("p") : F("a"));
    }
    h = node_time.hour() % 12;

    if (h == 0) { h = 12; }
  } else
  # endif // ifdef P104_USE_DATETIME_OPTIONS
  {
    h = node_time.hour();
  }
  M = node_time.minute();

  if (!seconds) {
    sprintf_P(psz, PSTR("%02d%c%02d%s"), h, (colon ? ':' : ' '), M, ampm.c_str());
  } else {
    s = node_time.second();
    sprintf_P(psz, PSTR("%02d%c%02d %02d%s"), h, (colon ? ':' : ' '), M, s, ampm.c_str());
  }
  return M;
}

void P104_data_struct::getDate(char           *psz,
                               bool            showYear,
                               bool            fourDgt
                               # ifdef         P104_USE_DATETIME_OPTIONS
                               , const uint8_t dateFmt
                               , const uint8_t dateSep
                               # endif // ifdef P104_USE_DATETIME_OPTIONS
                               ) {
  uint16_t d, m, y;
  const uint16_t year = node_time.year() - (fourDgt ? 0 : 2000);

  # ifdef P104_USE_DATETIME_OPTIONS
  const String separators = F(" /-.");
  const char   sep        = separators[dateSep];
  # else // ifdef P104_USE_DATETIME_OPTIONS
  const char sep = ' ';
  # endif // ifdef P104_USE_DATETIME_OPTIONS

  d = node_time.day();
  m = node_time.month();
  y = year;
  # ifdef P104_USE_DATETIME_OPTIONS

  if (showYear) {
    switch (dateFmt) {
      case P104_DATE_FORMAT_US:
        d = node_time.month();
        m = node_time.day();
        y = year;
        break;
      case P104_DATE_FORMAT_JP:
        d = year;
        m = node_time.month();
        y = node_time.day();
        break;
    }
  } else {
    if ((dateFmt == P104_DATE_FORMAT_US) ||
        (dateFmt == P104_DATE_FORMAT_JP)) {
      std::swap(d, m);
    }
  }
  # endif // ifdef P104_USE_DATETIME_OPTIONS

  if (showYear) {
    sprintf_P(psz, PSTR("%02d%c%02d%c%02d"), d, sep, m, sep, y); // %02d will expand to 04 when needed
  } else {
    sprintf_P(psz, PSTR("%02d%c%02d"), d, sep, m);
  }
}

uint8_t P104_data_struct::getDateTime(char           *psz,
                                      bool            colon,
                                      bool            time12h,
                                      bool            timeAmpm,
                                      bool            fourDgt
                                      # ifdef         P104_USE_DATETIME_OPTIONS
                                      , const uint8_t dateFmt
                                      , const uint8_t dateSep
                                      # endif // ifdef P104_USE_DATETIME_OPTIONS
                                      ) {
  String   ampm;
  uint16_t d, M, y;
  uint8_t  h, m;
  const uint16_t year = node_time.year() - (fourDgt ? 0 : 2000);

  # ifdef P104_USE_DATETIME_OPTIONS
  const String separators = F(" /-.");
  const char   sep        = separators[dateSep];
  # else // ifdef P104_USE_DATETIME_OPTIONS
  const char sep = ' ';
  # endif // ifdef P104_USE_DATETIME_OPTIONS

  # ifdef P104_USE_DATETIME_OPTIONS

  if (time12h) {
    if (timeAmpm) {
      ampm = (node_time.hour() >= 12 ? F("p") : F("a"));
    }
    h = node_time.hour() % 12;

    if (h == 0) { h = 12; }
  } else
  # endif // ifdef P104_USE_DATETIME_OPTIONS
  {
    h = node_time.hour();
  }
  M = node_time.minute();

  # ifdef P104_USE_DATETIME_OPTIONS

  switch (dateFmt) {
    case P104_DATE_FORMAT_US:
      d = node_time.month();
      m = node_time.day();
      y = year;
      break;
    case P104_DATE_FORMAT_JP:
      d = year;
      m = node_time.month();
      y = node_time.day();
      break;
    default:
  # endif // ifdef P104_USE_DATETIME_OPTIONS
  d = node_time.day();
  m = node_time.month();
  y = year;
  # ifdef P104_USE_DATETIME_OPTIONS
}

  # endif // ifdef P104_USE_DATETIME_OPTIONS
  sprintf_P(psz, PSTR("%02d%c%02d%c%02d %02d%c%02d%s"), d, sep, m, sep, y, h, (colon ? ':' : ' '), M, ampm.c_str()); // %02d will expand to
                                                                                                                     // 04 when needed
  return M;
}

# if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
void P104_data_struct::createHString(String& string) {
  const uint16_t stringLen = string.length();

  for (uint16_t i = 0; i < stringLen; i++) {
    string[i] |= 0x80; // use 'high' part of the font, by adding 0x80
  }
}

# endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)

void P104_data_struct::reverseStr(String& str) {
  const uint16_t n = str.length();

  // Swap characters starting from two corners
  for (uint16_t i = 0; i < n / 2; i++) {
    std::swap(str[i], str[n - i - 1]);
  }
}

/************************************************************************
 * execute all PLUGIN_ONE_PER_SECOND tasks
 ***********************************************************************/
bool P104_data_struct::handlePluginOncePerSecond(struct EventStruct *event) {
  if (nullptr == P) { return false; }
  bool redisplay = false;
  bool success   = false;

  # ifdef P104_USE_DATETIME_OPTIONS
  bool useFlasher = !bitRead(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_FLASH);
  bool time12h    = bitRead(P104_CONFIG_DATETIME,  P104_CONFIG_DATETIME_12H);
  bool timeAmpm   = bitRead(P104_CONFIG_DATETIME,  P104_CONFIG_DATETIME_AMPM);
  bool year4dgt   = bitRead(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_YEAR4DGT);
  # else // ifdef P104_USE_DATETIME_OPTIONS
  bool useFlasher = true;
  bool time12h    = false;
  bool timeAmpm   = false;
  bool year4dgt   = false;
  # endif // ifdef P104_USE_DATETIME_OPTIONS
  bool newFlasher = !flasher && useFlasher;

  for (auto it = zones.begin(); it != zones.end(); ++it) {
    redisplay = false;

    if (P->getZoneStatus(it->zone - 1)) { // Animations done?
      switch (it->content) {
        case P104_CONTENT_TIME:           // time
        case P104_CONTENT_TIME_SEC:       // time sec
        {
          bool   useSeconds = (it->content == P104_CONTENT_TIME_SEC);
          int8_t m          = getTime(szTimeL, useSeconds, flasher || !useFlasher, time12h, timeAmpm);
          flasher          = newFlasher;
          redisplay        = useFlasher || useSeconds || (it->_lastChecked != m);
          it->_lastChecked = m;
          break;
        }
        case P104_CONTENT_DATE4: // date/4
        case P104_CONTENT_DATE6: // date/6
        {
          if (it->_lastChecked != node_time.day()) {
            getDate(szTimeL,
                    it->content != P104_CONTENT_DATE4,
                    year4dgt
                    # ifdef P104_USE_DATETIME_OPTIONS
                    , get4BitFromUL(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_FORMAT)
                    , get4BitFromUL(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_SEP_CHAR)
                    # endif // ifdef P104_USE_DATETIME_OPTIONS
                    );
            redisplay        = true;
            it->_lastChecked = node_time.day();
          }
          break;
        }
        case P104_CONTENT_DATE_TIME: // date-time/9
        {
          int8_t m = getDateTime(szTimeL,
                                 flasher || !useFlasher,
                                 time12h,
                                 timeAmpm,
                                 year4dgt
                                 # ifdef P104_USE_DATETIME_OPTIONS
                                 , get4BitFromUL(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_FORMAT)
                                 , get4BitFromUL(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_SEP_CHAR)
                                 # endif // ifdef P104_USE_DATETIME_OPTIONS
                                 );
          flasher          = newFlasher;
          redisplay        = useFlasher || (it->_lastChecked != m);
          it->_lastChecked = m;
          break;
        }
        default:
          break;
      }

      if (redisplay) {
        displayOneZoneText(it->zone - 1, *it, String(szTimeL));
        P->displayReset(it->zone - 1);

        if (it->repeatDelay > -1) {
          it->_repeatTimer = millis();
        }
      }
    }
    delay(0); // Leave some breathingroom
  }

  if (redisplay) {
    // synchronise the start
    P->synchZoneStart();
  }
  return redisplay || success;
}

/***************************************************
 * restart a zone if the repeat delay (if any) has passed
 **************************************************/
void P104_data_struct::checkRepeatTimer(uint8_t z) {
  if (nullptr == P) { return; }
  bool handled = false;

  for (auto it = zones.begin(); it != zones.end() && !handled; ++it) {
    if (it->zone == z + 1) {
      handled = true;

      if ((it->repeatDelay > -1) && (timePassedSince(it->_repeatTimer) >= (it->repeatDelay - 1) * 1000)) { // Compensated for the '1' in
                                                                                                           // PLUGIN_ONE_PER_SECOND
        # ifdef P104_DEBUG

        if (logAllText && loglevelActiveFor(LOG_LEVEL_INFO)) {
          String log;
          log.reserve(51);
          log  = F("dotmatrix: Repeat zone: ");
          log += it->zone;
          log += F(" delay: ");
          log += it->repeatDelay;
          log += F(" (");
          log += (timePassedSince(it->_repeatTimer) / 1000.0f); // Decimals can be useful here
          log += ')';
          addLogMove(LOG_LEVEL_INFO, log);
        }
        # endif // ifdef P104_DEBUG

        if ((it->content == P104_CONTENT_TEXT) ||
            (it->content == P104_CONTENT_TEXT_REV)) {
          displayOneZoneText(it->zone - 1, *it, sZoneInitial[it->zone - 1]); // Re-send last displayed text
          P->displayReset(it->zone - 1);
        }

        if ((it->content == P104_CONTENT_TIME) ||
            (it->content == P104_CONTENT_TIME_SEC) ||
            (it->content == P104_CONTENT_DATE4) ||
            (it->content == P104_CONTENT_DATE6) ||
            (it->content == P104_CONTENT_DATE_TIME)) {
          it->_lastChecked = -1; // Invalidate so next run will re-display the date/time
        }
        # ifdef P104_USE_BAR_GRAPH

        if (it->content == P104_CONTENT_BAR_GRAPH) {
          displayBarGraph(it->zone - 1, *it, sZoneInitial[it->zone - 1]); // Re-send last displayed bar graph
        }
        # endif // ifdef P104_USE_BAR_GRAPH
        it->_repeatTimer = millis();
      }
    }
    delay(0); // Leave some breathingroom
  }
}

/***************************************
 * saveSettings gather the zones data from the UI and store in customsettings
 **************************************/
bool P104_data_struct::saveSettings() {
  error = String(); // Clear

  # ifdef P104_DEBUG_DEV

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    addLogMove(LOG_LEVEL_INFO, concat(F("P104: saving zones, count: "), expectedZones));
  }
  # endif // ifdef P104_DEBUG_DEV

  uint8_t index      = 0;
  uint8_t action     = P104_ACTION_NONE;
  uint8_t zoneIndex  = 0;
  int8_t  zoneOffset = 0;

  zones.clear(); // Start afresh

  for (uint8_t zCounter = 0; zCounter < expectedZones; zCounter++) {
    # ifdef P104_USE_ZONE_ACTIONS
    action = getFormItemIntCustomArgName(index + P104_OFFSET_ACTION);

    if (((action == P104_ACTION_ADD_ABOVE) && (zoneOrder == 0)) ||
        ((action == P104_ACTION_ADD_BELOW) && (zoneOrder == 1))) {
      zones.push_back(P104_zone_struct(0));
      zoneOffset++;
      #  ifdef P104_DEBUG_DEV

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        addLogMove(LOG_LEVEL_INFO, concat(F("P104: insert before zone: "), zoneIndex + 1));
      }
      #  endif // ifdef P104_DEBUG_DEV
    }
    # endif    // ifdef P104_USE_ZONE_ACTIONS
    zoneIndex = zCounter + zoneOffset;

    if (action == P104_ACTION_DELETE) {
      zoneOffset--;
    } else {
      # ifdef P104_DEBUG_DEV

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        addLogMove(LOG_LEVEL_INFO, concat(F("P104: read zone: "), zoneIndex + 1));
      }
      # endif // ifdef P104_DEBUG_DEV
      zones.push_back(P104_zone_struct(zoneIndex + 1));

      for (uint8_t i = 0; i < P104_OFFSET_COUNT; ++i) {
        // for newly added zone, use defaults
        const bool mustCheckSize =
          (i == P104_OFFSET_BRIGHTNESS) ||
          (i == P104_OFFSET_REPEATDELAY);

        if (!mustCheckSize || (zones[zoneIndex].size != 0)) {
          if (i == P104_OFFSET_TEXT) {
            zones[zoneIndex].text = wrapWithQuotes(webArg(getPluginCustomArgName(index + P104_OFFSET_TEXT)));
          } else {
            zones[zoneIndex].setIntValue(i, getFormItemIntCustomArgName(index + i));
          }
        }
      }
    }
    # ifdef P104_DEBUG_DEV

    if (loglevelActiveFor(LOG_LEVEL_INFO)) {
      addLogMove(LOG_LEVEL_INFO, concat(F("P104: add zone: "), zoneIndex + 1));
    }
    # endif // ifdef P104_DEBUG_DEV

    # ifdef P104_USE_ZONE_ACTIONS

    if (((action == P104_ACTION_ADD_BELOW) && (zoneOrder == 0)) ||
        ((action == P104_ACTION_ADD_ABOVE) && (zoneOrder == 1))) {
      zones.push_back(P104_zone_struct(0));
      zoneOffset++;
      #  ifdef P104_DEBUG_DEV

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        addLogMove(LOG_LEVEL_INFO, concat(F("P104: insert after zone: "), zoneIndex + 2));
      }
      #  endif // ifdef P104_DEBUG_DEV
    }
    # endif    // ifdef P104_USE_ZONE_ACTIONS

    index += P104_OFFSET_COUNT;
    delay(0);
  }

  uint16_t bufferSize;
  int saveOffset = 0;

  numDevices = 0;                      // Count the number of connected display units

  # if P104_FEATURE_STORAGE_V3
  bufferSize = P104_CONFIG_VERSION_V3; // Save special marker that we're using V3 (extended) settings
  # else // if P104_FEATURE_STORAGE_V3
  bufferSize = P104_CONFIG_VERSION_V2; // Save special marker that we're using V2 settings
  # endif // if P104_FEATURE_STORAGE_V3

  // This write is counting
  error += SaveToFile(SettingsType::Enum::CustomTaskSettings_Type, taskIndex, (uint8_t *)&bufferSize, sizeof(bufferSize), saveOffset);
  # if P104_FEATURE_STORAGE_V3
  saveOffset = DAT_TASKS_CUSTOM_SIZE; // Start in the extension file
  # else // if P104_FEATURE_STORAGE_V3
  saveOffset += sizeof(bufferSize);
  # endif // if P104_FEATURE_STORAGE_V3

  String zbuffer;

  // 47 total + (max) 100 characters for it->text requires a buffer of ~150 (P104_SETTINGS_BUFFER_V2), but only the required length is
  // stored with the length prefixed
  if (zbuffer.reserve(P104_SETTINGS_BUFFER_V2 + 2)) {
    for (auto it = zones.begin(); it != zones.end() && error.length() == 0; ++it) {
      // WARNING: Order of values should match the numeric order of P104_OFFSET_* values
      zbuffer.clear();

      for (uint8_t i = 0; i < P104_OFFSET_COUNT; ++i) {
        if (i == P104_OFFSET_TEXT) {
          zbuffer += it->text;
          zbuffer += '\x01';
        } else {
          int32_t value{};

          if (it->getIntValue(i, value)) {
            zbuffer += value;
            zbuffer += '\x01';
          }
        }
      }

      numDevices += (it->size != 0 ? it->size : 1) + it->offset; // Count corrected for newly added zones

      ZERO_FILL(P104_storeThis);                                 // Clean previous data

      if (saveOffset + zbuffer.length() + (sizeof(P104_dataSize) * 2) >
          (
            # if !P104_FEATURE_STORAGE_V3       // Don't count the skipped storage
            DAT_TASKS_CUSTOM_SIZE +
            # endif // if !P104_FEATURE_STORAGE_V3
            DAT_TASKS_CUSTOM_EXTENSION_SIZE)) { // Detect ourselves if we've reached the
        error.reserve(55);                      // high-water mark
        error += F("Total combination of Zones & text too long to store.\n");
        addLogMove(LOG_LEVEL_ERROR, error);
      } else {
        // Store length of buffer
        P104_dataSize = zbuffer.length();
        safe_strncpy(P104_data, zbuffer.c_str(), P104_dataSize + 1);

        // As we write in parts, only count as single write.
        if (RTC.flashDayCounter > 0) {
          RTC.flashDayCounter--;
        }
        error += SaveToFile(SettingsType::Enum::CustomTaskSettings_Type,
                            taskIndex,
                            (uint8_t *)P104_storeThis,
                            P104_dataSize + sizeof(P104_dataSize),
                            saveOffset);
        saveOffset += P104_dataSize + sizeof(P104_dataSize);

        # ifdef P104_DEBUG_DEV

        if (loglevelActiveFor(LOG_LEVEL_INFO)) {
          addLogMove(LOG_LEVEL_INFO, strformat(F("P104: saveSettings zone: %d bufferSize: %d offset: %d"),
                                               it->zone, bufferSize, saveOffset));
          zbuffer.replace(P104_FIELD_SEP, P104_FIELD_DISP);
          addLog(LOG_LEVEL_INFO, zbuffer);
        }
        # endif // ifdef P104_DEBUG_DEV
      }

      delay(0);
    }

    // Store an End-of-settings marker == 0
    bufferSize = 0u;

    // This write is counting
    SaveToFile(SettingsType::Enum::CustomTaskSettings_Type, taskIndex, (uint8_t *)&bufferSize, sizeof(bufferSize), saveOffset);

    if (numDevices > 255) {
      error += strformat(F("More than 255 modules configured (%u)\n"), numDevices);
    }
  } else {
    addLog(LOG_LEVEL_ERROR, F("DOTMATRIX: Can't allocate string for saving settings, insufficient memory!"));
    return false; // Don't continue
  }

  return error.isEmpty();
}

/**************************************************************
* webform_load
**************************************************************/
bool P104_data_struct::webform_load(struct EventStruct *event) {
  {                                       // Hardware types
    # define P104_hardwareTypeCount 8
    const __FlashStringHelper *hardwareTypes[P104_hardwareTypeCount] = {
      F("Generic (DR:0, CR:1, RR:0)"),    // 010
      F("Parola (DR:1, CR:1, RR:0)"),     // 110
      F("FC16 (DR:1, CR:0, RR:0)"),       // 100
      F("IC Station (DR:1, CR:1, RR:1)"), // 111
      F("Other 1 (DR:0, CR:0, RR:0)"),    // 000
      F("Other 2 (DR:0, CR:0, RR:1)"),    // 001
      F("Other 3 (DR:0, CR:1, RR:1)"),    // 011
      F("Other 4 (DR:1, CR:0, RR:1)")     // 101
    };
    constexpr int hardwareOptions[P104_hardwareTypeCount] = {
      static_cast<int>(MD_MAX72XX::moduleType_t::GENERIC_HW),
      static_cast<int>(MD_MAX72XX::moduleType_t::PAROLA_HW),
      static_cast<int>(MD_MAX72XX::moduleType_t::FC16_HW),
      static_cast<int>(MD_MAX72XX::moduleType_t::ICSTATION_HW),
      static_cast<int>(MD_MAX72XX::moduleType_t::DR0CR0RR0_HW),
      static_cast<int>(MD_MAX72XX::moduleType_t::DR0CR0RR1_HW),
      static_cast<int>(MD_MAX72XX::moduleType_t::DR0CR1RR1_HW),
      static_cast<int>(MD_MAX72XX::moduleType_t::DR1CR0RR1_HW)
    };
    addFormSelector(F("Hardware type"),
                    F("hardware"),
                    P104_hardwareTypeCount,
                    hardwareTypes,
                    hardwareOptions,
                    P104_CONFIG_HARDWARETYPE);
    # ifdef P104_ADD_SETTINGS_NOTES
    addFormNote(F("DR = Digits as Rows, CR = Column Reversed, RR = Row Reversed; 0 = no, 1 = yes."));
    # endif // ifdef P104_ADD_SETTINGS_NOTES
  }

  {
    addFormCheckBox(F("Clear display on disable"), F("clrdsp"),
                    bitRead(P104_CONFIG_FLAGS, P104_CONFIG_FLAG_CLEAR_DISABLE));

    addFormCheckBox(F("Log all displayed text (info)"),
                    F("logtxt"),
                    bitRead(P104_CONFIG_FLAGS, P104_CONFIG_FLAG_LOG_ALL_TEXT));
  }

  # ifdef P104_USE_DATETIME_OPTIONS
  {
    addFormSubHeader(F("Content options"));

    addFormCheckBox(F("Clock with flashing colon"), F("clkflash"), !bitRead(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_FLASH));
    addFormCheckBox(F("Clock 12h display"),         F("clk12h"),   bitRead(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_12H));
    addFormCheckBox(F("Clock 12h AM/PM indicator"), F("clkampm"),  bitRead(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_AMPM));
  }
  { // Date format
    const __FlashStringHelper *dateFormats[] = {
      F("Day Month [Year]"),
      F("Month Day [Year] (US-style)"),
      F("[Year] Month Day (Japanese-style)")
    };
    constexpr int dateFormatOptions[] = {
      P104_DATE_FORMAT_EU,
      P104_DATE_FORMAT_US,
      P104_DATE_FORMAT_JP
    };
    addFormSelector(F("Date format"), F("datefmt"),
                    3,
                    dateFormats, dateFormatOptions,
                    get4BitFromUL(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_FORMAT));
  }
  { // Date separator
    const __FlashStringHelper *dateSeparators[] = {
      F("Space"),
      F("Slash /"),
      F("Dash -"),
      F("Dot <b>.</b>")
    };
    constexpr int dateSeparatorOptions[] = {
      P104_DATE_SEPARATOR_SPACE,
      P104_DATE_SEPARATOR_SLASH,
      P104_DATE_SEPARATOR_DASH,
      P104_DATE_SEPARATOR_DOT
    };
    addFormSelector(F("Date separator"), F("datesep"),
                    4,
                    dateSeparators, dateSeparatorOptions,
                    get4BitFromUL(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_SEP_CHAR));

    addFormCheckBox(F("Year uses 4 digits"), F("year4dgt"), bitRead(P104_CONFIG_DATETIME, P104_CONFIG_DATETIME_YEAR4DGT));
  }
  # endif // ifdef P104_USE_DATETIME_OPTIONS

  addFormSubHeader(F("Zones"));

  { // Zones
    String zonesList[P104_MAX_ZONES];
    int    zonesOptions[P104_MAX_ZONES];

    for (uint8_t i = 0; i < P104_MAX_ZONES; i++) {
      zonesList[i]    = i + 1;
      zonesOptions[i] = i + 1; // No 0 needed or wanted
    }
    # if defined(P104_USE_TOOLTIPS) || defined(P104_ADD_SETTINGS_NOTES)

    const String zonetip = F("Select between 1 and " STRINGIFY(P104_MAX_ZONES) " zones, changing"
      #  ifdef P104_USE_ZONE_ORDERING
                             " Zones or Zone order"
      #  endif // ifdef P104_USE_ZONE_ORDERING
                             " will save and reload the page.");
    # endif    // if defined(P104_USE_TOOLTIPS) || defined(P104_ADD_SETTINGS_NOTES)
    addFormSelector(F("Zones"), F("zonecnt"), P104_MAX_ZONES, zonesList, zonesOptions, nullptr, P104_CONFIG_ZONE_COUNT, true
                    # ifdef P104_USE_TOOLTIPS
                    , zonetip
                    # endif // ifdef P104_USE_TOOLTIPS
                    );

    # ifdef P104_USE_ZONE_ORDERING
    const String orderTypes[] = {
      F("Numeric order (1..n)"),
      F("Display order (n..1)")
    };
    const int    orderOptions[] = { 0, 1 };
    addFormSelector(F("Zone order"), F("zoneorder"), 2, orderTypes, orderOptions, nullptr,
                    bitRead(P104_CONFIG_FLAGS, P104_CONFIG_FLAG_ZONE_ORDER) ? 1 : 0, true
                    #  ifdef P104_USE_TOOLTIPS
                    , zonetip
                    #  endif // ifdef P104_USE_TOOLTIPS
                    );
    # endif                  // ifdef P104_USE_ZONE_ORDERING
    # ifdef P104_ADD_SETTINGS_NOTES
    addFormNote(zonetip);
    # endif // ifdef P104_ADD_SETTINGS_NOTES
  }
  expectedZones = P104_CONFIG_ZONE_COUNT;

  if (expectedZones == 0) { expectedZones++; } // Minimum of 1 zone

  { // Optionlists and zones table
    const __FlashStringHelper *alignmentTypes[3] = {
      F("Left"),
      F("Center"),
      F("Right")
    };
    const int alignmentOptions[3] = {
      static_cast<int>(textPosition_t::PA_LEFT),
      static_cast<int>(textPosition_t::PA_CENTER),
      static_cast<int>(textPosition_t::PA_RIGHT)
    };


    // Append the numeric value as a reference for the 'anim.in' and 'anim.out' subcommands
    const __FlashStringHelper *animationTypes[] {
      F("None (0)")
      , F("Print (1)")
      , F("Scroll up (2)")
      , F("Scroll down (3)")
      , F("Scroll left * (4)")
      , F("Scroll right * (5)")
    # if ENA_SPRITE
      , F("Sprite (6)")
    # endif // ENA_SPRITE
    # if ENA_MISC
      , F("Slice * (7)")
      , F("Mesh (8)")
      , F("Fade (9)")
      , F("Dissolve (10)")
      , F("Blinds (11)")
      , F("Random (12)")
    # endif // ENA_MISC
    # if ENA_WIPE
      , F("Wipe (13)")
      , F("Wipe w. cursor (14)")
    # endif // ENA_WIPE
    # if ENA_SCAN
      , F("Scan horiz. (15)")
      , F("Scan horiz. cursor (16)")
      , F("Scan vert. (17)")
      , F("Scan vert. cursor (18)")
    # endif // ENA_SCAN
    # if ENA_OPNCLS
      , F("Opening (19)")
      , F("Opening w. cursor (20)")
      , F("Closing (21)")
      , F("Closing w. cursor (22)")
    # endif // ENA_OPNCLS
    # if ENA_SCR_DIA
      , F("Scroll up left * (23)")
      , F("Scroll up right * (24)")
      , F("Scroll down left * (25)")
      , F("Scroll down right * (26)")
    # endif // ENA_SCR_DIA
    # if ENA_GROW
      , F("Grow up (27)")
      , F("Grow down (28)")
    # endif // ENA_GROW
    };

    const int animationOptions[] = {
      static_cast<int>(textEffect_t::PA_NO_EFFECT)
      , static_cast<int>(textEffect_t::PA_PRINT)
      , static_cast<int>(textEffect_t::PA_SCROLL_UP)
      , static_cast<int>(textEffect_t::PA_SCROLL_DOWN)
      , static_cast<int>(textEffect_t::PA_SCROLL_LEFT)
      , static_cast<int>(textEffect_t::PA_SCROLL_RIGHT)
    # if ENA_SPRITE
      , static_cast<int>(textEffect_t::PA_SPRITE)
    # endif // ENA_SPRITE
    # if ENA_MISC
      , static_cast<int>(textEffect_t::PA_SLICE)
      , static_cast<int>(textEffect_t::PA_MESH)
      , static_cast<int>(textEffect_t::PA_FADE)
      , static_cast<int>(textEffect_t::PA_DISSOLVE)
      , static_cast<int>(textEffect_t::PA_BLINDS)
      , static_cast<int>(textEffect_t::PA_RANDOM)
    # endif // ENA_MISC
    # if ENA_WIPE
      , static_cast<int>(textEffect_t::PA_WIPE)
      , static_cast<int>(textEffect_t::PA_WIPE_CURSOR)
    # endif // ENA_WIPE
    # if ENA_SCAN
      , static_cast<int>(textEffect_t::PA_SCAN_HORIZ)
      , static_cast<int>(textEffect_t::PA_SCAN_HORIZX)
      , static_cast<int>(textEffect_t::PA_SCAN_VERT)
      , static_cast<int>(textEffect_t::PA_SCAN_VERTX)
    # endif // ENA_SCAN
    # if ENA_OPNCLS
      , static_cast<int>(textEffect_t::PA_OPENING)
      , static_cast<int>(textEffect_t::PA_OPENING_CURSOR)
      , static_cast<int>(textEffect_t::PA_CLOSING)
      , static_cast<int>(textEffect_t::PA_CLOSING_CURSOR)
    # endif // ENA_OPNCLS
    # if ENA_SCR_DIA
      , static_cast<int>(textEffect_t::PA_SCROLL_UP_LEFT)
      , static_cast<int>(textEffect_t::PA_SCROLL_UP_RIGHT)
      , static_cast<int>(textEffect_t::PA_SCROLL_DOWN_LEFT)
      , static_cast<int>(textEffect_t::PA_SCROLL_DOWN_RIGHT)
    # endif // ENA_SCR_DIA
    # if ENA_GROW
      , static_cast<int>(textEffect_t::PA_GROW_UP)
      , static_cast<int>(textEffect_t::PA_GROW_DOWN)
    # endif // ENA_GROW
    };

    constexpr int animationCount = NR_ELEMENTS(animationOptions);

    delay(0);

    const __FlashStringHelper *fontTypes[] = {
      F("Default (0)")
    # ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
      , F("Numeric, double height (1)")
    # endif // ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
    # ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
      , F("Full, double height (2)")
    # endif // ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
    # ifdef P104_USE_VERTICAL_FONT
      , F("Vertical (3)")
    # endif // ifdef P104_USE_VERTICAL_FONT
    # ifdef P104_USE_EXT_ASCII_FONT
      , F("Extended ASCII (4)")
      # endif // ifdef P104_USE_EXT_ASCII_FONT
    # ifdef P104_USE_ARABIC_FONT
      , F("Arabic (5)")
    # endif // ifdef P104_USE_ARABIC_FONT
    # ifdef P104_USE_GREEK_FONT
      , F("Greek (6)")
    # endif // ifdef P104_USE_GREEK_FONT
    # ifdef P104_USE_KATAKANA_FONT
      , F("Katakana (7)")
    # endif // ifdef P104_USE_KATAKANA_FONT
    };
    const int fontOptions[] = {
      P104_DEFAULT_FONT_ID
    # ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
      , P104_DOUBLE_HEIGHT_FONT_ID
    # endif // ifdef P104_USE_NUMERIC_DOUBLEHEIGHT_FONT
    # ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
      , P104_FULL_DOUBLEHEIGHT_FONT_ID
    # endif // ifdef P104_USE_FULL_DOUBLEHEIGHT_FONT
    # ifdef P104_USE_VERTICAL_FONT
      , P104_VERTICAL_FONT_ID
    # endif // ifdef P104_USE_VERTICAL_FONT
    # ifdef P104_USE_EXT_ASCII_FONT
      , P104_EXT_ASCII_FONT_ID
      # endif // ifdef P104_USE_EXT_ASCII_FONT
    # ifdef P104_USE_ARABIC_FONT
      , P104_ARABIC_FONT_ID
    # endif // ifdef P104_USE_ARABIC_FONT
    # ifdef P104_USE_GREEK_FONT
      , P104_GREEK_FONT_ID
    # endif // ifdef P104_USE_GREEK_FONT
    # ifdef P104_USE_KATAKANA_FONT
      , P104_KATAKANA_FONT_ID
    # endif // ifdef P104_USE_KATAKANA_FONT
    };
    constexpr int fontCount = NR_ELEMENTS(fontTypes);

    const __FlashStringHelper *layoutTypes[] = {
      F("Standard")
    # if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
      , F("Double, upper")
      , F("Double, lower")
    # endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
    };
    const int layoutOptions[] = {
      P104_LAYOUT_STANDARD
    # if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
      , P104_LAYOUT_DOUBLE_UPPER
      , P104_LAYOUT_DOUBLE_LOWER
    # endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) || defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
    };
    constexpr int layoutCount = NR_ELEMENTS(layoutTypes);

    const __FlashStringHelper *specialEffectTypes[] = {
      F("None"),
      F("Flip up/down"),
      F("Flip left/right *"),
      F("Flip u/d &amp; l/r *")
    };
    const int specialEffectOptions[] = {
      P104_SPECIAL_EFFECT_NONE,
      P104_SPECIAL_EFFECT_UP_DOWN,
      P104_SPECIAL_EFFECT_LEFT_RIGHT,
      P104_SPECIAL_EFFECT_BOTH
    };
    constexpr int specialEffectCount = NR_ELEMENTS(specialEffectTypes);

    const __FlashStringHelper *contentTypes[] = {
      F("Text"),
      F("Text reverse"),
      F("Clock (4 mod)"),
      F("Clock sec (6 mod)"),
      F("Date (4 mod)"),
      F("Date yr (6/7 mod)"),
      F("Date/time (9/13 mod)"),
      # ifdef P104_USE_BAR_GRAPH
      F("Bar graph"),
      # endif // ifdef P104_USE_BAR_GRAPH
    };
    const int contentOptions[] {
      P104_CONTENT_TEXT,
      P104_CONTENT_TEXT_REV,
      P104_CONTENT_TIME,
      P104_CONTENT_TIME_SEC,
      P104_CONTENT_DATE4,
      P104_CONTENT_DATE6,
      P104_CONTENT_DATE_TIME,
      # ifdef P104_USE_BAR_GRAPH
      P104_CONTENT_BAR_GRAPH,
      # endif // ifdef P104_USE_BAR_GRAPH
    };
    const __FlashStringHelper *invertedTypes[3] = {
      F("Normal"),
      F("Inverted")
    };
    const int invertedOptions[] = {
      0,
      1
    };
    # ifdef P104_USE_ZONE_ACTIONS
    uint8_t actionCount = 0;
    const __FlashStringHelper *actionTypes[4];
    int actionOptions[4];
    actionTypes[actionCount]   = F("None");
    actionOptions[actionCount] = P104_ACTION_NONE;
    actionCount++;

    if (zones.size() < P104_MAX_ZONES) {
      actionTypes[actionCount]   = F("New above");
      actionOptions[actionCount] = P104_ACTION_ADD_ABOVE;
      actionCount++;
      actionTypes[actionCount]   = F("New below");
      actionOptions[actionCount] = P104_ACTION_ADD_BELOW;
      actionCount++;
    }
    actionTypes[actionCount]   = F("Delete");
    actionOptions[actionCount] = P104_ACTION_DELETE;
    actionCount++;
    # endif // ifdef P104_USE_ZONE_ACTIONS

    delay(0);

    addFormSubHeader(F("Zone configuration"));

    {
      html_table(EMPTY_STRING); // Sub-table

      const __FlashStringHelper *headers[] = {
        F("Zone #&nbsp;"),
        F("Modules"),
        F("Text"),
        F("Content"),
        F("Alignment"),
        F("Animation In/Out"),               // 1st and 2nd row title
        F("Speed/Pause"),                    // 1st and 2nd row title
        F("Font/Layout"),                    // 1st and 2nd row title
        F("Inverted/ Special&nbsp;Effects"), // 1st and 2nd row title
        F("Offset"),
        F("Brightness"),
        F("Repeat (sec)")
      };

      constexpr unsigned nrHeaders = NR_ELEMENTS(headers);

      for (unsigned i = 0; i < nrHeaders; ++i) {
        int width = 0;

        if (i == 2) {
          // "Text" needs a width
          width = 180;
        }
        html_table_header(headers[i], width);
      }
      # ifdef P104_USE_ZONE_ACTIONS
      html_table_header(F(""),       15); // Spacer
      html_table_header(F("Action"), 45);
      # endif // ifdef P104_USE_ZONE_ACTIONS
    }

    uint16_t index;
    int16_t  startZone, endZone;
    int8_t   incrZone = 1;
    # ifdef P104_USE_ZONE_ACTIONS
    uint8_t currentRow = 0;
    # endif // ifdef P104_USE_ZONE_ACTIONS

    # ifdef P104_USE_ZONE_ORDERING

    if (bitRead(P104_CONFIG_FLAGS, P104_CONFIG_FLAG_ZONE_ORDER)) {
      startZone = zones.size() - 1;
      endZone   = -1;
      incrZone  = -1;
    } else
    # endif // ifdef P104_USE_ZONE_ORDERING
    {
      startZone = 0;
      endZone   = zones.size();
    }

    for (int8_t zone = startZone; zone != endZone; zone += incrZone) {
      if (zones[zone].zone <= expectedZones) {
        index = (zones[zone].zone - 1) * P104_OFFSET_COUNT;

        html_TR_TD(); // All columns use max. width available
        addHtml(F("&nbsp;"));
        addHtmlInt(zones[zone].zone);

        html_TD(); // Modules
        addNumericBox(getPluginCustomArgName(index + P104_OFFSET_SIZE), zones[zone].size, 1, P104_MAX_MODULES_PER_ZONE);

        html_TD(); // Text
        addTextBox(getPluginCustomArgName(index + P104_OFFSET_TEXT),
                   zones[zone].text,
                   P104_MAX_TEXT_LENGTH_PER_ZONE,
                   false,
                   false,
                   EMPTY_STRING,
                   F(""));

        html_TD(); // Content
        addSelector(getPluginCustomArgName(index + P104_OFFSET_CONTENT),
                    P104_CONTENT_count,
                    contentTypes,
                    contentOptions,
                    nullptr,
                    zones[zone].content,
                    false,
                    true,
                    F(""));

        html_TD(); // Alignment
        addSelector(getPluginCustomArgName(index + P104_OFFSET_ALIGNMENT),
                    3,
                    alignmentTypes,
                    alignmentOptions,
                    nullptr,
                    zones[zone].alignment,
                    false,
                    true,
                    F(""));

        {
          html_TD(); // Animation In (without None by passing the second element index)
          addSelector(getPluginCustomArgName(index + P104_OFFSET_ANIM_IN),
                      animationCount - 1,
                      &animationTypes[1],
                      &animationOptions[1],
                      nullptr,
                      zones[zone].animationIn,
                      false,
                      true,
                      F("")
                      # ifdef P104_USE_TOOLTIPS
                      , F("Animation In")
                      # endif // ifdef P104_USE_TOOLTIPS
                      );
        }

        html_TD();                 // Speed In
        addNumericBox(getPluginCustomArgName(index + P104_OFFSET_SPEED), zones[zone].speed, 0, P104_MAX_SPEED_PAUSE_VALUE
                      # ifdef P104_USE_TOOLTIPS
                      , F("")      // classname
                      , F("Speed") // title
                      # endif // ifdef P104_USE_TOOLTIPS
                      );

        html_TD(); // Font
        addSelector(getPluginCustomArgName(index + P104_OFFSET_FONT),
                    NR_ELEMENTS(fontOptions),
                    fontTypes,
                    fontOptions,
                    nullptr,
                    zones[zone].font,
                    false,
                    true,
                    F("")
                    # ifdef P104_USE_TOOLTIPS
                    , F("Font") // title
                    # endif // ifdef P104_USE_TOOLTIPS
                    );

        html_TD(); // Inverted
        addSelector(getPluginCustomArgName(index + P104_OFFSET_INVERTED),
                    NR_ELEMENTS(invertedOptions),
                    invertedTypes,
                    invertedOptions,
                    nullptr,
                    zones[zone].inverted,
                    false,
                    true,
                    F("")
                    # ifdef P104_USE_TOOLTIPS
                    , F("Inverted") // title
                    # endif // ifdef P104_USE_TOOLTIPS
                    );

        html_TD(3); // Fill columns
        # ifdef P104_USE_ZONE_ACTIONS

        html_TD();  // Spacer
        addHtml('|');

        if (currentRow < 2) {
          addHtml(F("<TD style=\"text-align:center;font-size:90%\">")); // Action column, text centered and font-size 90%
        } else {
          html_TD();
        }

        if (currentRow == 0) {
          addHtml(F("(applied immediately!)"));
        } else if (currentRow == 1) {
          addHtml(F("(Delete can't be undone!)"));
        }
        currentRow++;
        # endif // ifdef P104_USE_ZONE_ACTIONS

        // Split here
        html_TR_TD(); // Start new row
        html_TD(4);   // Start with some blank columns

        {
          html_TD();  // Animation Out
          addSelector(getPluginCustomArgName(index + P104_OFFSET_ANIM_OUT),
                      animationCount,
                      animationTypes,
                      animationOptions,
                      nullptr,
                      zones[zone].animationOut,
                      false,
                      true,
                      F("")
                      # ifdef P104_USE_TOOLTIPS
                      , F("Animation Out")
                      # endif // ifdef P104_USE_TOOLTIPS
                      );
        }

        html_TD();                 // Pause after Animation In
        addNumericBox(getPluginCustomArgName(index + P104_OFFSET_PAUSE), zones[zone].pause, 0, P104_MAX_SPEED_PAUSE_VALUE
                      # ifdef P104_USE_TOOLTIPS
                      , F("")      // classname
                      , F("Pause") // title
                      # endif // ifdef P104_USE_TOOLTIPS
                      );

        html_TD(); // Layout
        addSelector(getPluginCustomArgName(index + P104_OFFSET_LAYOUT),
                    NR_ELEMENTS(layoutOptions),
                    layoutTypes,
                    layoutOptions,
                    nullptr,
                    zones[zone].layout,
                    false,
                    true,
                    F("")
                    # ifdef P104_USE_TOOLTIPS
                    , F("Layout") // title
                    # endif // ifdef P104_USE_TOOLTIPS
                    );

        html_TD(); // Special effects
        addSelector(getPluginCustomArgName(index + P104_OFFSET_SPEC_EFFECT),
                    NR_ELEMENTS(specialEffectOptions),
                    specialEffectTypes,
                    specialEffectOptions,
                    nullptr,
                    zones[zone].specialEffect,
                    false,
                    true,
                    F("")
                    # ifdef P104_USE_TOOLTIPS
                    , F("Special Effects") // title
                    # endif // ifdef P104_USE_TOOLTIPS
                    );

        html_TD(); // Offset
        addNumericBox(getPluginCustomArgName(index + P104_OFFSET_OFFSET), zones[zone].offset, 0, 254);

        html_TD(); // Brightness

        if (zones[zone].brightness == -1) { zones[zone].brightness = P104_BRIGHTNESS_DEFAULT; }
        addNumericBox(getPluginCustomArgName(index + P104_OFFSET_BRIGHTNESS), zones[zone].brightness, 0, P104_BRIGHTNESS_MAX);

        html_TD(); // Repeat (sec)
        addNumericBox(getPluginCustomArgName(index + P104_OFFSET_REPEATDELAY),
                      zones[zone].repeatDelay,
                      -1,
                      P104_MAX_REPEATDELAY_VALUE                     // max delay 86400 sec. = 24 hours
                      # ifdef P104_USE_TOOLTIPS
                      , F("")                                        // classname
                      , F("Repeat after this delay (sec), -1 = off") // tooltip
                      # endif // ifdef P104_USE_TOOLTIPS
                      );

        # ifdef P104_USE_ZONE_ACTIONS
        html_TD(); // Spacer
        addHtml('|');

        html_TD(); // Action
        addSelector(getPluginCustomArgName(index + P104_OFFSET_ACTION),
                    actionCount,
                    actionTypes,
                    actionOptions,
                    nullptr,
                    P104_ACTION_NONE, // Always start with None
                    true,
                    true,
                    F(""));
        # endif // ifdef P104_USE_ZONE_ACTIONS

        delay(0);
      }
    }
    html_end_table();
  }

  # ifdef P104_ADD_SETTINGS_NOTES
  addFormNote(concat(F("- Maximum nr. of modules possible (Zones * Size + Offset) = 255. Last saved: "), numDevices));
  addFormNote(F("- 'Animation In' or 'Animation Out' and 'Special Effects' marked with <b>*</b> should <b>not</b> be combined in a Zone."));
  #  if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) && !defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
  addFormNote(F("- 'Layout' 'Double upper' and 'Double lower' are only supported for numeric 'Content' types like 'Clock' and 'Date'."));
  #  endif // if defined(P104_USE_NUMERIC_DOUBLEHEIGHT_FONT) && !defined(P104_USE_FULL_DOUBLEHEIGHT_FONT)
  # endif    // ifdef P104_ADD_SETTINGS_NOTES

  return true;
}

/**************************************************************
* webform_save
**************************************************************/
bool P104_data_struct::webform_save(struct EventStruct *event) {
  P104_CONFIG_ZONE_COUNT   = getFormItemInt(F("zonecnt"));
  P104_CONFIG_HARDWARETYPE = getFormItemInt(F("hardware"));

  bitWrite(P104_CONFIG_FLAGS, P104_CONFIG_FLAG_CLEAR_DISABLE, isFormItemChecked(F("clrdsp")));
  bitWrite(P104_CONFIG_FLAGS, P104_CONFIG_FLAG_LOG_ALL_TEXT,  isFormItemChecked(F("logtxt")));

  # ifdef P104_USE_ZONE_ORDERING
  zoneOrder = getFormItemInt(F("zoneorder")); // Is used in saveSettings()
  bitWrite(P104_CONFIG_FLAGS, P104_CONFIG_FLAG_ZONE_ORDER, zoneOrder == 1);
  # endif // ifdef P104_USE_ZONE_ORDERING

  # ifdef P104_USE_DATETIME_OPTIONS
  uint32_t ulDateTime = 0;
  bitWrite(ulDateTime, P104_CONFIG_DATETIME_FLASH,    !isFormItemChecked(F("clkflash"))); // Inverted flag
  bitWrite(ulDateTime, P104_CONFIG_DATETIME_12H,      isFormItemChecked(F("clk12h")));
  bitWrite(ulDateTime, P104_CONFIG_DATETIME_AMPM,     isFormItemChecked(F("clkampm")));
  bitWrite(ulDateTime, P104_CONFIG_DATETIME_YEAR4DGT, isFormItemChecked(F("year4dgt")));
  set4BitToUL(ulDateTime, P104_CONFIG_DATETIME_FORMAT,   getFormItemInt(F("datefmt")));
  set4BitToUL(ulDateTime, P104_CONFIG_DATETIME_SEP_CHAR, getFormItemInt(F("datesep")));
  P104_CONFIG_DATETIME = ulDateTime;
  # endif // ifdef P104_USE_DATETIME_OPTIONS

  previousZones = expectedZones;
  expectedZones = P104_CONFIG_ZONE_COUNT;

  bool result = saveSettings();         // Determines numDevices and re-fills zones list

  P104_CONFIG_ZONE_COUNT  = zones.size();
  P104_CONFIG_TOTAL_UNITS = numDevices; // Store counted number of devices

  zones.clear();                        // Free some memory (temporarily)

  return result;
}

P104_zone_struct::P104_zone_struct(uint8_t _zone)
  :  text(F("\"\"")), zone(_zone) {}


bool P104_zone_struct::getIntValue(uint8_t offset, int32_t& value) const
{
  switch (offset) {
    case P104_OFFSET_SIZE:          value = size;           break;
    case P104_OFFSET_TEXT:          return false;
    case P104_OFFSET_CONTENT:       value = content;        break;
    case P104_OFFSET_ALIGNMENT:     value = alignment;      break;
    case P104_OFFSET_ANIM_IN:       value = animationIn;    break;
    case P104_OFFSET_SPEED:         value = speed;          break;
    case P104_OFFSET_ANIM_OUT:      value = animationOut;   break;
    case P104_OFFSET_PAUSE:         value = pause;          break;
    case P104_OFFSET_FONT:          value = font;           break;
    case P104_OFFSET_LAYOUT:        value = layout;         break;
    case P104_OFFSET_SPEC_EFFECT:   value = specialEffect;  break;
    case P104_OFFSET_OFFSET:        value = offset;         break;
    case P104_OFFSET_BRIGHTNESS:    value = brightness;     break;
    case P104_OFFSET_REPEATDELAY:   value = repeatDelay;    break;
    case P104_OFFSET_INVERTED:      value = inverted;       break;

    default:
      return false;
  }
  return true;
}

bool P104_zone_struct::setIntValue(uint8_t offset, int32_t value)
{
  switch (offset) {
    case P104_OFFSET_SIZE:          size = value; break;
    case P104_OFFSET_TEXT:          return false;
    case P104_OFFSET_CONTENT:       content       = value; break;
    case P104_OFFSET_ALIGNMENT:     alignment     = value; break;
    case P104_OFFSET_ANIM_IN:       animationIn   = value; break;
    case P104_OFFSET_SPEED:         speed         = value; break;
    case P104_OFFSET_ANIM_OUT:      animationOut  = value; break;
    case P104_OFFSET_PAUSE:         pause         = value; break;
    case P104_OFFSET_FONT:          font          = value; break;
    case P104_OFFSET_LAYOUT:        layout        = value; break;
    case P104_OFFSET_SPEC_EFFECT:   specialEffect = value; break;
    case P104_OFFSET_OFFSET:        offset        = value; break;
    case P104_OFFSET_BRIGHTNESS:    brightness    = value; break;
    case P104_OFFSET_REPEATDELAY:   repeatDelay   = value; break;
    case P104_OFFSET_INVERTED:      inverted      = value; break;

    default:
      return false;
  }
  return true;
}

#endif // ifdef USES_P104

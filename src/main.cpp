#include <Arduino.h>
#include <M5Cardputer.h>
#include <stdint.h>
#include <SD.h>
#include <string.h>
#include <ctype.h>
#include <SPI.h>
#include <Preferences.h>

// ===================== config.h =====================

// ============================================================================
// M5Stack Cardputer-Adv Hardware Pinout (Stamp-S3A / ESP32-S3FN8)
// Verified against M5Unified / Cardputer-Adv v1.0 schematic
// ============================================================================

// microSD Card (SPI2 / FSPI)
constexpr int PIN_SD_SCK  = 40;
constexpr int PIN_SD_MISO = 39;
constexpr int PIN_SD_MOSI = 14;
constexpr int PIN_SD_CS   = 12;
constexpr uint32_t SD_SPI_FREQ = 16000000; // 25 MHz fast SPI read

// ES8311 Audio Codec (Internal I2C bus 0, managed by M5.In_I2C)
constexpr uint8_t ES8311_I2C_ADDR = 0x18;
constexpr int PIN_I2C_SDA = 8;
constexpr int PIN_I2C_SCL = 9;

// I2S Output to ES8311 DAC (MCLK is derived internally from BCLK)
constexpr int PIN_I2S_BCLK = 41;
constexpr int PIN_I2S_LRCK = 43;
constexpr int PIN_I2S_DOUT = 42;

// ============================================================================
// Memory & Performance Tuning (512 KB Internal SRAM, NO PSRAM)
// Zero-allocation static pools prevent heap fragmentation during AAC/M4A decode
// ============================================================================

// Maximum entries in currently active folder (static arena, ~24 KB total)
constexpr uint16_t MAX_DIR_ENTRIES = 4096;
constexpr uint16_t NAME_POOL_BYTES = 12288; // 20 KB contiguous char arena
constexpr uint16_t MAX_PATH_LEN    = 256;

// FreeRTOS Task Pinning & Priorities
constexpr BaseType_t AUDIO_TASK_CORE     = 0;     // Dedicated audio decoding core
constexpr UBaseType_t AUDIO_TASK_PRIO    = 5;     // High priority (never starves DMA)
constexpr uint32_t   AUDIO_TASK_STACK    = 16384;  // 8 KB stack for Helix AAC/MP3/M4A
constexpr BaseType_t UI_TASK_CORE        = 1;     // Arduino loop() runs on Core 1

// UI Rendering & Input Tuning
constexpr uint16_t SCREEN_W = 240;
constexpr uint16_t SCREEN_H = 135;
constexpr uint32_t UI_FRAME_MS_ACTIVE   = 33;     // ~30 FPS when visualizer is ON
constexpr uint32_t UI_FRAME_MS_IDLE     = 100;    // ~10 FPS when visualizer is OFF (saves power!)
constexpr uint32_t KEY_REPEAT_DELAY_MS  = 380;    // Hold delay before auto-repeat
constexpr uint32_t KEY_REPEAT_RATE_MS   = 95;     // Auto-repeat cadence

// Volume Range (ESP32-audioI2S uses 0..21)
constexpr uint8_t VOL_MIN     = 0;
constexpr uint8_t VOL_MAX     = 21;
constexpr uint8_t VOL_DEFAULT = 12;

// Pocket Mode (Button G0) multi-click timing
constexpr uint32_t POCKET_CLICK_WINDOW_MS = 380;

// ===================== hal_es8311.h =====================

// ============================================================================
// Dedicated ES8311 DAC Driver for M5Stack Cardputer-Adv
//
// Why this exists:
// M5Cardputer.Speaker uses M5Unified's internal I2S mixer (which doesn't
// decode M4A/AAC). When we hand I2S over to ESP32-audioI2S for native
// MP3/M4A/AAC/WAV/FLAC playback, we release M5.Speaker's I2S driver and
// directly configure the ES8311 codec over M5.In_I2C (addr 0x18) so that:
//   1. MCLK is sourced internally from BCLK (Cardputer-Adv has no MCLK pin).
//   2. DAC analog path + NS4150B speaker / 3.5mm jack auto-switch are active.
//   3. Zero clicks/pops on boot and clean hardware DAC gain.
// ============================================================================

namespace HalES8311 {

static inline bool writeReg(uint8_t reg, uint8_t val) {
    return M5.In_I2C.writeRegister8(ES8311_I2C_ADDR, reg, val, 400000);
}

static inline uint8_t readReg(uint8_t reg) {
    return M5.In_I2C.readRegister8(ES8311_I2C_ADDR, reg, 400000);
}

// Probe ES8311 on internal I2C bus (chip ID reg 0xFD == 0x83)
inline bool isPresent() {
    uint8_t id1 = readReg(0xFD);
    uint8_t id2 = readReg(0xFE);
    return (id1 == 0x83 && id2 == 0x11);
}

// Initialize ES8311 in Slave I2S DAC mode with MCLK derived from BCLK
inline bool initDAC(uint8_t hwVolume = 0xBF) {
    // 1. Let M5Unified first wake up internal I2C & power rails if needed,
    //    then release M5Unified's I2S peripheral so ESP32-audioI2S owns it cleanly.
    if (M5Cardputer.Speaker.isEnabled()) {
        M5Cardputer.Speaker.end();
        
    }   
    if (!isPresent()) return false;
    
    // 2. Soft reset ES8311 state machine
    writeReg(0x00, 0x1F);
    delay(5);
    writeReg(0x00, 0x80); // Power on, CSM active

    // 3. Clock Manager: MCLK from BCLK (bit 7 = 1), enable BCLK/MCLK/DAC clocks
    //    0x01 = 0xB5 -> MCLK_SEL=1 (BCLK), BCLK_ON=1, CLK_DAC_ON=1, ANACLK_DAC_ON=1
    writeReg(0x01, 0xB5);
    writeReg(0x02, 0x18); // Clock divider pre-scale
    writeReg(0x03, 0x10); // ADC OSR
    writeReg(0x04, 0x10); // DAC OSR = 64x (clean audio)
    writeReg(0x05, 0x00); // Clock dividers
    writeReg(0x06, 0x03); // BCLK divider
    writeReg(0x07, 0x00); // LRCK divider H
    writeReg(0x08, 0xFF); // LRCK divider L

    // 4. Serial Audio Data Format: Standard I2S, 16-bit
    writeReg(0x09, 0x0C); // SDP IN: 16-bit standard I2S, unmuted
    writeReg(0x0A, 0x0C); // SDP OUT: 16-bit standard I2S

    // 5. System & Analog Power Up
    writeReg(0x0D, 0x01); // Power up analog circuitry
    writeReg(0x0E, 0x02); // Enable analog PGA / bias
    writeReg(0x12, 0x00); // Power up DAC
    writeReg(0x13, 0x10); // Enable output driver to speaker/headphone jack
    writeReg(0x14, 0x1A); // Analog reference

    // 6. DAC Digital Control & Volume
    writeReg(0x31, 0x00);     // Unmute DAC digital soft-mute
    writeReg(0x32, hwVolume); // DAC volume (0xBF = 0dB clean reference, max 0xFF)
    writeReg(0x37, 0x08);     // DAC ramp rate (smooth anti-pop)

    return isPresent();
}

inline void setMute(bool muted) {
  if (!isPresent()) return;
  writeReg(0x31, muted ? 0x60 : 0x00);
}

inline void setHardwareVolume(uint8_t regVal) {
  if (!isPresent()) return;
  writeReg(0x32, regVal);
}

} // namespace HalES8311

// ===================== themes.h =====================

// ============================================================================
// Themes, Visualizers & Font Profiles (Zero-overhead POD structs in Flash)
// ============================================================================

struct Theme {
    const char* name;
    uint16_t bg;          // Main background
    uint16_t panel;       // Secondary panel / header bg
    uint16_t border;      // Divider & border lines
    uint16_t primary;     // Main accent (progress, active cursor, peaks)
    uint16_t secondary;   // Secondary accent (codec badge, icons)
    uint16_t text;        // Normal text
    uint16_t textDim;     // Dimmed / inactive text
    uint16_t selBg;       // Selected list row background
    uint16_t selText;     // Selected list row text
    uint16_t visLow;      // Visualizer bottom color
    uint16_t visMid;      // Visualizer mid color
    uint16_t visHigh;     // Visualizer peak color
};

// Helper macro to pack 24-bit RGB into RGB565 at compile time
#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

constexpr uint8_t THEME_COUNT = 6;

static const Theme THEMES[THEME_COUNT] = {
    // 0: Cyber Neon (Default)
    {
        "Cyber Neon",
        RGB565(10, 12, 20),    // bg
        RGB565(18, 24, 38),    // panel
        RGB565(35, 55, 85),    // border
        RGB565(0, 230, 255),   // primary (cyan)
        RGB565(255, 60, 180),  // secondary (magenta)
        RGB565(235, 245, 255), // text
        RGB565(115, 135, 165), // textDim
        RGB565(0, 110, 140),   // selBg
        RGB565(255, 255, 255), // selText
        RGB565(0, 200, 180),   // visLow
        RGB565(0, 230, 255),   // visMid
        RGB565(255, 60, 180)   // visHigh
    },
    // 1: Winamp Classic
    {
        "Winamp",
        RGB565(16, 18, 22),
        RGB565(32, 36, 46),
        RGB565(64, 72, 90),
        RGB565(0, 240, 80),
        RGB565(255, 210, 0),
        RGB565(0, 240, 80),
        RGB565(100, 140, 110),
        RGB565(0, 75, 140),
        RGB565(255, 255, 255),
        RGB565(0, 220, 40),
        RGB565(240, 220, 0),
        RGB565(255, 45, 30)
    },
    // 2: Amber Terminal
    {
        "Amber CRT",
        RGB565(18, 12, 4),
        RGB565(34, 22, 8),
        RGB565(90, 56, 16),
        RGB565(255, 176, 0),
        RGB565(255, 110, 20),
        RGB565(255, 200, 90),
        RGB565(150, 100, 35),
        RGB565(140, 82, 0),
        RGB565(255, 245, 210),
        RGB565(180, 95, 0),
        RGB565(255, 165, 0),
        RGB565(255, 235, 120)
    },
    // 3: Synthwave 84
    {
        "Synthwave",
        RGB565(20, 8, 36),
        RGB565(36, 14, 62),
        RGB565(95, 35, 140),
        RGB565(255, 50, 160),
        RGB565(50, 235, 255),
        RGB565(250, 230, 255),
        RGB565(155, 110, 190),
        RGB565(140, 25, 95),
        RGB565(255, 255, 255),
        RGB565(110, 40, 255),
        RGB565(255, 50, 160),
        RGB565(255, 220, 60)
    },
    // 4: Forest OLED (Ultra high contrast dark)
    {
        "Forest",
        RGB565(4, 10, 8),
        RGB565(12, 24, 18),
        RGB565(35, 75, 55),
        RGB565(80, 245, 140),
        RGB565(180, 255, 90),
        RGB565(225, 250, 235),
        RGB565(95, 140, 115),
        RGB565(25, 95, 55),
        RGB565(255, 255, 255),
        RGB565(30, 150, 75),
        RGB565(80, 245, 140),
        RGB565(210, 255, 110)
    },
    // 5: Ice Minimal
    {
        "Ice Clean",
        RGB565(14, 18, 24),
        RGB565(26, 32, 42),
        RGB565(60, 74, 92),
        RGB565(130, 200, 255),
        RGB565(255, 130, 100),
        RGB565(240, 245, 250),
        RGB565(125, 140, 160),
        RGB565(55, 90, 135),
        RGB565(255, 255, 255),
        RGB565(80, 140, 210),
        RGB565(140, 210, 255),
        RGB565(255, 255, 255)
    }
};

enum VisualizerMode : uint8_t {
    VIS_BARS = 0, // Classic multi-band equalizer bars
    VIS_PEAK = 1, // Bars with floating peak-hold dots
    VIS_WAVE = 2, // Dynamic audio waveform ribbon
    VIS_OFF  = 3, // Disabled (cuts UI refresh to 10 FPS to save battery)
    VIS_COUNT = 4
};

inline const char* visModeName(VisualizerMode m) {
    switch (m) {
        case VIS_BARS: return "BARS";
        case VIS_PEAK: return "PEAK";
        case VIS_WAVE: return "WAVE";
        default:       return "OFF";
    }
}

enum FontProfile : uint8_t {
    FONT_COMPACT = 0, // Small crisp font (more tracks visible in list)
    FONT_NORMAL  = 1, // Balanced default
    FONT_LARGE   = 2, // Larger track title & readable list
    FONT_COUNT   = 3
};

inline const char* fontProfileName(FontProfile f) {
    switch (f) {
        case FONT_COMPACT: return "SMALL";
        case FONT_NORMAL:  return "MED";
        default:           return "LARGE";
    }
}

// ===================== playlist.h =====================

// ============================================================================
// Zero-Allocation Folder Browser & Non-Destructive Shuffle / Playback Engine
//
// Key Optimizations:
// 1. ZERO heap fragmentation: all filenames live in one static 20KB char arena.
// 2. Non-destructive TEMPORARY SHUFFLE:
//    - Original sorted track order is NEVER modified or written to disk.
//    - playOrder[0..trackCount-1] holds a Fisher-Yates permutation of indices.
//    - Turning Shuffle ON puts the currently playing track at playOrder[0] so
//      playback continues seamlessly without interruption or repeats.
//    - Turning Shuffle OFF restores playOrder[i] = i and maps cursor right
//      back to the exact original position in the folder!
// 3. Full playback modes:
//    - LOOP_NORMAL: Play to end of folder and stop
//    - LOOP_1X:     Repeat current track EXACTLY 1 extra time, then advance
//    - LOOP_INF:    Repeat current track infinitely
//    - LOOP_ALL:    Loop entire folder endlessly
//    - Reverse flag: Plays sequence backwards (N -> N-1 -> ... -> 1)
// ============================================================================

enum AudioCodec : uint8_t {
    CODEC_UNKNOWN = 0,
    CODEC_MP3,
    CODEC_M4A,
    CODEC_AAC,
    CODEC_WAV,
    CODEC_FLAC
};

inline const char* codecLabel(AudioCodec c) {
    switch (c) {
        case CODEC_MP3:  return "MP3";
        case CODEC_M4A:  return "M4A";
        case CODEC_AAC:  return "AAC";
        case CODEC_WAV:  return "WAV";
        case CODEC_FLAC: return "FLAC";
        default:         return "AUD";
    }
}

enum LoopMode : uint8_t {
    LOOP_NORMAL = 0, // Normal sequential playback
    LOOP_1X     = 1, // Repeat current track 1 extra time, then next
    LOOP_INF    = 2, // Repeat current track infinitely
    LOOP_ALL    = 3, // Loop entire folder
    LOOP_COUNT  = 4
};

inline const char* loopModeShort(LoopMode m) {
    switch (m) {
        case LOOP_NORMAL: return "NORM";
        case LOOP_1X:     return "RP1X";
        case LOOP_INF:    return "RPINF";
        case LOOP_ALL:    return "RPALL";
        default:          return "NORM";
    }
}

struct DirEntry {
  uint32_t fileOffset : 24;
  uint32_t codec      : 4;
  uint32_t isDir      : 1;
};

class PlaylistManager {
public:
  DirEntry entries[MAX_DIR_ENTRIES];
  uint16_t entryCount = 0;

  uint16_t trackEntryIdx[MAX_DIR_ENTRIES];
  uint16_t trackCount = 0;

  uint16_t playOrder[MAX_DIR_ENTRIES];
  int16_t  playCursor = -1;

  char     currentDir[MAX_PATH_LEN] = "/";
  LoopMode loopMode = LOOP_ALL;
  bool     shuffleEnabled = false;
  bool     reverseEnabled = false;
  uint8_t  repeat1xRemaining = 0;

  mutable File    idxFile;
  mutable char    cacheNames[16][96];
  mutable int32_t cacheIdx[16];
  mutable uint8_t cacheNext = 0;

  PlaylistManager() {
    clearCache();
  }

  static AudioCodec detectCodec(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot || dot == name) return CODEC_UNKNOWN;
    char ext[8];
    size_t i = 0;
    for (const char* p = dot + 1; *p && i < 7; ++p, ++i) {
      ext[i] = (char)tolower((unsigned char)*p);
    }
    ext[i] = '\0';
    if (strcmp(ext, "mp3")  == 0) return CODEC_MP3;
    if (strcmp(ext, "m4a")  == 0) return CODEC_M4A;
    if (strcmp(ext, "aac")  == 0) return CODEC_AAC;
    if (strcmp(ext, "wav")  == 0) return CODEC_WAV;
    return CODEC_UNKNOWN;
  }

  const char* entryName(uint16_t idx) const {
    if (idx >= entryCount) return "";
    for (uint8_t i = 0; i < 16; ++i) {
      if (cacheIdx[i] == (int32_t)idx) return cacheNames[i];
    }
    uint8_t slot = cacheNext;
    cacheNext = (cacheNext + 1) & 15;
    cacheIdx[slot] = (int32_t)idx;
    cacheNames[slot][0] = '\0';

    if (idxFile) {
      idxFile.seek(entries[idx].fileOffset);
      size_t n = idxFile.readBytesUntil('\n', cacheNames[slot], 95);
      while (n > 0 && (cacheNames[slot][n - 1] == '\r' || cacheNames[slot][n - 1] == '\n')) n--;
      cacheNames[slot][n] = '\0';
    }
    return cacheNames[slot];
  }

  inline const char* trackName(uint16_t trackIdx) const {
    if (trackIdx >= trackCount) return "";
    return entryName(trackEntryIdx[trackIdx]);
  }

  inline AudioCodec trackCodec(uint16_t trackIdx) const {
    if (trackIdx >= trackCount) return CODEC_UNKNOWN;
    return (AudioCodec)entries[trackEntryIdx[trackIdx]].codec;
  }

  inline int16_t currentTrackIndex() const {
    if (playCursor < 0 || playCursor >= (int16_t)trackCount) return -1;
    return (int16_t)playOrder[playCursor];
  }

  void buildFullPath(const char* filename, char* outBuf, size_t outSize) const {
    if (strcmp(currentDir, "/") == 0) {
      snprintf(outBuf, outSize, "/%s", filename);
    } else {
      snprintf(outBuf, outSize, "%s/%s", currentDir, filename);
    }
  }

  bool getCurrentTrackPath(char* outBuf, size_t outSize) const {
    int16_t t = currentTrackIndex();
    if (t < 0) return false;
    buildFullPath(trackName((uint16_t)t), outBuf, outSize);
    return true;
  }

  bool loadDirectory(const char* path, bool forceRescan = false) {
    if (idxFile) idxFile.close();
    clearCache();

    strncpy(currentDir, path, MAX_PATH_LEN - 1);
    currentDir[MAX_PATH_LEN - 1] = '\0';
    size_t len = strlen(currentDir);
    if (len > 1 && currentDir[len - 1] == '/') {
      currentDir[len - 1] = '\0';
    }

    char idxPath[MAX_PATH_LEN];
    char binPath[MAX_PATH_LEN];
    buildFullPath(".mp3plus.idx", idxPath, sizeof(idxPath));
    buildFullPath(".mp3plus.bin", binPath, sizeof(binPath));

    if (!forceRescan && SD.exists(binPath) && SD.exists(idxPath)) {
      File bf = SD.open(binPath, FILE_READ);
      if (bf) {
        char cachedDir[MAX_PATH_LEN] = {};
        uint16_t cachedCount = 0;
        if (bf.read((uint8_t*)cachedDir, MAX_PATH_LEN) == MAX_PATH_LEN &&
            bf.read((uint8_t*)&cachedCount, sizeof(cachedCount)) == sizeof(cachedCount) &&
            strcmp(cachedDir, currentDir) == 0 && cachedCount <= MAX_DIR_ENTRIES) {
          size_t needBytes = (size_t)cachedCount * sizeof(DirEntry);
          if (bf.read((uint8_t*)entries, needBytes) == needBytes) {
            bf.close();
            entryCount = cachedCount;
            idxFile = SD.open(idxPath, FILE_READ);
            rebuildTrackIndex();
            rebuildPlayOrder(-1);
            return true;
          }
        }
        bf.close();
      }
    }

    File dir = SD.open(currentDir);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return false;
    }

    File out = SD.open(idxPath, FILE_WRITE);
    if (!out) {
      dir.close();
      return false;
    }

    entryCount = 0;
    trackCount = 0;
    playCursor = -1;

    if (strcmp(currentDir, "/") != 0) {
      entries[entryCount].fileOffset = (uint32_t)out.position();
      entries[entryCount].isDir = 1;
      entries[entryCount].codec = CODEC_UNKNOWN;
      out.print("..\n");
      entryCount++;
    }

    File f = dir.openNextFile();
    while (f && entryCount < MAX_DIR_ENTRIES) {
      const char* rawName = f.name();
      const char* base = strrchr(rawName, '/');
      base = base ? (base + 1) : rawName;

      if (base[0] != '.' && strcmp(base, "System Volume Information") != 0) {
        if (f.isDirectory()) {
          entries[entryCount].fileOffset = (uint32_t)out.position();
          entries[entryCount].isDir = 1;
          entries[entryCount].codec = CODEC_UNKNOWN;
          out.print(base);
          out.print('\n');
          entryCount++;
        } else {
          AudioCodec c = detectCodec(base);
          if (c != CODEC_UNKNOWN) {
            entries[entryCount].fileOffset = (uint32_t)out.position();
            entries[entryCount].isDir = 0;
            entries[entryCount].codec = (uint8_t)c;
            out.print(base);
            out.print('\n');
            entryCount++;
          }
        }
      }
      if ((entryCount & 31) == 0 && entryCount > 0) {
        auto& d = M5Cardputer.Display;
        d.fillRoundRect(30, 52, 180, 32, 4, RGB565(12, 8, 0));
        d.drawRoundRect(30, 52, 180, 32, 4, RGB565(255, 175, 0));
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextColor(RGB565(255, 220, 80), RGB565(12, 8, 0));
        d.setCursor(44, 59);
        d.printf("SCANNING... %u files", entryCount);
        int barW = min(156, (int)((entryCount % 512) * 156 / 512));
        d.drawRect(42, 71, 156, 7, RGB565(180, 100, 0));
        d.fillRect(43, 72, barW, 5, RGB565(255, 200, 0));
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
    out.close();

    uint16_t start = (entryCount > 0 && entries[0].isDir && strcmp(currentDir, "/") != 0) ? 1 : 0;
    uint16_t writePos = start;
    for (uint16_t i = start; i < entryCount; ++i) {
      if (entries[i].isDir) {
        if (i != writePos) {
          DirEntry tmp = entries[i];
          for (uint16_t j = i; j > writePos; --j) entries[j] = entries[j - 1];
          entries[writePos] = tmp;
        }
        writePos++;
      }
    }

    File bf = SD.open(binPath, FILE_WRITE);
    if (bf) {
      bf.write((const uint8_t*)currentDir, MAX_PATH_LEN);
      bf.write((const uint8_t*)&entryCount, sizeof(entryCount));
      bf.write((const uint8_t*)entries, (size_t)entryCount * sizeof(DirEntry));
      bf.close();
    }

    idxFile = SD.open(idxPath, FILE_READ);
    rebuildTrackIndex();
    rebuildPlayOrder(-1);
    return true;
      }

  bool goParentDirectory() {
    if (strcmp(currentDir, "/") == 0) return false;
    char parent[MAX_PATH_LEN];
    strncpy(parent, currentDir, MAX_PATH_LEN - 1);
    parent[MAX_PATH_LEN - 1] = '\0';
    char* slash = strrchr(parent, '/');
    if (!slash || slash == parent) strcpy(parent, "/");
    else *slash = '\0';
    return loadDirectory(parent);
  }

  void selectTrackByOriginalIndex(uint16_t origIdx) {
    if (origIdx >= trackCount) return;
    if (shuffleEnabled) {
      rebuildPlayOrder((int16_t)origIdx);
      playCursor = 0;
    } else {
      playCursor = (int16_t)origIdx;
    }
    armRepeat1x();
  }

  void setShuffle(bool enable) {
    if (shuffleEnabled == enable) return;
    int16_t activeOrig = currentTrackIndex();
    shuffleEnabled = enable;
    rebuildPlayOrder(activeOrig);
  }

  void setLoopMode(LoopMode m) {
    loopMode = m;
    armRepeat1x();
  }

  void setReverse(bool rev) {
    reverseEnabled = rev;
  }

  bool advanceOnEOF() {
    if (trackCount == 0) return false;
    if (loopMode == LOOP_INF) return true;
    if (loopMode == LOOP_1X && repeat1xRemaining > 0) {
      repeat1xRemaining--;
      return true;
    }
    bool ok = stepTrack(reverseEnabled ? -1 : +1, loopMode == LOOP_ALL);
    if (ok) armRepeat1x();
    return ok;
  }

  bool nextTrack() {
    if (trackCount == 0) return false;
    bool ok = stepTrack(reverseEnabled ? -1 : +1, true);
    if (ok) armRepeat1x();
    return ok;
  }

  bool prevTrack() {
    if (trackCount == 0) return false;
    bool ok = stepTrack(reverseEnabled ? +1 : -1, true);
    if (ok) armRepeat1x();
    return ok;
  }

private:
  void clearCache() const {
    for (uint8_t i = 0; i < 16; ++i) {
      cacheIdx[i] = -1;
      cacheNames[i][0] = '\0';
    }
    cacheNext = 0;
  }

  void armRepeat1x() {
    repeat1xRemaining = (loopMode == LOOP_1X) ? 1 : 0;
  }

  void rebuildTrackIndex() {
    trackCount = 0;
    for (uint16_t i = 0; i < entryCount; ++i) {
      if (!entries[i].isDir) {
        trackEntryIdx[trackCount++] = i;
      }
    }
  }

  void rebuildPlayOrder(int16_t keepOriginalTrack) {
    for (uint16_t i = 0; i < trackCount; ++i) playOrder[i] = i;
    if (trackCount == 0) {
      playCursor = -1;
      return;
    }
    if (!shuffleEnabled) {
      if (keepOriginalTrack >= 0 && keepOriginalTrack < (int16_t)trackCount) {
        playCursor = keepOriginalTrack;
      } else {
        playCursor = reverseEnabled ? (int16_t)(trackCount - 1) : 0;
      }
      return;
    }
    for (int i = (int)trackCount - 1; i > 0; --i) {
      uint32_t r = esp_random() % (uint32_t)(i + 1);
      uint16_t tmp = playOrder[i];
      playOrder[i] = playOrder[r];
      playOrder[r] = tmp;
    }
    if (keepOriginalTrack >= 0 && keepOriginalTrack < (int16_t)trackCount) {
      for (uint16_t i = 0; i < trackCount; ++i) {
        if (playOrder[i] == (uint16_t)keepOriginalTrack) {
          uint16_t tmp = playOrder[0];
          playOrder[0] = playOrder[i];
          playOrder[i] = tmp;
          break;
        }
      }
      playCursor = 0;
    } else {
      playCursor = 0;
    }
  }

  bool stepTrack(int delta, bool wrap) {
    if (trackCount == 0) return false;
    if (playCursor < 0) {
      playCursor = (delta < 0) ? (int16_t)(trackCount - 1) : 0;
      return true;
    }
    int next = (int)playCursor + delta;
    if (next >= (int)trackCount) {
      if (!wrap) return false;
      if (shuffleEnabled) {
        rebuildPlayOrder(-1);
        playCursor = 0;
        return true;
      }
      next = 0;
    } else if (next < 0) {
      if (!wrap) return false;
      next = (int)trackCount - 1;
    }
    playCursor = (int16_t)next;
    return true;
  }
};

// ======= PART 2: AUDIO ENGINE =======

#include <Audio.h>

enum AmpBoost : uint8_t {
  BOOST_100 = 0, // 0 dB clean reference (0xBF)
  BOOST_150 = 1, // +6 dB loud (0xCB)
  BOOST_200 = 2, // +11 dB max turbo boost (0xD5)
  BOOST_COUNT = 3
};

inline uint8_t ampBoostReg(AmpBoost b) {
  switch (b) {
    case BOOST_150: return 0xCB;
    case BOOST_200: return 0xD5;
    default:        return 0xBF;
  }
}

inline const char* ampBoostName(AmpBoost b) {
  switch (b) {
    case BOOST_150: return "150% (+6dB)";
    case BOOST_200: return "200% (+11dB)";
    default:        return "100% (0dB)";
  }
}

enum AudioCmdType : uint8_t {
  CMD_PLAY_FILE = 0,
  CMD_PAUSE_RESUME,
  CMD_STOP,
  CMD_SET_VOLUME,
  CMD_SEEK_REL,
  CMD_SET_BOOST
};

struct AudioCmd {
  AudioCmdType type;
  int16_t      param;
  char         path[MAX_PATH_LEN];
};

struct AudioTelemetry {
  volatile bool     isPlaying   = false;
  volatile bool     isPaused    = false;
  volatile bool     eofPending  = false;
  volatile uint32_t currentSec  = 0;
  volatile uint32_t durationSec = 0;
  volatile uint32_t bitrate     = 0;
  volatile uint32_t sampleRate  = 0;
  volatile uint8_t  vuLeft      = 0; // 0..127
  volatile uint8_t  vuRight     = 0; // 0..127
  volatile uint8_t  volume      = VOL_DEFAULT;
  char              metaTitle[64]  = "";
  char              metaArtist[64] = "";
  volatile bool     metaDirty      = false;
  volatile uint8_t  visBass      = 0;
  volatile uint8_t  visMid       = 0;
  volatile uint8_t  visTreble    = 0;
  volatile uint32_t trackStartMs = 0;
};

class AudioEngine {
public:
  Audio*         audio = nullptr;
  AudioTelemetry state;
  QueueHandle_t  cmdQueue = nullptr;
  TaskHandle_t   taskHandle = nullptr;
  bool           es8311Ok = false;

  static AudioEngine* instance;

  bool begin(uint8_t initialVolume, AmpBoost initialBoost = BOOST_100) {
    instance = this;
    state.volume = constrain(initialVolume, VOL_MIN, VOL_MAX);

    es8311Ok = HalES8311::initDAC(ampBoostReg(initialBoost));

    if (!audio) audio = new Audio();
    audio->setPinout(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT);
    audio->setVolume(state.volume);

    cmdQueue = xQueueCreate(8, sizeof(AudioCmd));
    if (!cmdQueue) return false;

    BaseType_t ok = xTaskCreatePinnedToCore(
      audioTaskTrampoline,
      "AudioCore0",
      AUDIO_TASK_STACK,
      this,
      AUDIO_TASK_PRIO,
      &taskHandle,
      AUDIO_TASK_CORE
    );
    return (ok == pdPASS);
  }

  void playFile(const char* fullPath) {
    AudioCmd cmd{};
    cmd.type = CMD_PLAY_FILE;
    strncpy(cmd.path, fullPath, MAX_PATH_LEN - 1);
    cmd.path[MAX_PATH_LEN - 1] = '\0';
    xQueueSend(cmdQueue, &cmd, 0);
  }

  void togglePause() {
    AudioCmd cmd{};
    cmd.type = CMD_PAUSE_RESUME;
    xQueueSend(cmdQueue, &cmd, 0);
  }

  void stop() {
    AudioCmd cmd{};
    cmd.type = CMD_STOP;
    xQueueSend(cmdQueue, &cmd, 0);
  }

  void setVolume(uint8_t vol) {
    vol = constrain(vol, VOL_MIN, VOL_MAX);
    state.volume = vol;
    AudioCmd cmd{};
    cmd.type = CMD_SET_VOLUME;
    cmd.param = vol;
    xQueueSend(cmdQueue, &cmd, 0);
  }

  void setBoost(AmpBoost b) {
    AudioCmd cmd{};
    cmd.type = CMD_SET_BOOST;
    cmd.param = (int16_t)b;
    xQueueSend(cmdQueue, &cmd, 0);
  }

  void seekRelative(int16_t deltaSeconds) {
    AudioCmd cmd{};
    cmd.type = CMD_SEEK_REL;
    cmd.param = deltaSeconds;
    xQueueSend(cmdQueue, &cmd, 0);
  }

  bool consumeEOF() {
    if (state.eofPending) {
      state.eofPending = false;
      return true;
    }
    return false;
  }

  void onMetadata(const char* info) {
    if (!info) return;
    if (strncmp(info, "Title: ", 7) == 0) {
      strncpy(state.metaTitle, info + 7, sizeof(state.metaTitle) - 1);
      state.metaTitle[sizeof(state.metaTitle) - 1] = '\0';
      state.metaDirty = true;
    } else if (strncmp(info, "Artist: ", 8) == 0) {
      strncpy(state.metaArtist, info + 8, sizeof(state.metaArtist) - 1);
      state.metaArtist[sizeof(state.metaArtist) - 1] = '\0';
      state.metaDirty = true;
    }
  }

  void onEOF() {
    state.isPlaying = false;
    state.isPaused  = false;
    state.vuLeft    = 0;
    state.vuRight   = 0;
    state.eofPending = true;
  }

private:
  static void audioTaskTrampoline(void* arg) {
    static_cast<AudioEngine*>(arg)->runLoop();
  }

  void runLoop() {
    AudioCmd cmd;
    uint32_t lastTelemetryMs = 0;

    for (;;) {
      while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
          case CMD_PLAY_FILE:
            HalES8311::setMute(true);
            audio->stopSong();
              vTaskDelay(pdMS_TO_TICKS(15));
            state.metaTitle[0]  = '\0';
            state.metaArtist[0] = '\0';
            state.currentSec    = 0;
            state.durationSec   = 0;
            state.eofPending    = false;
            state.isPaused      = false;
            state.metaDirty     = true;
            state.trackStartMs  = millis();
              if (ESP.getMaxAllocHeap() < 42000) {
  state.isPlaying = false;
  break;
 
              }
              
            if (audio->connecttoFS(SD, cmd.path)) {
              state.isPlaying = true;
              HalES8311::setMute(false);
            } else {
              state.isPlaying = false;
            }
            break;

          case CMD_PAUSE_RESUME:
            if (state.isPlaying) {
              audio->pauseResume();
              state.isPaused = !state.isPaused;
              HalES8311::setMute(state.isPaused);
              if (state.isPaused) {
                state.vuLeft = 0;
                state.vuRight = 0;
              }
            }
            break;

          case CMD_STOP:
            HalES8311::setMute(true);
            audio->stopSong();
            state.isPlaying = false;
            state.isPaused  = false;
            state.vuLeft    = 0;
            state.vuRight   = 0;
            break;

          case CMD_SET_VOLUME:
            audio->setVolume((uint8_t)cmd.param);
            break;

          case CMD_SEEK_REL:
            if (state.isPlaying && !state.isPaused) {
              audio->setTimeOffset(cmd.param);
            }
            break;

          case CMD_SET_BOOST:
            HalES8311::setHardwareVolume(ampBoostReg((AmpBoost)cmd.param));
            break;
        }
      }

      if (state.isPlaying && !state.isPaused) {
        audio->loop();

        uint32_t now = millis();
        if (now - lastTelemetryMs >= 40) {
          lastTelemetryMs = now;
          state.currentSec  = audio->getAudioCurrentTime();
          state.durationSec = audio->getAudioFileDuration();
          state.bitrate     = audio->getBitRate();
          state.sampleRate  = audio->getSampleRate();

        }
        vTaskDelay(1);
      } else {
        vTaskDelay(pdMS_TO_TICKS(12));
      }
    }
  }
};

inline AudioEngine* AudioEngine::instance = nullptr;


// ======= PART 3: UI RENDERER =======//

enum AppScreen : uint8_t {
  SCR_PLAYER   = 0, // Split-screen: Playlist (left) + Now Playing & Vis (right)
  SCR_FOLDERS  = 1, // Full-screen SD folder & file browser
  SCR_SETTINGS = 2, // Settings menu (Theme, Vis, Font, Brightness, Sleep, CPU)
  SCR_HELP     = 3  // Scrollable hotkey reference card
};

struct UiSettings {
  uint8_t        themeIdx       = 0;
  VisualizerMode visMode        = VIS_PEAK;
  FontProfile    fontProfile    = FONT_NORMAL;
  uint8_t        brightness     = 160; // 40..255
  uint16_t       screenTimeoutS = 30;  // 0 = Never, 15, 30, 60, 120
  bool           cpuPowerSaver  = false; // 160 MHz when screen off / idle
  AmpBoost       ampBoost       = BOOST_100;
  uint8_t        seekStepSec    = 5;     // 5 or 10 sec
};

class UiRenderer {
public:
  M5Canvas canvas;
  AppScreen screen = SCR_PLAYER;
  int16_t   listCursor = 0;      // Cursor in Player track list or Folder browser
  int16_t   settingsCursor = 0;  // Cursor in Settings menu
  int16_t   helpScroll = 0;

  static constexpr uint8_t VIS_BANDS = 12;
  float barLevel[VIS_BANDS] = {};
  float peakLevel[VIS_BANDS] = {};
  uint8_t waveHist[24] = {};
  uint8_t waveHead = 0;
  uint32_t titleScrollTick = 0;

  bool begin() {
    canvas.setColorDepth(8); // 32.4 KB SRAM — huge win on no-PSRAM ESP32-S3!
    return canvas.createSprite(SCREEN_W, SCREEN_H);
  }

  void updateVisualizer(const AudioTelemetry& st) {
    if (!st.isPlaying || st.isPaused) {
      for (uint8_t i = 0; i < VIS_BANDS; ++i) {
        barLevel[i] *= 0.72f;
        peakLevel[i] = max(0.0f, peakLevel[i] - 0.03f);
      }
      return;
    }

    float b = min(1.0f, (float)st.visBass   / 90.0f);
    float m = min(1.0f, (float)st.visMid    / 85.0f);
    float t = min(1.0f, (float)st.visTreble / 80.0f);
    float avg = min(1.0f, (float)(st.vuLeft + st.vuRight) / 180.0f);

    waveHist[waveHead] = (uint8_t)(avg * 255.0f);
    waveHead = (waveHead + 1) % 24;

    const float targets[VIS_BANDS] = {
      b * 1.00f, b * 0.92f, b * 0.78f + m * 0.25f, b * 0.55f + m * 0.50f,
      m * 0.95f, m * 1.00f, m * 0.88f + t * 0.20f, m * 0.65f + t * 0.45f,
      t * 0.85f + m * 0.25f, t * 0.95f, t * 1.00f, t * 0.82f
    };

    for (uint8_t i = 0; i < VIS_BANDS; ++i) {
      float tgt = constrain(targets[i], 0.02f, 1.0f);
      if (tgt > barLevel[i]) barLevel[i] = barLevel[i] * 0.25f + tgt * 0.75f;
      else                   barLevel[i] = barLevel[i] * 0.75f + tgt * 0.25f;

      if (barLevel[i] > peakLevel[i]) peakLevel[i] = barLevel[i];
      else peakLevel[i] = max(barLevel[i], peakLevel[i] - 0.025f);
    
    }
  }

  void render(const PlaylistManager& pl, const AudioTelemetry& st,
        const UiSettings& cfg, int batteryPct, bool sdOk) {
    const Theme& th = THEMES[cfg.themeIdx % THEME_COUNT];
    canvas.fillSprite(th.bg);

    drawTopBar(pl, st, cfg, th, batteryPct);

    if (!sdOk) {
      drawCenteredMsg("NO MICROSD CARD", "Insert FAT32 SD & press R", th);
    } else {
      switch (screen) {
        case SCR_PLAYER:   drawPlayerScreen(pl, st, cfg, th); break;
        case SCR_FOLDERS:  drawFolderScreen(pl, cfg, th);     break;
        case SCR_SETTINGS: drawSettingsScreen(cfg, th);       break;
        case SCR_HELP:     drawHelpScreen(th);                break;
      }
    }

    canvas.pushSprite(&M5Cardputer.Display, 0, 0);
  }

private:
    void applyListFont(FontProfile fp) {
    canvas.setTextSize(1);
    if (fp == FONT_COMPACT)     canvas.setFont(&fonts::efontCN_10);
    else if (fp == FONT_NORMAL) canvas.setFont(&fonts::efontCN_12);
    else                        canvas.setFont(&fonts::efontCN_16);
  }

  static int utf8CharLen(const char* s) {
    int c = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
      if ((*p & 0xC0) != 0x80) c++;
    }
    return c;
  }

  static void utf8Slice(const char* src, int startChar, int maxChars, char* dst, size_t dstSize) {
    const unsigned char* p = (const unsigned char*)src;
    int skipped = 0;
    while (*p && skipped < startChar) {
      if ((*p & 0xC0) != 0x80) skipped++;
      p++;
    }
    while (*p && (*p & 0xC0) == 0x80) p++;
    size_t out = 0;
    int copied = 0;
    while (*p && copied < maxChars && out + 4 < dstSize) {
      unsigned char c = *p;
      int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
      for (int i = 0; i < len && *p && out + 1 < dstSize; ++i) dst[out++] = (char)*p++;
      copied++;
    }
    dst[out] = '\0';
  }


  void drawTopBar(const PlaylistManager& pl, const AudioTelemetry& st,
          const UiSettings& cfg, const Theme& th, int batPct) {
    canvas.fillRect(0, 0, SCREEN_W, 15, th.panel);
    canvas.drawFastHLine(0, 15, SCREEN_W, th.border);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);

    int x = 3;
    if (pl.shuffleEnabled) {
      canvas.fillRoundRect(x, 2, 23, 11, 2, th.secondary);
      canvas.setTextColor(th.bg, th.secondary);
      canvas.setCursor(x + 3, 4);
      canvas.print("SHF");
      x += 25;
    }
    if (pl.reverseEnabled) {
      canvas.fillRoundRect(x, 2, 23, 11, 2, th.primary);
      canvas.setTextColor(th.bg, th.primary);
      canvas.setCursor(x + 3, 4);
      canvas.print("REV");
      x += 25;
    }
    canvas.setTextColor(th.primary, th.panel);
    canvas.setCursor(x + 1, 4);
    canvas.print(loopModeShort(pl.loopMode));

    canvas.setTextColor(th.text, th.panel);
    canvas.setCursor(138, 4);
    canvas.printf("V:%02u%s", st.volume, cfg.ampBoost == BOOST_200 ? "!!" : (cfg.ampBoost == BOOST_150 ? "+" : ""));

    uint16_t batCol = (batPct > 25) ? th.primary : RGB565(255, 60, 60);
    canvas.setTextColor(batCol, th.panel);
    canvas.setCursor(184, 4);
    canvas.printf("%3d%%", batPct);
    canvas.drawRect(216, 3, 19, 9, batCol);
    canvas.fillRect(235, 5, 2, 5, batCol);
    int fillW = constrain((batPct * 15) / 100, 1, 15);
    canvas.fillRect(218, 5, fillW, 5, batCol);
  }

  void drawPlayerScreen(const PlaylistManager& pl, const AudioTelemetry& st,
             const UiSettings& cfg, const Theme& th) {
    constexpr int SPLIT_X = 109;
    canvas.drawFastVLine(SPLIT_X, 16, SCREEN_H - 16, th.border);

    applyListFont(cfg.fontProfile);
    int rowH = (cfg.fontProfile == FONT_COMPACT) ? 11 :
         (cfg.fontProfile == FONT_NORMAL)  ? 13 : 16;
    int visibleRows = (SCREEN_H - 18) / rowH;

    if (pl.trackCount == 0) {
      canvas.setTextColor(th.textDim, th.bg);
      canvas.setCursor(6, 40);
      canvas.print("No audio here");
      canvas.setCursor(6, 54);
      canvas.print("Press [F]");
      canvas.setCursor(6, 66);
      canvas.print("for folders");
    } else {
      int16_t activeTrack = pl.currentTrackIndex();
      int16_t cursor = constrain(listCursor, 0, (int16_t)pl.trackCount - 1);
      int16_t topIdx = max(0, cursor - visibleRows / 2);
      if (topIdx + visibleRows > (int16_t)pl.trackCount) {
        topIdx = max(0, (int16_t)pl.trackCount - visibleRows);
      }

      int maxChars = (cfg.fontProfile == FONT_LARGE) ? 12 : 16;
      for (int r = 0; r < visibleRows; ++r) {
        int16_t idx = topIdx + r;
        if (idx >= (int16_t)pl.trackCount) break;
        int y = 18 + r * rowH;

        bool isSelected = (idx == cursor);
        bool isPlayingRow = (idx == activeTrack);

        if (isSelected) {
          canvas.fillRect(1, y, SPLIT_X - 2, rowH - 1, th.selBg);
          canvas.setTextColor(th.selText, th.selBg);
        } else {
          canvas.setTextColor(isPlayingRow ? th.primary : th.text, th.bg);
        }

        canvas.setCursor(3, y + 2);
        canvas.print(isPlayingRow ? ">" : " ");

        const char* raw = pl.trackName((uint16_t)idx);
        char shortName[48];
utf8Slice(pl.trackName((uint16_t)idx), 0, maxChars, shortName, sizeof(shortName));
canvas.print(shortName);

      }
    }

    constexpr int RX = SPLIT_X + 4;
    constexpr int RW = SCREEN_W - RX - 3;

    int16_t curIdx = pl.currentTrackIndex();
    const char* title = (st.metaTitle[0] != '\0') ? st.metaTitle :
              (curIdx >= 0 ? pl.trackName((uint16_t)curIdx) : "Ready");
    const char* cleanDir = (pl.currentDir[0] == '/' && pl.currentDir[1] != '\0') ? (pl.currentDir + 1) : pl.currentDir;
bool tagsLoading = (st.isPlaying && (millis() - st.trackStartMs < 500));
const char* artist = (st.metaArtist[0] != '\0') ? st.metaArtist : (tagsLoading ? "" : cleanDir);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    const char* badge = (curIdx >= 0) ? codecLabel(pl.trackCodec((uint16_t)curIdx)) : "---";
    canvas.fillRoundRect(RX, 19, 30, 11, 2, th.secondary);
    canvas.setTextColor(th.bg, th.secondary);
    canvas.setCursor(RX + 4, 21);
    canvas.print(badge);

    canvas.setTextColor(st.isPlaying ? th.primary : th.textDim, th.bg);
    canvas.setCursor(RX + 34, 21);
    if (st.isPlaying && !st.isPaused) {
      canvas.printf("%uk", (unsigned)(st.bitrate / 1000));
    } else if (st.isPaused) {
      canvas.print("PAUSED");
    } else {
      canvas.print("STOP");
    }

    if (pl.trackCount > 0 && curIdx >= 0) {
      canvas.setTextColor(th.textDim, th.bg);
      canvas.setCursor(SCREEN_W - 38, 21);
      canvas.printf("%d/%u", pl.playCursor + 1, pl.trackCount);
    }

    applyListFont(cfg.fontProfile);
canvas.setTextColor(th.text, th.bg);
int titleChars = utf8CharLen(title);
int winChars = (cfg.fontProfile == FONT_LARGE) ? 11 : 16;
int offset = 0;
if (titleChars > winChars) {
  offset = (titleScrollTick / 6) % (titleChars - winChars + 4);
  if (offset > titleChars - winChars) offset = 0;
}
char titleWin[64];
utf8Slice(title, offset, winChars, titleWin, sizeof(titleWin));
canvas.setCursor(RX, 33);
canvas.print(titleWin);

canvas.setFont(&fonts::efontCN_10);
canvas.setTextColor(th.textDim, th.bg);
char artWin[48];
utf8Slice(artist, 0, 18, artWin, sizeof(artWin));
canvas.setCursor(RX, cfg.fontProfile == FONT_LARGE ? 49 : 46);
canvas.print(artWin);
canvas.setFont(&fonts::Font0);


    constexpr int VY = 59;
    constexpr int VH = 44;
    if (cfg.visMode != VIS_OFF) {
      drawVisualizer(RX, VY, RW, VH, cfg.visMode, th);
    } else {
      canvas.drawRoundRect(RX, VY + 8, RW, 26, 3, th.border);
      canvas.setTextColor(th.textDim, th.bg);
      canvas.setCursor(RX + 16, VY + 17);
      canvas.print("ECO VIS OFF");
    }

    uint32_t curS = st.currentSec;
    uint32_t durS = st.durationSec;
    canvas.setTextColor(th.textDim, th.bg);
    canvas.setCursor(RX, 108);
    canvas.printf("%02u:%02u", (unsigned)(curS / 60), (unsigned)(curS % 60));
    canvas.setCursor(SCREEN_W - 34, 108);
    canvas.printf("%02u:%02u", (unsigned)(durS / 60), (unsigned)(durS % 60));

    canvas.drawRoundRect(RX, 119, RW, 7, 2, th.border);
    if (durS > 0) {
      int pw = (int)((uint64_t)(RW - 2) * min(curS, durS) / durS);
      if (pw > 0) canvas.fillRoundRect(RX + 1, 120, pw, 5, 1, th.primary);
    }

    canvas.setTextColor(th.textDim, th.bg);
    canvas.setCursor(RX, 128);
    canvas.print("F:Dir M:Set I:Hlp");
    titleScrollTick++;
  }

  void drawVisualizer(int x, int y, int w, int h, VisualizerMode mode, const Theme& th) {
    if (mode == VIS_WAVE) {
      int midY = y + h / 2;
      canvas.drawFastHLine(x, midY, w, th.border);
      int stepX = w / 23;
      for (int i = 0; i < 23; ++i) {
        uint8_t idx1 = (waveHead + i) % 24;
        uint8_t idx2 = (waveHead + i + 1) % 24;
        int amp1 = (waveHist[idx1] * (h / 2 - 2)) / 255;
        int amp2 = (waveHist[idx2] * (h / 2 - 2)) / 255;
        int sign1 = (i & 1) ? 1 : -1;
        int sign2 = ((i + 1) & 1) ? 1 : -1;
        canvas.drawLine(x + i * stepX, midY + amp1 * sign1,
                x + (i + 1) * stepX, midY + amp2 * sign2, th.primary);
        canvas.drawLine(x + i * stepX, midY - amp1 * sign1,
                x + (i + 1) * stepX, midY - amp2 * sign2, th.secondary);
      }
      return;
    }

    int barW = (w - (VIS_BANDS - 1) * 2) / VIS_BANDS;
    if (barW < 4) barW = 4;
    for (uint8_t i = 0; i < VIS_BANDS; ++i) {
      int bx = x + i * (barW + 2);
      int bh = (int)(barLevel[i] * (float)(h - 3));
      if (bh < 2) bh = 2;

            static const uint16_t zoneCols[5] = {
        RGB565(0, 230, 118),   // 1/5: изумрудный
        RGB565(160, 255, 40),  // 2/5: лайм
        RGB565(255, 225, 0),   // 3/5: ярко-жёлтый
        RGB565(255, 115, 0),   // 4/5: оранжевый
        RGB565(255, 30, 70)    // 5/5: неоновый алый
      };
      int step = max(1, h / 5);
      for (int z = 0; z < 5; ++z) {
        int zStart = z * step;
        int zEnd   = (z == 4) ? h : (z + 1) * step;
        int drawH  = min(bh, zEnd) - zStart;
        if (drawH > 0) {
          canvas.fillRect(bx, y + h - zStart - drawH, barW, drawH, zoneCols[z]);
        }
      }

      if (mode == VIS_PEAK) {
        int py = y + h - (int)(peakLevel[i] * (float)(h - 3)) - 2;
        py = constrain(py, y, y + h - 2);
        canvas.fillRect(bx, py, barW, 2, RGB565(255, 255, 255));
      }
    }
  
  }

  void drawFolderScreen(const PlaylistManager& pl, const UiSettings& cfg, const Theme& th) {
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.fillRect(0, 16, SCREEN_W, 13, th.panel);
    canvas.setTextColor(th.secondary, th.panel);
    canvas.setCursor(4, 19);
    canvas.printf("DIR: %.28s", pl.currentDir);
    canvas.setTextColor(th.textDim, th.panel);
    canvas.setCursor(188, 19);
    canvas.printf("(%u trk)", pl.trackCount);

    applyListFont(cfg.fontProfile);
    int rowH = (cfg.fontProfile == FONT_COMPACT) ? 12 :
         (cfg.fontProfile == FONT_NORMAL)  ? 14 : 16;
    int visibleRows = (SCREEN_H - 42) / rowH;

    int16_t cursor = constrain(listCursor, 0, max(0, (int)pl.entryCount - 1));
    int16_t topIdx = max(0, cursor - visibleRows / 2);
    if (topIdx + visibleRows > (int16_t)pl.entryCount) {
      topIdx = max(0, (int16_t)pl.entryCount - visibleRows);
    }

    for (int r = 0; r < visibleRows; ++r) {
      int16_t idx = topIdx + r;
      if (idx >= (int16_t)pl.entryCount) break;
      int y = 31 + r * rowH;
      bool sel = (idx == cursor);
      bool isDir = pl.entries[idx].isDir != 0;

      if (sel) {
        canvas.fillRect(2, y, SCREEN_W - 4, rowH - 1, th.selBg);
        canvas.setTextColor(th.selText, th.selBg);
      } else {
        canvas.setTextColor(isDir ? th.secondary : th.text, th.bg);
      }

      canvas.setCursor(5, y + 2);
      if (isDir) {
        canvas.printf("[DIR] %.28s", pl.entryName((uint16_t)idx));
      } else {
        canvas.printf("[%s] %.28s",
          codecLabel((AudioCodec)pl.entries[idx].codec),
          pl.entryName((uint16_t)idx));
      }
    }

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, th.panel);
    canvas.setTextColor(th.textDim, th.panel);
    canvas.setCursor(4, SCREEN_H - 10);
    canvas.print("ENT:Open/Play  SPC:PlayDir  ESC:Back");
  }

  void drawSettingsScreen(const UiSettings& cfg, const Theme& th) {
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextColor(th.primary, th.bg);
    canvas.setCursor(6, 20);
    canvas.print("SETTINGS  ;/.:Move ENT:Set ESC:Back");
    canvas.drawFastHLine(4, 30, SCREEN_W - 8, th.border);

    const char* labels[9] = {
      "Amp Boost",
      "Seek Step",
      "Theme",
      "Visualizer",
      "Font Size",
      "Brightness",
      "Screen Sleep",
      "CPU Eco Mode",
      "Rescan Folder"
    };

     char vals[9][24];
 snprintf(vals[0], 24, "%s", ampBoostName(cfg.ampBoost));
 snprintf(vals[1], 24, "%u sec", cfg.seekStepSec);
 snprintf(vals[2], 24, "%s", THEMES[cfg.themeIdx % THEME_COUNT].name);
 snprintf(vals[3], 24, "%s", visModeName(cfg.visMode));
 snprintf(vals[4], 24, "%s", fontProfileName(cfg.fontProfile));
 snprintf(vals[5], 24, "%u%%", (unsigned)((cfg.brightness * 100) / 255));
 if (cfg.screenTimeoutS == 0)      snprintf(vals[6], 24, "ALWAYS ON");
 else if (cfg.screenTimeoutS < 60) snprintf(vals[6], 24, "%u sec", cfg.screenTimeoutS);
 else                              snprintf(vals[6], 24, "%u min", cfg.screenTimeoutS / 60);
 snprintf(vals[7], 24, "%s", cfg.cpuPowerSaver ? "160MHz ECO" : "240MHz MAX");
 snprintf(vals[8], 24, "[ENTER]");

    for (int i = 0; i < 9; ++i) {
      int y = 32 + i * 11;
      bool sel = (i == settingsCursor);
      if (sel) {
        canvas.fillRoundRect(4, y, SCREEN_W - 8, 10, 2, th.selBg);
        canvas.setTextColor(th.selText, th.selBg);
      } else {
        canvas.setTextColor(th.text, th.bg);
      }
      canvas.setCursor(10, y + 1);
      canvas.print(labels[i]);
      canvas.setCursor(136, y + 1);
      canvas.print(vals[i]);
    }
  }

  void drawHelpScreen(const Theme& th) {
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextColor(th.primary, th.bg);
    canvas.setCursor(6, 19);
    canvas.print("HOTKEYS (ESC/I to close, ;/. scroll)");
    canvas.drawFastHLine(4, 29, SCREEN_W - 8, th.border);

    static const char* LINES[] = {
      "ENT / SPC : Play / Pause / Select",
      "; / .     : Up / Down in list",
      "N / B     : Next / Previous track",
      "/ / ,     : Seek +5s / -5s",
      "[ / ]     : Volume Down / Up",
      "A         : Amp Boost (100/150/200%)",
      "F         : Folder Browser (SD)",
      "S         : Temp Shuffle (keeps order)",
      "L         : Loop (NORM/1X/INF/ALL)",
      "R         : Reverse Order (FWD/REV)",
      "V         : Visualizer (BAR/PK/WV/OFF)",
      "T         : Switch Color Theme (6)",
      "G         : Font Size (S / M / L)",
      "M / ESC   : Settings Menu",
      "G0 (1x)   : Pocket Play / Pause",
      "G0 (2x/3x): Pocket Next / Prev"
    };
    constexpr int TOTAL = (int)(sizeof(LINES) / sizeof(LINES[0]));
    int start = constrain(helpScroll, 0, max(0, TOTAL - 9));
    for (int i = 0; i < 9 && (start + i) < TOTAL; ++i) {
      canvas.setTextColor((i & 1) ? th.text : th.secondary, th.bg);
      canvas.setCursor(8, 34 + i * 11);
      canvas.print(LINES[start + i]);
    }
  }

  void drawCenteredMsg(const char* l1, const char* l2, const Theme& th) {
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextColor(th.secondary, th.bg);
    canvas.setCursor(60, 56);
    canvas.print(l1);
    canvas.setTextColor(th.textDim, th.bg);
    canvas.setCursor(44, 72);
    canvas.print(l2);
  }
};

// ======= PART 4: MAIN LOOP & CONTROLS =======


static PlaylistManager playlist;
static AudioEngine     audioEngine;
static UiRenderer      ui;
static UiSettings      settings;
static Preferences     prefs;

static bool     sdReady       = false;
static bool     screenAsleep  = false;
static uint32_t lastInputMs   = 0;
static uint32_t lastFrameMs   = 0;
static uint32_t lastBatMs     = 0;
static int      batteryPct    = 100;

static bool     prefsDirty    = false;
static uint32_t prefsDirtyMs  = 0;

static uint8_t  g0ClickCount  = 0;
static uint32_t g0LastClickMs = 0;

static char     lastHeldChar   = 0;
static uint32_t keyHoldStartMs = 0;
static uint32_t keyLastRepMs   = 0;

static void markSettingsDirty() {
  prefsDirty   = true;
  prefsDirtyMs = millis();
}

static void loadSettings() {
  prefs.begin("hifi_adv", true);
  settings.themeIdx       = prefs.getUChar("theme", 0) % THEME_COUNT;
  settings.visMode        = (VisualizerMode)(prefs.getUChar("vis", VIS_PEAK) % VIS_COUNT);
  settings.fontProfile    = (FontProfile)(prefs.getUChar("font", FONT_NORMAL) % FONT_COUNT);
  settings.brightness     = constrain(prefs.getUChar("bright", 160), 40, 255);
  settings.screenTimeoutS = prefs.getUShort("sleep", 30);
  settings.cpuPowerSaver  = prefs.getBool("eco", false);
  settings.ampBoost       = (AmpBoost)(prefs.getUChar("boost", BOOST_100) % BOOST_COUNT);
  settings.seekStepSec    = (prefs.getUChar("seek", 5) == 10) ? 10 : 5;

  uint8_t savedVol        = constrain(prefs.getUChar("vol", VOL_DEFAULT), VOL_MIN, VOL_MAX);
  playlist.loopMode       = (LoopMode)(prefs.getUChar("loop", LOOP_ALL) % LOOP_COUNT);
  playlist.shuffleEnabled = prefs.getBool("shuf", false);
  playlist.reverseEnabled = prefs.getBool("rev", false);

  String lastDir = prefs.getString("dir", "/");
  strncpy(playlist.currentDir, lastDir.c_str(), MAX_PATH_LEN - 1);
  playlist.currentDir[MAX_PATH_LEN - 1] = '\0';
  prefs.end();

  audioEngine.state.volume = savedVol;
}

static void flushSettingsIfDirty() {
  if (!prefsDirty) return;
  if (millis() - prefsDirtyMs < 2000) return;

  prefs.begin("hifi_adv", false);
  prefs.putUChar("theme",  settings.themeIdx);
  prefs.putUChar("vis",    (uint8_t)settings.visMode);
  prefs.putUChar("font",   (uint8_t)settings.fontProfile);
  prefs.putUChar("bright", settings.brightness);
  prefs.putUShort("sleep", settings.screenTimeoutS);
  prefs.putBool("eco",     settings.cpuPowerSaver);
  prefs.putUChar("boost",  (uint8_t)settings.ampBoost);
  prefs.putUChar("seek",   settings.seekStepSec);
  prefs.putUChar("vol",    audioEngine.state.volume);
  prefs.putUChar("loop",   (uint8_t)playlist.loopMode);
  prefs.putBool("shuf",    playlist.shuffleEnabled);
  prefs.putBool("rev",     playlist.reverseEnabled);
  prefs.putString("dir",   playlist.currentDir);
  prefs.end();

  prefsDirty = false;
}

static SPIClass sdSPI(HSPI);
static bool mountSD() {
  sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, sdSPI, SD_SPI_FREQ)) {
    if (!SD.begin(PIN_SD_CS, sdSPI, 12000000)) {
      return false;
    }
  }
  return true;
}

static void startCurrentTrack() {
  char fullPath[MAX_PATH_LEN];
  if (playlist.getCurrentTrackPath(fullPath, sizeof(fullPath))) {
    audioEngine.playFile(fullPath);
    int16_t origIdx = playlist.currentTrackIndex();
    if (origIdx >= 0 && ui.screen == SCR_PLAYER) {
      ui.listCursor = origIdx;
    }
  }
}

static void playNextTrack() {
  if (playlist.nextTrack()) {
    startCurrentTrack();
  }
}

static void playPrevTrack() {
  if (playlist.prevTrack()) {
    startCurrentTrack();
  }
}

static void wakeScreen() {
  lastInputMs = millis();
  if (screenAsleep) {
    screenAsleep = false;
    if (getCpuFrequencyMhz() != 240) {
      setCpuFrequencyMhz(240);
    }
    M5Cardputer.Display.wakeup();
    M5Cardputer.Display.setBrightness(settings.brightness);
  }
}

static void checkPowerManagement() {
  if (settings.screenTimeoutS == 0 || screenAsleep) return;
  if (millis() - lastInputMs >= (uint32_t)settings.screenTimeoutS * 1000UL) {
    screenAsleep = true;
    M5Cardputer.Display.setBrightness(0);
    M5Cardputer.Display.sleep();
    if (settings.cpuPowerSaver) {
      setCpuFrequencyMhz(160);
    }
  }
}

static void handleCharAction(char rawKey) {
  char k = (char)tolower((unsigned char)rawKey);

  if (ui.screen == SCR_HELP) {
    if (k == ';' || k == 'w') {
      ui.helpScroll = max(0, (int)ui.helpScroll - 1);
    } else if (k == '.' || k == 's') {
      ui.helpScroll++;
    } else if (k == 'i' || k == '`' || k == 27) {
      ui.screen = SCR_PLAYER;
    }
    return;
  }

  if (ui.screen == SCR_SETTINGS) {
    if (k == ';') {
      ui.settingsCursor = (ui.settingsCursor + 8) % 9;
    } else if (k == '.') {
      ui.settingsCursor = (ui.settingsCursor + 1) % 9;
    } else if (k == '`' || k == 'm' || k == 27) {
      ui.screen = SCR_PLAYER;
    } else if (k == ' ' || k == '\n' || k == '\r') {
      switch (ui.settingsCursor) {
        case 0: // Amp Boost (100% / 150% / 200%)
          settings.ampBoost = (AmpBoost)((settings.ampBoost + 1) % BOOST_COUNT);
          audioEngine.setBoost(settings.ampBoost);
          break;
        case 1: // Seek Step (5s / 10s)
          settings.seekStepSec = (settings.seekStepSec == 5) ? 10 : 5;
          break;
        case 2: // Theme
          settings.themeIdx = (settings.themeIdx + 1) % THEME_COUNT;
          break;
        case 3: // Visualizer
          settings.visMode = (VisualizerMode)((settings.visMode + 1) % VIS_COUNT);
          break;
        case 4: // Font Size
          settings.fontProfile = (FontProfile)((settings.fontProfile + 1) % FONT_COUNT);
          break;
        case 5: // Brightness
  if (settings.brightness < 102)      settings.brightness = 102;
  else if (settings.brightness < 153) settings.brightness = 153;
  else if (settings.brightness < 204) settings.brightness = 204;
  else if (settings.brightness < 255) settings.brightness = 255;
  else                                settings.brightness = 51;
  M5Cardputer.Display.setBrightness(settings.brightness);
  break;
        case 6: // Screen Sleep
          if (settings.screenTimeoutS == 0) settings.screenTimeoutS = 15;
          else if (settings.screenTimeoutS == 15) settings.screenTimeoutS = 30;
          else if (settings.screenTimeoutS == 30) settings.screenTimeoutS = 60;
          else if (settings.screenTimeoutS == 60) settings.screenTimeoutS = 120;
          else settings.screenTimeoutS = 0;
          break;
        case 7: // CPU Eco
          settings.cpuPowerSaver = !settings.cpuPowerSaver;
          break;
        case 8: // Rescan current folder
          if (sdReady) {
            playlist.loadDirectory(playlist.currentDir);
            ui.listCursor = 0;
            ui.screen = SCR_PLAYER;
          }
          break;
      }
      markSettingsDirty();
    }
    return;
  }

  if (ui.screen == SCR_FOLDERS) {
    if (k == ';') {
      if (playlist.entryCount > 0) {
        ui.listCursor = (ui.listCursor - 1 + playlist.entryCount) % playlist.entryCount;
      }
    } else if (k == '.') {
      if (playlist.entryCount > 0) {
        ui.listCursor = (ui.listCursor + 1) % playlist.entryCount;
      }
    } else if (k == '`' || k == 'f' || k == 27) {
      ui.screen = SCR_PLAYER;
      int16_t cur = playlist.currentTrackIndex();
      ui.listCursor = (cur >= 0) ? cur : 0;
    } else if (k == ' ') {
      if (playlist.trackCount > 0) {
        playlist.selectTrackByOriginalIndex(playlist.reverseEnabled ? (playlist.trackCount - 1) : 0);
        startCurrentTrack();
        markSettingsDirty();
        ui.screen = SCR_PLAYER;
      }
    }
    return;
  }

  switch (k) {
    case ';': // Scroll track list up
      if (playlist.trackCount > 0) {
        ui.listCursor = (ui.listCursor - 1 + playlist.trackCount) % playlist.trackCount;
      }
      break;

    case '.': // Scroll track list down
      if (playlist.trackCount > 0) {
        ui.listCursor = (ui.listCursor + 1) % playlist.trackCount;
      }
      break;

    case ' ': // Play / Pause toggle
      if (audioEngine.state.isPlaying) {
        audioEngine.togglePause();
      } else if (playlist.trackCount > 0) {
        playlist.selectTrackByOriginalIndex(constrain(ui.listCursor, 0, (int)playlist.trackCount - 1));
        startCurrentTrack();
      }
      break;

    case 'n': // Next track
      playNextTrack();
      break;

    case 'b': // Previous track
      playPrevTrack();
      break;

    case '/': // Seek +5 sec
      audioEngine.seekRelative(+(int16_t)settings.seekStepSec);
      break;

    case ',': // Seek -5 sec
      audioEngine.seekRelative(-(int16_t)settings.seekStepSec);
      break;

    case '[':
    case '-': // Volume Down
      if (audioEngine.state.volume > VOL_MIN) {
        audioEngine.setVolume(audioEngine.state.volume - 1);
        markSettingsDirty();
      }
      break;

    case ']':
    case '=': // Volume Up
      if (audioEngine.state.volume < VOL_MAX) {
        audioEngine.setVolume(audioEngine.state.volume + 1);
        markSettingsDirty();
      }
      break;

    case 's': // Toggle TEMPORARY Non-Destructive Shuffle
      playlist.setShuffle(!playlist.shuffleEnabled);
      markSettingsDirty();
      break;

    case 'l': // Cycle Loop Mode: NORM -> RP1X -> RPINF -> RPALL
      playlist.setLoopMode((LoopMode)((playlist.loopMode + 1) % LOOP_COUNT));
      markSettingsDirty();
      break;

    case 'r': // Toggle Reverse Playback Order (FWD <-> REV)
      if (!sdReady) {
        sdReady = mountSD();
        if (sdReady) playlist.loadDirectory("/");
      } else {
        playlist.setReverse(!playlist.reverseEnabled);
        markSettingsDirty();
      }
      break;

    case 'f': // Open Folder Browser
      if (sdReady) {
        ui.screen = SCR_FOLDERS;
        ui.listCursor = 0;
      }
      break;

    case 'v': // Cycle Visualizer (BARS -> PEAK -> WAVE -> OFF)
      settings.visMode = (VisualizerMode)((settings.visMode + 1) % VIS_COUNT);
      markSettingsDirty();
      break;

    case 'a': // Quick toggle Amp Boost (100% -> 150% -> 200%)
      settings.ampBoost = (AmpBoost)((settings.ampBoost + 1) % BOOST_COUNT);
      audioEngine.setBoost(settings.ampBoost);
      markSettingsDirty();
      break;

    case 't': // Cycle Color Theme (6 themes)
      settings.themeIdx = (settings.themeIdx + 1) % THEME_COUNT;
      markSettingsDirty();
      break;

    case 'g': // Cycle Font Size (SMALL / MED / LARGE)
      settings.fontProfile = (FontProfile)((settings.fontProfile + 1) % FONT_COUNT);
      markSettingsDirty();
      break;

    case 'm':
    case '`': // Open Settings Menu
      ui.screen = SCR_SETTINGS;
      break;

    case 'i': // Open Help Popup
      ui.helpScroll = 0;
      ui.screen = SCR_HELP;
      break;
  }
}

static void handleEnterAction() {
  if (ui.screen == SCR_SETTINGS) {
    handleCharAction('\n');
    return;
  }
  if (ui.screen == SCR_HELP) {
    ui.screen = SCR_PLAYER;
    return;
  }
  if (ui.screen == SCR_FOLDERS) {
    if (playlist.entryCount == 0) return;
    int16_t idx = constrain(ui.listCursor, 0, (int)playlist.entryCount - 1);
    if (playlist.entries[idx].isDir) {
      const char* name = playlist.entryName((uint16_t)idx);
      if (strcmp(name, "..") == 0) {
        playlist.goParentDirectory();
      } else {
        char nextPath[MAX_PATH_LEN];
        playlist.buildFullPath(name, nextPath, sizeof(nextPath));
        playlist.loadDirectory(nextPath);
      }
      ui.listCursor = 0;
      markSettingsDirty();
    } else {
      for (uint16_t t = 0; t < playlist.trackCount; ++t) {
        if (playlist.trackEntryIdx[t] == (uint16_t)idx) {
          playlist.selectTrackByOriginalIndex(t);
          startCurrentTrack();
          ui.screen = SCR_PLAYER;
          ui.listCursor = t;
          markSettingsDirty();
          break;
        }
      }
    }
    return;
  }

  if (playlist.trackCount > 0) {
    int16_t sel = constrain(ui.listCursor, 0, (int)playlist.trackCount - 1);
    if (sel == playlist.currentTrackIndex() && audioEngine.state.isPlaying) {
      audioEngine.togglePause();
    } else {
      playlist.selectTrackByOriginalIndex((uint16_t)sel);
      startCurrentTrack();
    }
  }
}

static void handleBackspaceAction() {
  if (ui.screen == SCR_FOLDERS) {
    if (playlist.goParentDirectory()) {
      ui.listCursor = 0;
      markSettingsDirty();
    }
  } else if (ui.screen != SCR_PLAYER) {
    ui.screen = SCR_PLAYER;
  }
}

static void processInput() {
  M5Cardputer.update();

  if (M5Cardputer.BtnA.wasPressed()) {
    g0ClickCount++;
    g0LastClickMs = millis();
  }
  if (g0ClickCount > 0 && (millis() - g0LastClickMs >= POCKET_CLICK_WINDOW_MS)) {
    uint8_t clicks = g0ClickCount;
    g0ClickCount = 0;
    if (clicks == 1) {
      if (audioEngine.state.isPlaying) audioEngine.togglePause();
      else if (playlist.trackCount > 0) startCurrentTrack();
    } else if (clicks == 2) {
      playNextTrack();
    } else {
      playPrevTrack();
    }
  }

  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      bool wasAsleep = screenAsleep;
      wakeScreen();
      if (wasAsleep) {
        lastHeldChar = 0;
        return;
      }

      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
      if (status.enter) {
        handleEnterAction();
        lastHeldChar = 0;
      } else if (status.del) {
        handleBackspaceAction();
        lastHeldChar = 0;
      } else if (!status.word.empty()) {
        char c = status.word[0];
        handleCharAction(c);
        char lc = (char)tolower((unsigned char)c);
        if (lc == ';' || lc == '.' || lc == '[' || lc == ']' ||
          lc == '-' || lc == '=' || lc == '/' || lc == ',') {
          lastHeldChar   = c;
          keyHoldStartMs = millis();
          keyLastRepMs   = millis();
        } else {
          lastHeldChar = 0;
        }
      }
    } else {
      lastHeldChar = 0;
    }
  } else if (lastHeldChar != 0 && M5Cardputer.Keyboard.isPressed()) {
    uint32_t now = millis();
    if ((now - keyHoldStartMs >= KEY_REPEAT_DELAY_MS) &&
      (now - keyLastRepMs >= KEY_REPEAT_RATE_MS)) {
      keyLastRepMs = now;
      handleCharAction(lastHeldChar);
    }
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  loadSettings();
  M5Cardputer.Display.setBrightness(settings.brightness);

  ui.begin();

  for (int f = 0; f < 42; ++f) {
    ui.canvas.fillSprite(RGB565(8, 4, 0));
    for (int y = 0; y < 135; y += 3) {
      ui.canvas.drawFastHLine(0, y, 240, RGB565(22, 11, 0));
    }

    for (int i = 0; i < 16; ++i) {
      float wave = 0.5f + 0.5f * sinf(f * 0.24f + i * 0.45f);
      int bh = (int)(wave * (f < 10 ? f * 3.0f : 30.0f));
      int bx = 8 + i * 14;
      uint16_t col = (i < 5) ? RGB565(0, 240, 110) : (i < 11 ? RGB565(255, 200, 0) : RGB565(255, 70, 20));
      ui.canvas.fillRect(bx, 134 - bh, 10, bh, col);
      ui.canvas.fillRect(bx, 132 - bh, 10, 2, RGB565(255, 255, 200));
    }

    int lineW = min(200, f * 18);
    ui.canvas.drawFastHLine(120 - lineW / 2, 66, lineW, RGB565(255, 200, 0));

    ui.canvas.setFont(&fonts::Font4);
    ui.canvas.setTextSize(1);
    int gx = (f < 14 && (f % 3 == 0)) ? ((f & 1) ? 3 : -3) : 0;
    if (gx != 0) {
      ui.canvas.setTextColor(RGB565(255, 60, 0));
      ui.canvas.drawCenterString("MP3PLUS", 120 + gx, 26);
      ui.canvas.setTextColor(RGB565(0, 255, 160));
      ui.canvas.drawCenterString("MP3PLUS", 120 - gx, 26);
    }
    ui.canvas.setTextColor(RGB565(255, 245, 180));
    ui.canvas.drawCenterString("MP3PLUS", 120, 26);

    if (f >= 10) {
      ui.canvas.setFont(&fonts::Font0);
      ui.canvas.fillRoundRect(74, 52, 92, 11, 2, RGB565(255, 175, 0));
      ui.canvas.setTextColor(RGB565(12, 6, 0));
      ui.canvas.drawCenterString(M5.getBoard() == m5::board_t::board_M5CardputerADV ? "CARDPUTER ADV" : "CARDPUTER", 120, 54);

      ui.canvas.setTextColor(RGB565(80, 255, 140));
      ui.canvas.drawCenterString("4096 TRK * MP3 * M4A * FLAC", 120, 74);
    }

    int pw = (f * 156) / 41;
    ui.canvas.drawRoundRect(40, 90, 160, 7, 2, RGB565(180, 100, 0));
    if (pw > 0) {
      ui.canvas.fillRoundRect(42, 92, pw, 3, 1, RGB565(255, 215, 40));
      ui.canvas.fillCircle(42 + pw, 93, 2, RGB565(255, 255, 255));
    }

    ui.canvas.pushSprite(&M5Cardputer.Display, 0, 0);
    delay(24);
  }

  sdReady = mountSD();
  if (sdReady) {
    if (!playlist.loadDirectory(playlist.currentDir)) {
      playlist.loadDirectory("/");
    }
  }

  audioEngine.begin(audioEngine.state.volume, settings.ampBoost);

  batteryPct  = M5Cardputer.Power.getBatteryLevel();
  lastInputMs = millis();
}

void loop() {
  processInput();

  if (audioEngine.consumeEOF()) {
    if (playlist.advanceOnEOF()) {
      startCurrentTrack();
    }
  }

  uint32_t now = millis();
  if (now - lastBatMs >= 5000) {
    lastBatMs = now;
    batteryPct = M5Cardputer.Power.getBatteryLevel();
  }

  flushSettingsIfDirty();

  checkPowerManagement();

  if (!screenAsleep) {
    uint32_t frameInterval = (settings.visMode == VIS_OFF || !audioEngine.state.isPlaying)
                ? UI_FRAME_MS_IDLE : UI_FRAME_MS_ACTIVE;
    if (now - lastFrameMs >= frameInterval) {
      lastFrameMs = now;
      ui.updateVisualizer(audioEngine.state);
      ui.render(playlist, audioEngine.state, settings, batteryPct, sdReady);
    }
  }

  delay(6);
}

void audio_id3data(const char* info) {
  if (AudioEngine::instance) AudioEngine::instance->onMetadata(info);
}

void audio_eof_mp3(const char* info) {
  (void)info;
  if (AudioEngine::instance) AudioEngine::instance->onEOF();
}
void audio_info(const char* info) {
  (void)info;
}

void audio_process_i2s(uint32_t* sample, bool* continueI2S) {
  *continueI2S = true;
  if (!AudioEngine::instance || !AudioEngine::instance->state.isPlaying) return;

  static uint16_t dec = 0;
  static int32_t lp = 0, hp = 0;
  static uint32_t accB = 0, accM = 0, accT = 0, accL = 0, accR = 0;
  static float agcPeak = 4000.0f;

  if ((++dec & 3) != 0) return;

  int16_t l = (int16_t)(*sample & 0xFFFF);
  int16_t r = (int16_t)(*sample >> 16);
  int32_t mono = ((int32_t)l + (int32_t)r) >> 1;

  lp += (mono - lp) >> 4;
  int32_t high = mono - lp;
  hp += (high - hp) >> 2;
  int32_t treb = high - hp;

  accB += (uint32_t)abs(lp);
  accM += (uint32_t)abs(hp);
  accT += (uint32_t)abs(treb);
  accL += (uint32_t)abs(l);
  accR += (uint32_t)abs(r);

  if ((dec & 255) == 0) {
    auto& st = AudioEngine::instance->state;
    float curPeak = (float)max(max(accB, accM * 2U), accT * 3U);
    if (curPeak > agcPeak) agcPeak = agcPeak * 0.70f + curPeak * 0.30f;
    else                   agcPeak = max(600.0f, agcPeak * 0.96f);

    float volFactor = 0.52f + 0.42f * ((float)st.volume / (float)VOL_MAX);
    float gain = (127.0f * volFactor) / agcPeak;

    st.visBass   = (uint8_t)min(127.0f, (float)accB * gain * 1.15f);
    st.visMid    = (uint8_t)min(127.0f, (float)accM * gain * 2.10f);
    st.visTreble = (uint8_t)min(127.0f, (float)accT * gain * 3.20f);
    st.vuLeft    = (uint8_t)min(127.0f, (float)accL * gain * 1.10f);
    st.vuRight   = (uint8_t)min(127.0f, (float)accR * gain * 1.10f);
    accB = accM = accT = accL = accR = 
        0;
  }
    
}

// Interactive calibration for the 2 HX711 + load cells (left/right), XIAO ESP32-C3.
//
// Hardware reality (locked in, see the design notes "Measurement model"/M22): the platform
// physically rests on 4 corners, only these 2 cells are wired, the other 2 are dead but
// still load-bearing. So `T = L + R` is NOT position-independent -- there is no universal
// counts->grams scale, and the old "isolate each cell, trim until they agree" idea
// (W3-W4/A3) is retired. What this sketch is for now: (a) tare, (b) an optional rough
// shared scale so live numbers are human-readable, and (c) `rawT`, printed every line
// regardless of scale state, which is what zone calibration (the design notes W-c: a_res,
// a_pot, a_bask) is actually measured against -- take a known mass, place it at the real
// zone (reservoir/pot/basket), read `rawT` before and after, a_zone = raw delta / known
// grams. Per-cell scale precision genuinely doesn't matter for that ratio, since it's the
// same (l, r) scale factors on both sides of the delta.
//
// Library: bogde/HX711 -- tare()/set_scale()/get_value() already do the counts<->grams
// math, no custom calibration arithmetic needed here.
//
// Serial commands (115200 baud, newline-terminated):
//   z          tare both channels (empty platform first)
//   m <grams>  set the known reference mass, e.g. "m 177" for a bare iPhone 17
//   l          capture: known mass is currently on the platform -> sets left's rough scale
//   r          capture: known mass is currently on the platform -> sets right's rough scale
//   h          print this help
//   # <label>  print a clearly-delimited marker line, e.g. "# W15 reservoir +1000ml" --
//              use this every time you change what's on the platform, so a pasted or
//              saved log shows exactly which readings belong to which step
// With no command, prints live readings every 500ms: `t` (micros() timestamp, for rate/timing
// work per the design notes table F -- sample count is not a reliable clock, the HX711's RC
// oscillator drifts), raw counts per channel, `rawT` (raw L+R sum, always), and `T` in grams
// once both channels have a scale set. Marks are timestamped the same way.

#include <HX711.h>

struct Cell {
  const char *name;
  uint8_t dout;
  uint8_t sck;
  HX711 hx;
  bool scaled = false;
  double scaleFactor = 1.0;
};

Cell cells[] = {
  { "left ", 7, 21, HX711() },  // D5/D6
  { "right", 20, 10, HX711() }, // D7/D10
};
const int NUM_CELLS = sizeof(cells) / sizeof(cells[0]);
const int LEFT = 0, RIGHT = 1;

double knownMassG = 177.0; // bare iPhone 17, no case -- change with "m <grams>"

void printHelp() {
  Serial.println();
  Serial.println("=== Kahvibotti calibration (XIAO ESP32-C3) ===");
  Serial.printf("Known reference mass: %.2f g (change with \"m <grams>\")\n", knownMassG);
  Serial.println("Commands:");
  Serial.println("  z          tare both channels (empty platform)");
  Serial.println("  m <grams>  set known reference mass");
  Serial.println("  l          capture LEFT cell scale (mass over left cell now)");
  Serial.println("  r          capture RIGHT cell scale (mass over right cell now)");
  Serial.println("  h          this help");
  Serial.println("  # <label>  print a marker line, e.g. \"# W15 reservoir +1000ml\"");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  for (int i = 0; i < NUM_CELLS; i++) {
    cells[i].hx.begin(cells[i].dout, cells[i].sck);
    cells[i].hx.wait_ready_timeout(1000);
  }

  printHelp();
}

void handleLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  char cmd = line.charAt(0);
  switch (cmd) {
    case 'z': {
      for (int i = 0; i < NUM_CELLS; i++) cells[i].hx.tare(20);
      Serial.println("Tared both channels.");
      break;
    }
    case 'm': {
      double g = line.substring(1).toFloat();
      if (g > 0) {
        knownMassG = g;
        Serial.printf("Known mass set to %.2f g\n", knownMassG);
      } else {
        Serial.println("Usage: m <grams>, e.g. \"m 177\"");
      }
      break;
    }
    case 'l':
    case 'r': {
      int idx = (cmd == 'l') ? LEFT : RIGHT;
      double factor = cells[idx].hx.get_value(10) / knownMassG;
      cells[idx].hx.set_scale(factor);
      cells[idx].scaleFactor = factor;
      cells[idx].scaled = true;
      Serial.printf("%s scale set: %.4f counts/g (raw value %.1f / %.2f g)\n",
                    cells[idx].name, factor, factor * knownMassG, knownMassG);
      break;
    }
    case 'h':
      printHelp();
      break;
    case '#': {
      String label = line.substring(1);
      label.trim();
      Serial.println();
      Serial.printf("############ MARK t=%10lu us: %s ############\n", micros(), label.c_str());
      Serial.println();
      break;
    }
    default:
      Serial.println("Unknown command, 'h' for help.");
  }
}

String inputBuffer = "";

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      handleLine(inputBuffer);
      inputBuffer = "";
    } else if (c != '\r') {
      inputBuffer += c;
    }
  }

  Serial.printf("t=%10lu us  ", micros());
  long rawSum = 0;
  double gramSum = 0;
  bool allScaled = true;
  for (int i = 0; i < NUM_CELLS; i++) {
    long raw = cells[i].hx.get_value(5);
    rawSum += raw;
    if (cells[i].scaled) {
      double g = raw / cells[i].scaleFactor;
      gramSum += g;
      Serial.printf("%s: %8.1f g   ", cells[i].name, g);
    } else {
      allScaled = false;
      Serial.printf("%s: %8ld raw   ", cells[i].name, raw);
    }
  }
  Serial.printf(" rawT=%9ld", rawSum);
  if (allScaled) Serial.printf("  T=%8.1f g", gramSum);
  Serial.println();

  delay(500);
}

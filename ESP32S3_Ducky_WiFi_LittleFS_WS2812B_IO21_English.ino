/*
  ESP32-S3 DuckyScript 3.x Core Interpreter
  Framework: Arduino-ESP32
  USB: Native USB HID keyboard (TinyUSB)

  Supported core syntax:
    REM ...
    REM_BLOCK ... END_REM
    STRING ...
    STRINGLN ...
    DELAY <expr>
    DEFAULT_DELAY <expr>
    DEFAULTDELAY <expr>
    REPEAT <expr>
    VAR $NAME = <expr>
    $NAME = <expr>
    DEFINE #NAME <text>
    IF (<expr>) THEN / ELSE IF (<expr>) THEN / ELSE / END_IF
    WHILE (<expr>) / END_WHILE
    FUNCTION NAME() / END_FUNCTION / NAME()
    RETURN
    HOLD <key>
    RELEASE <key|ALL>
    INJECT_MOD <modifier>
    Standard key names and arbitrary key combinations
    RANDOM_LOWERCASE_LETTER
    RANDOM_UPPERCASE_LETTER
    RANDOM_LETTER
    RANDOM_NUMBER
    RANDOM_SPECIAL
    RANDOM_CHAR
    ATTACKMODE HID   (accepted as compatibility no-op)

  Expression operators:
    + - * / %
    == != > < >= <=
    && || !
    Parentheses
    TRUE / FALSE
    $variables
    $_RANDOM_INT using $_RANDOM_MIN / $_RANDOM_MAX

  Notes:
    - This is a source-code interpreter, not Hak5 inject.bin compatible.
    - Hak5 hardware-specific commands such as STORAGE attack mode, OS_DETECT,
      Keystroke Reflection extensions, EXFIL, button/LED commands, etc. are
      intentionally not emulated here.
*/

#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <FS.h>
#include <LittleFS.h>
#include "esp_system.h"
#include "esp32-hal-rgb-led.h"

#include <vector>
#include <map>

USBHIDKeyboard Keyboard;

// ------------------------------------------------------------
// Configuration
// ------------------------------------------------------------
static const uint32_t USB_ENUMERATION_DELAY_MS = 1500;
static const uint32_t KEY_TAP_MS = 8;
static const uint32_t MAX_EXECUTED_STEPS = 100000;

// One external WS2812B status LED connected to GPIO21.
// Red   = the current payload has not been executed.
// Green = manual/automatic execution has been requested or completed.
static const uint8_t STATUS_LED_PIN = 21;
static const uint8_t STATUS_LED_BRIGHTNESS = 48;  // 0..255
static const rgb_led_color_order_t STATUS_LED_COLOR_ORDER = LED_COLOR_ORDER_GRB;

// Wi-Fi access point.
// Change the password before distributing a product.
static const char *AP_BASE_NAME = "DSTIKE-DUCKY";
static const char *AP_PASSWORD  = "dstike123";  // Minimum 8 characters
static const uint8_t AP_CHANNEL = 6;
static const uint8_t AP_MAX_CLIENTS = 2;

// LittleFS payload paths.
static const char *PAYLOAD_PATH = "/payload.txt";
static const char *UPLOAD_TEMP_PATH = "/payload.upload";
static const char *PAYLOAD_BACKUP_PATH = "/payload.backup";

// The current interpreter loads the script into memory when it runs.
// Typical DuckyScript files are small; 128 KiB is a conservative limit.
static const size_t MAX_PAYLOAD_BYTES = 128 * 1024;

// Safer defaults: uploading never executes a payload automatically.
static const bool AUTO_RUN_AFTER_UPLOAD = false;
static const bool AUTO_RUN_ON_BOOT = false;


// ------------------------------------------------------------
// Utility
// ------------------------------------------------------------
static String trimCopy(String s) {
  s.trim();
  return s;
}

static bool startsWithWord(const String &line, const char *word) {
  String w(word);
  if (!line.startsWith(w)) return false;
  if (line.length() == w.length()) return true;
  char c = line[w.length()];
  return c == ' ' || c == '\t' || c == '(';
}

static String upperCopy(String s) {
  s.toUpperCase();
  return s;
}

static std::vector<String> splitLines(const String &src) {
  std::vector<String> out;
  int start = 0;
  while (start <= (int)src.length()) {
    int e = src.indexOf('\n', start);
    if (e < 0) e = src.length();
    String line = src.substring(start, e);
    if (line.endsWith("\r")) line.remove(line.length() - 1);
    out.push_back(line);
    if (e >= (int)src.length()) break;
    start = e + 1;
  }
  return out;
}

static std::vector<String> splitWs(const String &s) {
  std::vector<String> out;
  int i = 0;
  while (i < (int)s.length()) {
    while (i < (int)s.length() && isspace((unsigned char)s[i])) i++;
    if (i >= (int)s.length()) break;
    int j = i;
    while (j < (int)s.length() && !isspace((unsigned char)s[j])) j++;
    out.push_back(s.substring(i, j));
    i = j;
  }
  return out;
}

// ------------------------------------------------------------
// Ducky interpreter
// ------------------------------------------------------------
class DuckyInterpreter {
public:
  explicit DuckyInterpreter(USBHIDKeyboard &kbd) : keyboard(kbd) {}

  void run(const String &source) {
    lines = splitLines(source);
    preprocess();
    pc = 0;
    defaultDelay = 0;
    lastSimpleLine = "";
    executedSteps = 0;
    callStack.clear();

    Serial.printf("[Ducky] %u lines loaded\n", (unsigned)lines.size());

    while (pc >= 0 && pc < (int)lines.size()) {
      if (++executedSteps > MAX_EXECUTED_STEPS) {
        Serial.println("[Ducky] Aborted: execution step limit reached.");
        keyboard.releaseAll();
        return;
      }

      String raw = lines[pc];
      String line = trimCopy(raw);

      if (line.length() == 0) {
        pc++;
        continue;
      }

      // REM_BLOCK ... END_REM
      if (startsWithWord(upperCopy(line), "REM_BLOCK")) {
        int end = findForwardToken(pc, "END_REM");
        pc = (end >= 0) ? end + 1 : (int)lines.size();
        continue;
      }

      String u = upperCopy(line);

      if (startsWithWord(u, "REM") || u == "END_REM") {
        pc++;
        continue;
      }

      // STRING / STRINGLN block forms
      if (u == "STRING" || u == "STRINGLN") {
        bool withEnter = (u == "STRINGLN");
        int end = findForwardToken(pc, withEnter ? "END_STRINGLN" : "END_STRING");
        if (end < 0) {
          Serial.printf("[Ducky] Missing %s at line %d\n",
                        withEnter ? "END_STRINGLN" : "END_STRING", pc + 1);
          pc++;
          continue;
        }

        for (int i = pc + 1; i < end; ++i) {
          String text = lines[i];
          if (text.startsWith("\t")) text.remove(0, 1);
          typeText(interpolate(text));
          if (withEnter) tapKey(KEY_RETURN);
        }
        pc = end + 1;
        applyDefaultDelay();
        continue;
      }

      if (u == "END_STRING" || u == "END_STRINGLN") {
        pc++;
        continue;
      }

      // FUNCTION definition is skipped during normal flow.
      if (startsWithWord(u, "FUNCTION")) {
        int end = matchingFunctionEnd(pc);
        pc = (end >= 0) ? end + 1 : pc + 1;
        continue;
      }

      // IF chain
      if (startsWithWord(u, "IF")) {
        bool cond = eval(conditionFromIf(line)) != 0;
        if (cond) {
          pc++;
        } else {
          pc = selectElseBranch(pc);
        }
        continue;
      }

      // Reaching ELSE/ELSE IF means a previous branch was already taken.
      if (u == "ELSE" || startsWithWord(u, "ELSE IF")) {
        int end = matchingIfEnd(pc);
        pc = (end >= 0) ? end + 1 : pc + 1;
        continue;
      }

      if (u == "END_IF") {
        pc++;
        continue;
      }

      // WHILE
      if (startsWithWord(u, "WHILE")) {
        String expr = trimCopy(line.substring(5));
        if (eval(expr) != 0) {
          pc++;
        } else {
          int end = matchingWhileEnd(pc);
          pc = (end >= 0) ? end + 1 : pc + 1;
        }
        continue;
      }

      if (u == "END_WHILE") {
        int begin = matchingWhileStart(pc);
        pc = (begin >= 0) ? begin : pc + 1;
        continue;
      }

      // RETURN
      if (startsWithWord(u, "RETURN")) {
        if (!callStack.empty()) {
          pc = callStack.back();
          callStack.pop_back();
        } else {
          pc++;
        }
        continue;
      }

      if (u == "END_FUNCTION") {
        if (!callStack.empty()) {
          pc = callStack.back();
          callStack.pop_back();
        } else {
          pc++;
        }
        continue;
      }

      // Function call: NAME()
      if (isFunctionCall(line)) {
        String name = line.substring(0, line.length() - 2);
        name.trim();
        name.toUpperCase();
        auto it = functions.find(name);
        if (it != functions.end()) {
          callStack.push_back(pc + 1);
          pc = it->second.first + 1;
        } else {
          Serial.printf("[Ducky] Unknown function at line %d: %s\n", pc + 1, line.c_str());
          pc++;
        }
        continue;
      }

      // Variable declaration / assignment
      if (startsWithWord(u, "VAR")) {
        String body = trimCopy(line.substring(3));
        assignVariable(body);
        pc++;
        continue;
      }

      if (line.startsWith("$")) {
        int eq = line.indexOf('=');
        if (eq > 0) {
          assignVariable(line);
          pc++;
          continue;
        }
      }

      // DEFINE
      if (startsWithWord(u, "DEFINE")) {
        parseDefine(line.substring(6));
        pc++;
        continue;
      }

      // Everything below is a simple command and can be REPEATed.
      if (startsWithWord(u, "REPEAT")) {
        int count = eval(trimCopy(line.substring(6)));
        if (count < 0) count = 0;
        for (int i = 0; i < count; ++i) {
          if (lastSimpleLine.length()) executeSimple(lastSimpleLine, true);
        }
        pc++;
        continue;
      }

      lastSimpleLine = line;
      executeSimple(line, true);
      pc++;
    }

    keyboard.releaseAll();
    Serial.println("[Ducky] Done.");
  }

private:
  USBHIDKeyboard &keyboard;
  std::vector<String> lines;
  std::map<String, int32_t> vars;
  std::map<String, String> defines;
  std::map<String, std::pair<int, int>> functions;
  std::vector<int> callStack;

  int pc = 0;
  uint32_t defaultDelay = 0;
  String lastSimpleLine;
  uint32_t executedSteps = 0;

  // ----------------------------------------------------------
  // Preprocess functions
  // ----------------------------------------------------------
  void preprocess() {
    vars.clear();
    defines.clear();
    functions.clear();

    for (int i = 0; i < (int)lines.size(); ++i) {
      String line = trimCopy(lines[i]);
      String u = upperCopy(line);
      if (!startsWithWord(u, "FUNCTION")) continue;

      String name = trimCopy(line.substring(8));
      int p = name.indexOf('(');
      if (p >= 0) name = name.substring(0, p);
      name.trim();
      name.toUpperCase();

      int end = matchingFunctionEnd(i);
      if (end >= 0 && name.length()) {
        functions[name] = {i, end};
      }
    }

    vars["$_RANDOM_MIN"] = 0;
    vars["$_RANDOM_MAX"] = 65535;
  }

  // ----------------------------------------------------------
  // Simple command execution
  // ----------------------------------------------------------
  void executeSimple(const String &lineIn, bool applyDelay) {
    String line = trimCopy(lineIn);
    String u = upperCopy(line);

    if (startsWithWord(u, "DELAY")) {
      int32_t ms = eval(trimCopy(line.substring(5)));
      if (ms > 0) delay((uint32_t)ms);
      return;
    }

    if (startsWithWord(u, "DEFAULT_DELAY")) {
      int32_t ms = eval(trimCopy(line.substring(13)));
      defaultDelay = ms < 0 ? 0 : (uint32_t)ms;
      return;
    }

    if (startsWithWord(u, "DEFAULTDELAY")) {
      int32_t ms = eval(trimCopy(line.substring(12)));
      defaultDelay = ms < 0 ? 0 : (uint32_t)ms;
      return;
    }

    if (startsWithWord(u, "STRINGLN")) {
      String text = line.length() > 8 ? line.substring(8) : "";
      if (text.startsWith(" ") || text.startsWith("\t")) text.remove(0, 1);
      typeText(interpolate(text));
      tapKey(KEY_RETURN);
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (startsWithWord(u, "STRING")) {
      String text = line.length() > 6 ? line.substring(6) : "";
      if (text.startsWith(" ") || text.startsWith("\t")) text.remove(0, 1);
      typeText(interpolate(text));
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (startsWithWord(u, "INJECT_MOD")) {
      String keyName = trimCopy(line.substring(10));
      int key = keyCode(keyName);
      if (key >= 0) tapKey((uint8_t)key);
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (startsWithWord(u, "HOLD")) {
      String keyName = trimCopy(line.substring(4));
      int key = keyCode(keyName);
      if (key >= 0) keyboard.press((uint8_t)key);
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (startsWithWord(u, "RELEASE")) {
      String keyName = trimCopy(line.substring(7));
      if (upperCopy(keyName) == "ALL" || keyName.length() == 0) {
        keyboard.releaseAll();
      } else {
        int key = keyCode(keyName);
        if (key >= 0) keyboard.release((uint8_t)key);
      }
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (u == "RANDOM_LOWERCASE_LETTER") {
      keyboard.write((uint8_t)('a' + random(26)));
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (u == "RANDOM_UPPERCASE_LETTER") {
      keyboard.write((uint8_t)('A' + random(26)));
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (u == "RANDOM_LETTER") {
      char c = (random(2) == 0) ? ('a' + random(26)) : ('A' + random(26));
      keyboard.write((uint8_t)c);
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (u == "RANDOM_NUMBER") {
      keyboard.write((uint8_t)('0' + random(10)));
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (u == "RANDOM_SPECIAL") {
      const char *chars = "!@#$%^&*()";
      keyboard.write((uint8_t)chars[random(10)]);
      if (applyDelay) applyDefaultDelay();
      return;
    }

    if (u == "RANDOM_CHAR") {
      const char *chars =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "!@#$%^&*()";
      keyboard.write((uint8_t)chars[random(strlen(chars))]);
      if (applyDelay) applyDefaultDelay();
      return;
    }

    // ESP32 implementation always starts in HID mode.
    if (startsWithWord(u, "ATTACKMODE")) {
      if (u.indexOf("HID") < 0 || u.indexOf("STORAGE") >= 0) {
        Serial.printf("[Ducky] ATTACKMODE partially unsupported: %s\n", line.c_str());
      }
      return;
    }

    // Otherwise treat the entire line as a key combination.
    if (!executeKeyCombo(line)) {
      Serial.printf("[Ducky] Unsupported command at line %d: %s\n", pc + 1, line.c_str());
    } else if (applyDelay) {
      applyDefaultDelay();
    }
  }

  void applyDefaultDelay() {
    if (defaultDelay) delay(defaultDelay);
  }

  void typeText(const String &text) {
    // Character-by-character is a little more deterministic than one huge Print.
    for (size_t i = 0; i < text.length(); ++i) {
      keyboard.write((uint8_t)text[i]);
      delay(1);
    }
  }

  void tapKey(uint8_t key) {
    keyboard.press(key);
    delay(KEY_TAP_MS);
    keyboard.release(key);
  }

  bool executeKeyCombo(const String &line) {
    std::vector<String> toks = splitWs(line);
    if (toks.empty()) return false;

    std::vector<uint8_t> pressed;
    for (String tok : toks) {
      int code = keyCode(tok);
      if (code < 0) {
        keyboard.releaseAll();
        return false;
      }
      keyboard.press((uint8_t)code);
      pressed.push_back((uint8_t)code);
      delay(2);
    }

    delay(KEY_TAP_MS);
    keyboard.releaseAll();
    return true;
  }

  int keyCode(String token) {
    token.trim();
    String u = upperCopy(token);

    if (token.length() == 1) return (uint8_t)token[0];

    if (u == "CTRL" || u == "CONTROL") return KEY_LEFT_CTRL;
    if (u == "SHIFT") return KEY_LEFT_SHIFT;
    if (u == "ALT" || u == "OPTION") return KEY_LEFT_ALT;
    if (u == "GUI" || u == "WINDOWS" || u == "COMMAND") return KEY_LEFT_GUI;

    if (u == "RCTRL" || u == "RIGHTCTRL") return KEY_RIGHT_CTRL;
    if (u == "RSHIFT" || u == "RIGHTSHIFT") return KEY_RIGHT_SHIFT;
    if (u == "RALT" || u == "RIGHTALT") return KEY_RIGHT_ALT;
    if (u == "RGUI" || u == "RIGHTGUI") return KEY_RIGHT_GUI;

    if (u == "ENTER" || u == "RETURN") return KEY_RETURN;
    if (u == "ESC" || u == "ESCAPE") return KEY_ESC;
    if (u == "TAB") return KEY_TAB;
    if (u == "SPACE") return KEY_SPACE;
    if (u == "BACKSPACE") return KEY_BACKSPACE;
    if (u == "DELETE" || u == "DEL") return KEY_DELETE;
    if (u == "INSERT") return KEY_INSERT;
    if (u == "HOME") return KEY_HOME;
    if (u == "END") return KEY_END;
    if (u == "PAGEUP" || u == "PAGE_UP") return KEY_PAGE_UP;
    if (u == "PAGEDOWN" || u == "PAGE_DOWN") return KEY_PAGE_DOWN;

    if (u == "UP" || u == "UPARROW") return KEY_UP_ARROW;
    if (u == "DOWN" || u == "DOWNARROW") return KEY_DOWN_ARROW;
    if (u == "LEFT" || u == "LEFTARROW") return KEY_LEFT_ARROW;
    if (u == "RIGHT" || u == "RIGHTARROW") return KEY_RIGHT_ARROW;

    if (u == "PRINTSCREEN") return KEY_PRINT_SCREEN;
    if (u == "MENU" || u == "APP") return KEY_MENU;
    if (u == "PAUSE" || u == "BREAK") return KEY_PAUSE;
    if (u == "CAPSLOCK") return KEY_CAPS_LOCK;
    if (u == "NUMLOCK") return KEY_NUM_LOCK;
    if (u == "SCROLLLOCK" || u == "SCROLLOCK") return KEY_SCROLL_LOCK;

    if (u.length() >= 2 && u[0] == 'F') {
      int n = u.substring(1).toInt();
      switch (n) {
        case 1: return KEY_F1; case 2: return KEY_F2;
        case 3: return KEY_F3; case 4: return KEY_F4;
        case 5: return KEY_F5; case 6: return KEY_F6;
        case 7: return KEY_F7; case 8: return KEY_F8;
        case 9: return KEY_F9; case 10: return KEY_F10;
        case 11: return KEY_F11; case 12: return KEY_F12;
        case 13: return KEY_F13; case 14: return KEY_F14;
        case 15: return KEY_F15; case 16: return KEY_F16;
        case 17: return KEY_F17; case 18: return KEY_F18;
        case 19: return KEY_F19; case 20: return KEY_F20;
        case 21: return KEY_F21; case 22: return KEY_F22;
        case 23: return KEY_F23; case 24: return KEY_F24;
      }
    }

    return -1;
  }

  // ----------------------------------------------------------
  // Variables / DEFINE / interpolation
  // ----------------------------------------------------------
  void assignVariable(String body) {
    int eq = body.indexOf('=');
    if (eq < 0) return;

    String name = trimCopy(body.substring(0, eq));
    String expr = trimCopy(body.substring(eq + 1));
    name.toUpperCase();

    if (!name.startsWith("$")) {
      Serial.printf("[Ducky] Invalid variable name: %s\n", name.c_str());
      return;
    }

    vars[name] = eval(expr);
  }

  void parseDefine(String body) {
    body.trim();
    int sp = -1;
    for (int i = 0; i < (int)body.length(); ++i) {
      if (isspace((unsigned char)body[i]) || body[i] == '=') {
        sp = i;
        break;
      }
    }
    if (sp < 0) return;

    String name = trimCopy(body.substring(0, sp));
    String value = trimCopy(body.substring(sp + 1));
    if (value.startsWith("=")) value = trimCopy(value.substring(1));
    name.toUpperCase();

    if (!name.startsWith("#")) return;
    defines[name] = value;
  }

  String interpolate(const String &src) {
    String out;
    for (int i = 0; i < (int)src.length();) {
      if (src[i] == '$' || src[i] == '#') {
        char prefix = src[i];
        int j = i + 1;
        while (j < (int)src.length()) {
          char c = src[j];
          if (!(isalnum((unsigned char)c) || c == '_')) break;
          j++;
        }
        String name = src.substring(i, j);
        name.toUpperCase();

        if (prefix == '$') {
          out += String(getVariable(name));
        } else {
          auto it = defines.find(name);
          if (it != defines.end()) out += it->second;
          else out += src.substring(i, j);
        }
        i = j;
      } else {
        out += src[i++];
      }
    }
    return out;
  }

  int32_t getVariable(String name) {
    name.toUpperCase();

    if (name == "$_RANDOM_INT") {
      int32_t lo = vars["$_RANDOM_MIN"];
      int32_t hi = vars["$_RANDOM_MAX"];
      if (hi < lo) {
        int32_t t = lo; lo = hi; hi = t;
      }
      if (hi == lo) return lo;
      // Arduino random upper bound is exclusive.
      return random(lo, hi + 1);
    }

    auto it = vars.find(name);
    return (it == vars.end()) ? 0 : it->second;
  }

  // ----------------------------------------------------------
  // IF / WHILE / FUNCTION block matching
  // ----------------------------------------------------------
  int matchingIfEnd(int from) const {
    int depth = 0;
    for (int i = from + 1; i < (int)lines.size(); ++i) {
      String u = upperCopy(trimCopy(lines[i]));
      if (startsWithWord(u, "IF")) depth++;
      else if (u == "END_IF") {
        if (depth == 0) return i;
        depth--;
      }
    }
    return -1;
  }

  int selectElseBranch(int ifPc) {
    int depth = 0;
    for (int i = ifPc + 1; i < (int)lines.size(); ++i) {
      String line = trimCopy(lines[i]);
      String u = upperCopy(line);

      if (startsWithWord(u, "IF")) {
        depth++;
        continue;
      }

      if (u == "END_IF") {
        if (depth == 0) return i + 1;
        depth--;
        continue;
      }

      if (depth != 0) continue;

      if (startsWithWord(u, "ELSE IF")) {
        String expr = trimCopy(line.substring(7));
        int thenPos = upperCopy(expr).lastIndexOf(" THEN");
        if (thenPos >= 0) expr = trimCopy(expr.substring(0, thenPos));
        if (eval(expr) != 0) return i + 1;
      } else if (u == "ELSE") {
        return i + 1;
      }
    }
    return (int)lines.size();
  }

  String conditionFromIf(String line) {
    String expr = trimCopy(line.substring(2));
    String ue = upperCopy(expr);
    int thenPos = ue.lastIndexOf(" THEN");
    if (thenPos >= 0) expr = trimCopy(expr.substring(0, thenPos));
    return expr;
  }

  int matchingWhileEnd(int from) const {
    int depth = 0;
    for (int i = from + 1; i < (int)lines.size(); ++i) {
      String u = upperCopy(trimCopy(lines[i]));
      if (startsWithWord(u, "WHILE")) depth++;
      else if (u == "END_WHILE") {
        if (depth == 0) return i;
        depth--;
      }
    }
    return -1;
  }

  int matchingWhileStart(int from) const {
    int depth = 0;
    for (int i = from - 1; i >= 0; --i) {
      String u = upperCopy(trimCopy(lines[i]));
      if (u == "END_WHILE") depth++;
      else if (startsWithWord(u, "WHILE")) {
        if (depth == 0) return i;
        depth--;
      }
    }
    return -1;
  }

  int matchingFunctionEnd(int from) const {
    int depth = 0;
    for (int i = from + 1; i < (int)lines.size(); ++i) {
      String u = upperCopy(trimCopy(lines[i]));
      if (startsWithWord(u, "FUNCTION")) depth++;
      else if (u == "END_FUNCTION") {
        if (depth == 0) return i;
        depth--;
      }
    }
    return -1;
  }

  int findForwardToken(int from, const char *token) const {
    String target(token);
    for (int i = from + 1; i < (int)lines.size(); ++i) {
      if (upperCopy(trimCopy(lines[i])) == target) return i;
    }
    return -1;
  }

  bool isFunctionCall(const String &line) const {
    if (!line.endsWith("()")) return false;
    int p = line.indexOf(' ');
    return p < 0;
  }

  // ----------------------------------------------------------
  // Integer / boolean expression parser
  // ----------------------------------------------------------
  class ExprParser {
  public:
    ExprParser(DuckyInterpreter &owner, String text) : o(owner), s(text) {}

    int32_t parse() {
      pos = 0;
      int32_t v = parseOr();
      return v;
    }

  private:
    DuckyInterpreter &o;
    String s;
    int pos = 0;

    void ws() {
      while (pos < (int)s.length() && isspace((unsigned char)s[pos])) pos++;
    }

    bool eat(const char *op) {
      ws();
      int n = strlen(op);
      if (s.substring(pos, pos + n) == op) {
        pos += n;
        return true;
      }
      return false;
    }

    int32_t parseOr() {
      int32_t v = parseAnd();
      while (true) {
        if (eat("||")) {
          int32_t rhs = parseAnd();
          v = (v || rhs) ? 1 : 0;
        } else break;
      }
      return v;
    }

    int32_t parseAnd() {
      int32_t v = parseEq();
      while (true) {
        if (eat("&&")) {
          int32_t rhs = parseEq();
          v = (v && rhs) ? 1 : 0;
        } else break;
      }
      return v;
    }

    int32_t parseEq() {
      int32_t v = parseRel();
      while (true) {
        if (eat("==")) v = (v == parseRel()) ? 1 : 0;
        else if (eat("!=")) v = (v != parseRel()) ? 1 : 0;
        else break;
      }
      return v;
    }

    int32_t parseRel() {
      int32_t v = parseAdd();
      while (true) {
        if (eat(">=")) v = (v >= parseAdd()) ? 1 : 0;
        else if (eat("<=")) v = (v <= parseAdd()) ? 1 : 0;
        else if (eat(">")) v = (v > parseAdd()) ? 1 : 0;
        else if (eat("<")) v = (v < parseAdd()) ? 1 : 0;
        else break;
      }
      return v;
    }

    int32_t parseAdd() {
      int32_t v = parseMul();
      while (true) {
        if (eat("+")) v += parseMul();
        else if (eat("-")) v -= parseMul();
        else break;
      }
      return v;
    }

    int32_t parseMul() {
      int32_t v = parseUnary();
      while (true) {
        if (eat("*")) v *= parseUnary();
        else if (eat("/")) {
          int32_t d = parseUnary();
          v = d == 0 ? 0 : v / d;
        } else if (eat("%")) {
          int32_t d = parseUnary();
          v = d == 0 ? 0 : v % d;
        } else break;
      }
      return v;
    }

    int32_t parseUnary() {
      ws();
      if (eat("!")) return !parseUnary();
      if (eat("-")) return -parseUnary();
      if (eat("+")) return parseUnary();
      return parsePrimary();
    }

    int32_t parsePrimary() {
      ws();

      if (eat("(")) {
        int32_t v = parseOr();
        eat(")");
        return v;
      }

      if (pos >= (int)s.length()) return 0;

      if (s[pos] == '$') {
        int start = pos++;
        while (pos < (int)s.length()) {
          char c = s[pos];
          if (!(isalnum((unsigned char)c) || c == '_')) break;
          pos++;
        }
        String name = s.substring(start, pos);
        return o.getVariable(name);
      }

      if (s[pos] == '#') {
        int start = pos++;
        while (pos < (int)s.length()) {
          char c = s[pos];
          if (!(isalnum((unsigned char)c) || c == '_')) break;
          pos++;
        }
        String name = s.substring(start, pos);
        name.toUpperCase();
        auto it = o.defines.find(name);
        if (it == o.defines.end()) return 0;
        String val = trimCopy(it->second);
        String uv = upperCopy(val);
        if (uv == "TRUE") return 1;
        if (uv == "FALSE") return 0;
        return val.toInt();
      }

      if (isalpha((unsigned char)s[pos]) || s[pos] == '_') {
        int start = pos++;
        while (pos < (int)s.length()) {
          char c = s[pos];
          if (!(isalnum((unsigned char)c) || c == '_')) break;
          pos++;
        }
        String word = upperCopy(s.substring(start, pos));
        if (word == "TRUE") return 1;
        if (word == "FALSE") return 0;
        return 0;
      }

      bool hex = false;
      int start = pos;
      if (s.substring(pos, pos + 2) == "0x" || s.substring(pos, pos + 2) == "0X") {
        hex = true;
        pos += 2;
        while (pos < (int)s.length() && isxdigit((unsigned char)s[pos])) pos++;
      } else {
        while (pos < (int)s.length() && isdigit((unsigned char)s[pos])) pos++;
      }

      String num = s.substring(start, pos);
      if (hex) return (int32_t)strtol(num.c_str(), nullptr, 16);
      return (int32_t)strtol(num.c_str(), nullptr, 10);
    }
  };

  int32_t eval(String expr) {
    expr.trim();
    ExprParser p(*this, expr);
    return p.parse();
  }
};

// ------------------------------------------------------------
// Wi-Fi + LittleFS web file manager
// ------------------------------------------------------------
DuckyInterpreter ducky(Keyboard);
WebServer server(80);
DNSServer dnsServer;

IPAddress apIp(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

String apSsid;
String lastStatus = "System starting";
File uploadFile;

bool uploadInProgress = false;
bool uploadSucceeded = false;
bool uploadFailed = false;
size_t uploadBytes = 0;
String uploadError;

bool runRequested = false;
bool payloadRunning = false;
uint32_t runRequestedAt = 0;

// This state intentionally resets to red after reboot or after a new upload.
bool payloadExecutionTriggered = false;

static void setStatusLedColor(uint8_t red, uint8_t green, uint8_t blue) {
  rgbLedWriteOrdered(
    STATUS_LED_PIN,
    STATUS_LED_COLOR_ORDER,
    red,
    green,
    blue
  );
}

static void showPayloadNotExecuted() {
  payloadExecutionTriggered = false;
  setStatusLedColor(STATUS_LED_BRIGHTNESS, 0, 0);
}

static void showPayloadExecuted() {
  payloadExecutionTriggered = true;
  setStatusLedColor(0, STATUS_LED_BRIGHTNESS, 0);
}

static String htmlEscape(String text) {
  text.replace("&", "&amp;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  text.replace("\"", "&quot;");
  text.replace("'", "&#39;");
  return text;
}

static String formatBytes(size_t value) {
  if (value >= 1024UL * 1024UL) {
    return String((double)value / (1024.0 * 1024.0), 2) + " MB";
  }
  if (value >= 1024UL) {
    return String((double)value / 1024.0, 1) + " KB";
  }
  return String(value) + " B";
}

static bool payloadExists() {
  File file = LittleFS.open(PAYLOAD_PATH, FILE_READ);
  if (!file) return false;
  bool valid = !file.isDirectory();
  file.close();
  return valid;
}

static size_t payloadSize() {
  File file = LittleFS.open(PAYLOAD_PATH, FILE_READ);
  if (!file) return 0;
  size_t size = file.size();
  file.close();
  return size;
}

static String makePage(const String &title, const String &body) {
  String html;
  html.reserve(5000 + body.length());

  html += F("<!doctype html><html lang='en'><head>");
  html += F("<meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<meta http-equiv='Cache-Control' content='no-store'>");
  html += F("<title>");
  html += htmlEscape(title);
  html += F("</title><style>");
  html += F(
    "body{margin:0;background:#0d1117;color:#e6edf3;font-family:Arial,"
    "sans-serif}.wrap{max-width:760px;margin:0 auto;padding:24px}"
    ".card{background:#161b22;border:1px solid #30363d;border-radius:14px;"
    "padding:20px;margin:16px 0}.status{padding:12px;border-radius:9px;"
    "background:#0b2e1b;border:1px solid #238636;word-break:break-word}"
    "h1{font-size:25px;margin:0 0 8px}h2{font-size:18px;margin-top:0}"
    "p{line-height:1.6}.muted{color:#8b949e}.grid{display:grid;"
    "grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}"
    ".item{padding:12px;background:#0d1117;border-radius:9px}"
    "button,.button{display:inline-block;border:0;border-radius:8px;padding:11px 16px;"
    "font-weight:700;cursor:pointer;text-decoration:none;margin:4px 4px 4px 0}"
    ".primary{background:#238636;color:white}.secondary{background:#1f6feb;color:white}"
    ".danger{background:#da3633;color:white}.neutral{background:#30363d;color:white}"
    "input[type=file]{display:block;width:100%;box-sizing:border-box;padding:13px;"
    "margin:12px 0;background:#0d1117;color:#e6edf3;border:1px solid #30363d;"
    "border-radius:8px}code{background:#0d1117;padding:2px 6px;border-radius:5px}"
    "form{display:inline}.upload-form{display:block}.warning{border-color:#9e6a03;"
    "background:#2d2208}"
  );
  html += F("</style></head><body><div class='wrap'>");
  html += F("<h1>ESP32-S3 Wi-Fi Payload Manager</h1>");
  html += F("<div class='muted'>For use only on systems you own or are explicitly authorized to test.</div>");
  html += body;
  html += F("</div></body></html>");
  return html;
}

static void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
}

static void sendHome() {
  const bool exists = payloadExists();
  const size_t size = exists ? payloadSize() : 0;
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();

  String body;
  body.reserve(4200);

  body += F("<div class='card'><div class='status'>");
  body += htmlEscape(lastStatus);
  body += F("</div></div>");

  body += F("<div class='card'><h2>Connection Information</h2><div class='grid'>");
  body += F("<div class='item'><b>Wi-Fi</b><br>");
  body += htmlEscape(apSsid);
  body += F("</div><div class='item'><b>Management Address</b><br><code>http://");
  body += WiFi.softAPIP().toString();
  body += F("</code></div><div class='item'><b>Connected Clients</b><br>");
  body += String(WiFi.softAPgetStationNum());
  body += F("</div><div class='item'><b>WS2812B Status</b><br>");
  body += payloadExecutionTriggered
    ? String("Green: execution triggered")
    : String("Red: not executed");
  body += F("</div></div></div>");

  body += F("<div class='card'><h2>LittleFS Storage</h2><div class='grid'>");
  body += F("<div class='item'><b>Total Capacity</b><br>");
  body += formatBytes(total);
  body += F("</div><div class='item'><b>Used Space</b><br>");
  body += formatBytes(used);
  body += F("</div><div class='item'><b>payload.txt</b><br>");
  body += exists ? formatBytes(size) : String("Not uploaded");
  body += F("</div></div></div>");

  body += F("<div class='card'><h2>Upload or Replace payload.txt</h2>");
  body += F("<p class='muted'>The file is written to Flash in chunks instead of being buffered entirely in RAM. ");
  body += F("It is always saved as <code>/payload.txt</code>. Maximum size: ");
  body += formatBytes(MAX_PAYLOAD_BYTES);
  body += F(".</p>");
  body += F("<form class='upload-form' method='POST' action='/upload' ");
  body += F("enctype='multipart/form-data'>");
  body += F("<input type='file' name='payload' accept='.txt,text/plain' required>");
  body += F("<button class='primary' type='submit'>Upload payload.txt</button></form></div>");

  body += F("<div class='card'><h2>Payload Actions</h2>");
  if (exists) {
    body += F("<form method='POST' action='/run'>");
    body += F("<button class='secondary' type='submit'>Run Manually</button></form>");
    body += F("<a class='button neutral' href='/view'>View</a>");
    body += F("<a class='button neutral' href='/download'>Download</a>");
    body += F("<form method='POST' action='/delete' ");
    body += F("onsubmit=\"return confirm('Delete payload.txt?')\">");
    body += F("<button class='danger' type='submit'>Delete</button></form>");
  } else {
    body += F("<p class='muted'>Upload a payload before running it.</p>");
  }
  body += F("</div>");

  body += F("<div class='card warning'><h2>Execution Behavior</h2>");
  body += F("<p>Payloads do not run automatically after upload by default. After you click Run Manually, this page may be temporarily unavailable while the script is running.</p>");
  body += F("<p class='muted'>To run automatically after upload, set ");
  body += F("<code>AUTO_RUN_AFTER_UPLOAD</code> to <code>true</code> in the source code.</p></div>");

  sendNoCacheHeaders();
  server.send(200, "text/html; charset=utf-8", makePage("Payload Manager", body));
}

static void sendActionPage(const String &title, const String &message) {
  String body;
  body.reserve(1000);
  body += F("<div class='card'><h2>");
  body += htmlEscape(title);
  body += F("</h2><p>");
  body += htmlEscape(message);
  body += F("</p><a class='button secondary' href='/'>Back to Payload Manager</a></div>");
  sendNoCacheHeaders();
  server.send(200, "text/html; charset=utf-8", makePage(title, body));
}

static bool installUploadedFile(String &error) {
  const bool hadOldPayload = payloadExists();

  LittleFS.remove(PAYLOAD_BACKUP_PATH);

  if (hadOldPayload && !LittleFS.rename(PAYLOAD_PATH, PAYLOAD_BACKUP_PATH)) {
    error = "Could not back up the existing payload.txt file";
    return false;
  }

  if (!LittleFS.rename(UPLOAD_TEMP_PATH, PAYLOAD_PATH)) {
    if (hadOldPayload) {
      LittleFS.rename(PAYLOAD_BACKUP_PATH, PAYLOAD_PATH);
    }
    error = "Could not install the temporary upload as payload.txt";
    return false;
  }

  LittleFS.remove(PAYLOAD_BACKUP_PATH);
  return true;
}

static void resetUploadState() {
  if (uploadFile) uploadFile.close();
  uploadInProgress = false;
  uploadSucceeded = false;
  uploadFailed = false;
  uploadBytes = 0;
  uploadError = "";
}

static void failUpload(const String &reason) {
  uploadFailed = true;
  uploadError = reason;
  if (uploadFile) uploadFile.close();
  LittleFS.remove(UPLOAD_TEMP_PATH);
}

static void handleUploadData() {
  HTTPUpload &upload = server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
      resetUploadState();
      uploadInProgress = true;
      LittleFS.remove(UPLOAD_TEMP_PATH);

      uploadFile = LittleFS.open(UPLOAD_TEMP_PATH, FILE_WRITE);
      if (!uploadFile) {
        failUpload("Could not create a temporary file in LittleFS");
      }
      break;

    case UPLOAD_FILE_WRITE:
      if (!uploadInProgress || uploadFailed) break;

      if (uploadBytes + upload.currentSize > MAX_PAYLOAD_BYTES) {
        failUpload("The file exceeds the maximum allowed size");
        break;
      }

      if (!uploadFile) {
        failUpload("The upload file handle is invalid");
        break;
      }

      {
        size_t written = uploadFile.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
          failUpload("An error occurred while writing to Flash");
          break;
        }
        uploadBytes += written;
      }
      break;

    case UPLOAD_FILE_END:
      if (uploadFile) {
        uploadFile.flush();
        uploadFile.close();
      }

      if (!uploadFailed) {
        if (uploadBytes == 0) {
          failUpload("Empty files are not accepted");
        } else {
          String error;
          if (installUploadedFile(error)) {
            uploadSucceeded = true;
            showPayloadNotExecuted();
            lastStatus = "Upload successful: /payload.txt, size " + formatBytes(uploadBytes);
            if (AUTO_RUN_AFTER_UPLOAD) {
              showPayloadExecuted();
              runRequested = true;
              runRequestedAt = millis();
            }
          } else {
            failUpload(error);
          }
        }
      }

      uploadInProgress = false;
      break;

    case UPLOAD_FILE_ABORTED:
      failUpload("The upload was aborted by the client");
      uploadInProgress = false;
      break;
  }
}

static void handleUploadFinished() {
  if (uploadSucceeded) {
    sendActionPage(
      "Upload Complete",
      "payload.txt has been saved to LittleFS in the internal Flash."
    );
  } else {
    String message = uploadError.length() ? uploadError : "No valid file was received";
    lastStatus = "Upload failed: " + message;
    sendActionPage("Upload Failed", message);
  }
}

static void handleViewPayload() {
  File file = LittleFS.open(PAYLOAD_PATH, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain; charset=utf-8", "payload.txt not found");
    return;
  }

  sendNoCacheHeaders();
  server.streamFile(file, "text/plain; charset=utf-8");
  file.close();
}

static void handleDownloadPayload() {
  File file = LittleFS.open(PAYLOAD_PATH, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain; charset=utf-8", "payload.txt not found");
    return;
  }

  sendNoCacheHeaders();
  server.sendHeader("Content-Disposition", "attachment; filename=\"payload.txt\"");
  server.streamFile(file, "text/plain; charset=utf-8");
  file.close();
}

static void handleRunPayload() {
  if (payloadRunning || runRequested) {
    sendActionPage("Device Busy", "A payload is already running or waiting to run.");
    return;
  }

  if (!payloadExists()) {
    server.send(404, "text/plain; charset=utf-8", "payload.txt not found");
    return;
  }

  showPayloadExecuted();
  runRequested = true;
  runRequestedAt = millis();
  lastStatus = "payload.txt has been queued. The WS2812B is now green.";
  sendActionPage(
    "Execution Queued",
    "The device will run payload.txt after the web response has been sent. This page may be temporarily unavailable during execution."
  );
}

static void handleDeletePayload() {
  if (payloadRunning || uploadInProgress) {
    sendActionPage("Device Busy", "A payload is running or a file upload is in progress. Deletion is temporarily unavailable.");
    return;
  }

  bool removed = LittleFS.remove(PAYLOAD_PATH);
  LittleFS.remove(PAYLOAD_BACKUP_PATH);
  LittleFS.remove(UPLOAD_TEMP_PATH);
  showPayloadNotExecuted();

  if (removed) {
    lastStatus = "payload.txt has been deleted";
    sendActionPage("Delete Complete", "payload.txt has been removed from LittleFS.");
  } else {
    sendActionPage("Delete Failed", "payload.txt was not found, or the file system could not delete it.");
  }
}

static void recoverPayloadFiles() {
  LittleFS.remove(UPLOAD_TEMP_PATH);

  const bool payloadOk = payloadExists();
  File backup = LittleFS.open(PAYLOAD_BACKUP_PATH, FILE_READ);
  const bool backupExists = (bool)backup && !backup.isDirectory();
  if (backup) backup.close();

  if (!payloadOk && backupExists) {
    LittleFS.rename(PAYLOAD_BACKUP_PATH, PAYLOAD_PATH);
  } else if (backupExists) {
    LittleFS.remove(PAYLOAD_BACKUP_PATH);
  }
}

static bool loadPayload(String &source, String &error) {
  File file = LittleFS.open(PAYLOAD_PATH, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    error = "Could not open /payload.txt";
    return false;
  }

  const size_t size = file.size();
  if (size == 0) {
    file.close();
    error = "payload.txt is empty";
    return false;
  }

  if (size > MAX_PAYLOAD_BYTES) {
    file.close();
    error = "payload.txt exceeds the execution size limit";
    return false;
  }

  source = "";
  if (!source.reserve(size + 1)) {
    file.close();
    error = "Not enough memory to load payload.txt";
    return false;
  }

  uint8_t buffer[512];
  while (file.available()) {
    size_t count = file.read(buffer, sizeof(buffer));
    if (count == 0) break;

    if (!source.concat(reinterpret_cast<const char *>(buffer), count)) {
      file.close();
      source = "";
      error = "Not enough memory while loading payload.txt";
      return false;
    }
  }
  file.close();

  if (source.length() != size) {
    source = "";
    error = "payload.txt could not be read completely";
    return false;
  }

  // Remove an optional UTF-8 BOM produced by some Windows editors.
  if (source.length() >= 3 &&
      static_cast<uint8_t>(source[0]) == 0xEF &&
      static_cast<uint8_t>(source[1]) == 0xBB &&
      static_cast<uint8_t>(source[2]) == 0xBF) {
    source.remove(0, 3);
  }

  return true;
}

static void executeStoredPayload() {
  payloadRunning = true;
  runRequested = false;

  String source;
  String error;

  if (!loadPayload(source, error)) {
    showPayloadNotExecuted();
    lastStatus = "Execution failed: " + error;
    payloadRunning = false;
    return;
  }

  lastStatus = "Executing /payload.txt";
  Serial.printf("[Web] Executing %s (%u bytes)\n",
                PAYLOAD_PATH, static_cast<unsigned>(source.length()));

  Keyboard.releaseAll();
  delay(250);
  ducky.run(source);
  Keyboard.releaseAll();

  source = "";
  lastStatus = "payload.txt execution completed";
  payloadRunning = false;
}

static void startAccessPoint() {
  uint64_t id = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lX",
           static_cast<unsigned long>(id & 0xFFFFFFULL));
  apSsid = AP_BASE_NAME;
  apSsid += "-";
  apSsid += suffix;

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(apIp, apGateway, apSubnet);

  bool started = WiFi.softAP(
    apSsid.c_str(),
    AP_PASSWORD,
    AP_CHANNEL,
    false,
    AP_MAX_CLIENTS
  );

  if (!started) {
    lastStatus = "Wi-Fi access point failed to start";
    Serial.println("[WiFi] softAP failed");
    return;
  }

  dnsServer.start(53, "*", WiFi.softAPIP());

  Serial.printf("[WiFi] SSID: %s\n", apSsid.c_str());
  Serial.printf("[WiFi] URL : http://%s/\n",
                WiFi.softAPIP().toString().c_str());
}

static void startWebServer() {
  server.on("/", HTTP_GET, sendHome);
  server.on("/upload", HTTP_POST, handleUploadFinished, handleUploadData);
  server.on("/view", HTTP_GET, handleViewPayload);
  server.on("/download", HTTP_GET, handleDownloadPayload);
  server.on("/run", HTTP_POST, handleRunPayload);
  server.on("/delete", HTTP_POST, handleDeletePayload);

  // Common captive-portal probes.
  server.on("/generate_204", HTTP_GET, sendHome);
  server.on("/hotspot-detect.html", HTTP_GET, sendHome);
  server.on("/connecttest.txt", HTTP_GET, sendHome);
  server.on("/ncsi.txt", HTTP_GET, sendHome);

  server.onNotFound([]() {
    if (server.method() == HTTP_GET) {
      server.sendHeader("Location",
                        String("http://") + WiFi.softAPIP().toString() + "/",
                        true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain; charset=utf-8", "Not found");
    }
  });

  server.begin();
  Serial.println("[Web] HTTP server started");
}

// ------------------------------------------------------------
// Arduino app
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Every reboot starts in the not-executed state.
  showPayloadNotExecuted();

  // Native USB HID. Connect the host to ESP32-S3 USB D-/D+,
  // not only to a separate CH340/CP210x UART connector.
  Keyboard.begin(KeyboardLayout_en_US);
  USB.begin();
  delay(USB_ENUMERATION_DELAY_MS);

  randomSeed(static_cast<uint32_t>(esp_random()));

  // formatOnFail=true formats only if mounting fails.
  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    lastStatus = "LittleFS mount failed. Check the 16 MB Flash setting and partitions.csv.";
    Serial.println("[LittleFS] mount failed");
  } else {
    recoverPayloadFiles();
    lastStatus = payloadExists()
      ? "LittleFS mounted. /payload.txt was found."
      : "LittleFS mounted. Upload payload.txt to begin.";

    Serial.printf("[LittleFS] total=%u used=%u\n",
                  static_cast<unsigned>(LittleFS.totalBytes()),
                  static_cast<unsigned>(LittleFS.usedBytes()));
  }

  startAccessPoint();
  startWebServer();

  if (AUTO_RUN_ON_BOOT && payloadExists()) {
    showPayloadExecuted();
    runRequested = true;
    runRequestedAt = millis();
  }
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // Leave enough time for the HTTP response to reach the browser.
  if (runRequested && !payloadRunning &&
      static_cast<uint32_t>(millis() - runRequestedAt) >= 300) {
    executeStoredPayload();
  }

  delay(2);
}

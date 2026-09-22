#include <ArduinoJson.h>
#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <NativeTextEngine.h>
#include <SdCardFontSystem.h>
#include <network/FontWebApi.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
void reply(const FontWebApi::Response& response) {
  JsonDocument envelope;
  envelope["status"] = response.status;
  envelope["body"] = response.body;
  serializeJson(envelope, std::cout);
  std::cout << '\n' << std::flush;
}
int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}
}  // namespace

// Line-delimited JSON is transport only. All font responses and filesystem
// behavior come from the same FontWebApi used by CrossPointWebServer.
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: NativeFontWebHost <sd-root>\n";
    return 2;
  }
  std::filesystem::create_directories(argv[1]);
  Storage.setRoot(std::filesystem::absolute(argv[1]));
  NativeTextEngine engine;
  if (engine.initialize() != TextStatus::Ok) return 3;
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.begin();
  renderer.setNativeTextEngine(&engine);
  SETTINGS.sdFontFamilyName[0] = 0;
  sdFontSystem.begin(renderer);
  {
    FontWebApi api(sdFontSystem);
    std::string line;
    while (std::getline(std::cin, line)) {
      JsonDocument command;
      if (deserializeJson(command, line)) {
        reply({400, "{\"error\":\"Invalid transport command\"}"});
        continue;
      }
      const std::string op = command["op"] | "";
      if (op == "list") {
        reply(api.list());
      } else if (op == "delete") {
        reply(api.remove(command["body"].as<std::string>()));
      } else if (op == "finish") {
        reply(api.finishUpload());
      } else if (op == "abort") {
        api.abortUpload();
        reply(api.finishUpload());
      } else {
        bool accepted = false;
        if (op == "begin")
          accepted = api.beginUpload(command["manifest"].as<std::string>());
        else if (op == "file")
          accepted = api.beginFile(command["name"].as<std::string>());
        else if (op == "end")
          accepted = api.endFile();
        else if (op == "chunk") {
          const std::string hex = command["hex"] | "";
          std::vector<uint8_t> bytes(hex.size() / 2);
          bool valid = hex.size() % 2 == 0 && bytes.size() <= 65536;
          for (size_t i = 0; valid && i < bytes.size(); ++i) {
            const int hi = hexDigit(hex[2 * i]), lo = hexDigit(hex[2 * i + 1]);
            valid = hi >= 0 && lo >= 0;
            if (valid) bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
          }
          if (valid)
            accepted = api.writeChunk(bytes.data(), bytes.size());
          else
            api.abortUpload();
        } else {
          reply({400, "{\"error\":\"Unknown transport command\"}"});
          continue;
        }
        reply({200, accepted ? "{\"accepted\":true}" : "{\"accepted\":false}"});
      }
    }
    api.abortUpload();
  }
  sdFontSystem.releaseNativeFonts();
  renderer.setNativeTextEngine(nullptr);
  engine.shutdown();
  return 0;
}

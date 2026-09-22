#include "clinesting/demo_cli.hpp"

#include "clinesting/demo_setup.hpp"
#include "clinesting/model.hpp"

#include <cmath>
#include <exception>
#include <stdexcept>
#include <string>

namespace clinesting {

namespace {

// Validate a bounded positive integer demo option.
int parsePositiveValue(std::string_view raw, const char* flagName) {
  try {
    const int value = std::stoi(std::string(raw));
    if (value <= 0) {
      throw std::invalid_argument("non-positive");
    }
    return value;
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(flagName) + " must be a positive integer");
  }
}

// Validate a positive finite numeric demo option.
double parsePositiveDouble(std::string_view raw, const char* flagName) {
  try {
    const double value = std::stod(std::string(raw));
    if (!(value > 0.0) || !std::isfinite(value)) {
      throw std::invalid_argument("non-positive");
    }
    return value;
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(flagName) + " must be a positive number");
  }
}

}  // namespace

// Validate and translate demonstration command-line options.
DemoCliOptions parseDemoCliOptions(const std::vector<std::string_view>& args) {
  DemoCliOptions options{
      kDefaultDemoPartCount, defaultWorkerCount(), 1.0, 1, false, std::nullopt, false};

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "--help") {
      options.showHelp = true;
      return options;
    }
    if (arg == "--count") {
      if (i + 1 >= args.size()) {
        throw std::invalid_argument("Missing value for --count");
      }
      options.count = parsePositiveValue(args[++i], "--count");
      continue;
    }
    if (arg == "--threads") {
      if (i + 1 >= args.size()) {
        throw std::invalid_argument("Missing value for --threads");
      }
      options.threads = parsePositiveValue(args[++i], "--threads");
      continue;
    }
    if (arg == "--output") {
      if (i + 1 >= args.size()) {
        throw std::invalid_argument("Missing value for --output");
      }
      if (options.outputPath.has_value()) {
        throw std::invalid_argument("--output provided more than once");
      }
      options.outputPath = std::filesystem::path(args[++i]);
      continue;
    }
    if (arg == "--bitmap-resolution") {
      if (i + 1 >= args.size()) {
        throw std::invalid_argument("Missing value for --bitmap-resolution");
      }
      options.bitmapResolutionMm = parsePositiveDouble(args[++i], "--bitmap-resolution");
      continue;
    }
    if (arg == "--bitmap-step") {
      if (i + 1 >= args.size()) {
        throw std::invalid_argument("Missing value for --bitmap-step");
      }
      options.bitmapSearchStepPx = parsePositiveValue(args[++i], "--bitmap-step");
      continue;
    }
    if (arg == "--debug-placement") {
      options.debugPlacement = true;
      continue;
    }

    throw std::invalid_argument("Unknown argument: " + std::string(arg));
  }

  options.threads = normalizeWorkerCount(options.threads);
  return options;
}

}  // namespace clinesting

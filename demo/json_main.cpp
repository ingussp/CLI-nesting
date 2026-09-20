#include "clinesting/json_io.hpp"
#include "clinesting/dxf_export.hpp"
#include "clinesting/gpu_bitmap.hpp"
#include "clinesting/continuous_nesting.hpp"
#include "continuous_results.hpp"
#include <atomic>
#include <csignal>
#include <iostream>
#include <optional>
#include <cctype>
#ifdef _WIN32
#include <shellapi.h>
#endif

namespace {
std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);
#ifdef _WIN32
HANDLE sessionFinished=nullptr;
// Request cooperative cancellation when the console is interrupted.
BOOL WINAPI stopHandler(DWORD event) {
  if(event!=CTRL_C_EVENT && event!=CTRL_BREAK_EVENT && event!=CTRL_CLOSE_EVENT) return FALSE;
  stopRequested.store(true,std::memory_order_relaxed);
  // Windows gives console-close handlers a limited grace period. The main thread
  // can publish a final validated candidate while this handler keeps the process alive.
  if(event==CTRL_CLOSE_EVENT && sessionFinished) WaitForSingleObject(sessionFinished,4000);
  return TRUE;
}
#else
// Request cooperative cancellation when the console is interrupted.
void stopHandler(int) { stopRequested.store(true,std::memory_order_relaxed); }
#endif
// Keep process stop handlers active during nesting and output publication.
struct Signals {
  // Initialize cooperative console stop handling.
  Signals() {
#ifdef _WIN32
    sessionFinished=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!sessionFinished || !SetConsoleCtrlHandler(stopHandler,TRUE)) throw std::runtime_error("Cannot install stop handler");
#else
    std::signal(SIGINT,stopHandler); std::signal(SIGTERM,stopHandler);
#endif
  }
  // Notify the console-close handler that the session has finished.
  ~Signals() {
#ifdef _WIN32
    // The OS closes this process-lifetime handle; do not close it while a close
    // handler might still be waiting on it.
    SetEvent(sessionFinished);
#endif
  }
};
// Read the process-wide cooperative stop request.
bool stopped() { return stopRequested.load(std::memory_order_relaxed); }
// Open the saved SVG with the platform's associated viewer.
void preview(const std::filesystem::path& path) {
#ifdef _WIN32
  if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
    std::cerr<<"Could not open preview automatically. Open the saved SVG manually.\n";
#else
  std::cout<<"Preview: "<<path.string()<<'\n';
#endif
}
// Adapt nesting events to the current command-line workflow.
class Sink:public clinesting::EventSink {
 public:
  // Receive the start-of-job notification and input counts.
  void onTestStart(const std::vector<clinesting::Polygon>& sheets,const std::vector<clinesting::Polygon>& parts,
                   const clinesting::Config&,int) override {
    std::cout<<"Nesting "<<parts.size()<<" parts on "<<sheets.size()<<" available sheets...\n"<<std::flush;
  }
  // Receive the current nesting progress notification.
  void onProgress(int,double) override {}
  // Receive the completed placement result.
  void onResult(const clinesting::PlacementResult&) override {}
};
// Parse a bounded positive integer command-line argument.
int positive(const std::string& s,int max,const char* name) {
  size_t used=0; const int n=std::stoi(s,&used);
  if(used!=s.size() || n<1 || n>max) throw std::invalid_argument(std::string("Invalid ")+name);
  return n;
}
// Reject input and output paths that refer to the same file.
void validatePaths(const std::vector<std::filesystem::path>& paths) {
  for(size_t i=0;i<paths.size();++i) for(size_t j=0;j<i;++j) {
    const auto a=clinesting::cli::resolvePath(paths[i]),b=clinesting::cli::resolvePath(paths[j]);
    if((clinesting::cli::within(a,b) && clinesting::cli::within(b,a)) ||
       (std::filesystem::exists(a) && std::filesystem::exists(b) && std::filesystem::equivalent(a,b)))
      throw std::invalid_argument("Input, JSON, DXF and SVG must be different files");
  }
}
}

// Parse command-line arguments and run the selected nesting workflow.
int main(int argc,char** argv) {
  try {
    namespace fs=std::filesystem;
    fs::path input="input.json";
    std::optional<fs::path> outputOverride,dxfOverride;
    std::optional<int> threads,trials,rotations;
    for(int i=1;i<argc;++i) {
      const std::string arg=argv[i];
      if(arg=="--list-gpus") {
        const auto devices=clinesting::listGpuDevices();
        for(const auto& d:devices) std::cout<<d.index<<": "<<d.name<<" ("<<d.vendor<<", "<<d.memoryBytes/(1024*1024)<<" MiB)\n";
        if(devices.empty()) std::cout<<"No available OpenCL GPU found.\n";
        return 0;
      }
      if(arg=="--help" || arg=="-h") {
        std::cout<<"Usage: clinesting [--input input.json]\n"
          <<"All settings belong in input.json: config.mode=first|timed|continuous, config.gpu, output.json/dxf/svg/openPreview.\n"
          <<"first: one complete greedy layout. timed: optimize until timeLimitSeconds expires.\n"
          <<"continuous: optimize until Ctrl+C or console close; clear results next to input and save resultN files.\n"
          <<"JSON output paths are relative to input.json. --list-gpus lists OpenCL devices.\n"
          <<"Legacy overrides: --output result.json --dxf result.dxf --threads N --trials 1..4 --rotations 1..3600.\n";
        return 0;
      }
      if(i+1>=argc) throw std::invalid_argument("Missing value for "+arg);
      const std::string value=argv[++i];
      if(arg=="--input") input=fs::u8path(value);
      else if(arg=="--output") outputOverride=fs::u8path(value);
      else if(arg=="--dxf") dxfOverride=fs::u8path(value);
      else if(arg=="--threads") threads=positive(value,256,"threads");
      else if(arg=="--trials") trials=positive(value,4,"trials");
      else if(arg=="--rotations") rotations=positive(value,clinesting::Config::maxRotations,"rotations");
      else throw std::invalid_argument("Unknown argument: "+arg);
    }
    const auto absoluteInput=fs::weakly_canonical(input);
    const auto base=absoluteInput.parent_path();
    auto request=clinesting::readNestingJson(absoluteInput);
    if(threads) request.config.threads=*threads;
    if(trials) request.config.bitmapTrials=*trials;
    if(rotations) request.config.rotations=*rotations;
    const auto resolve=[&](const std::string& value) { return base/fs::u8path(value); };
    fs::path output=outputOverride.value_or(resolve(request.output.json));
    std::optional<fs::path> dxf=dxfOverride,svg;
    if(!dxf && !request.output.dxf.empty()) dxf=resolve(request.output.dxf);
    if(!request.output.svg.empty()) svg=resolve(request.output.svg);
    auto extension=output.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c) { return char(std::tolower(c)); });
    if(extension==".dxf") {
      if(dxfOverride && clinesting::cli::resolvePath(*dxfOverride)!=clinesting::cli::resolvePath(output))
        throw std::invalid_argument("Conflicting DXF paths");
      dxf=output; output.replace_extension(".json");
    }
    std::vector<fs::path> paths{absoluteInput,output};
    if(dxf) paths.push_back(*dxf);
    if(svg) paths.push_back(*svg);
    validatePaths(paths);
    Signals signals;
    if(request.config.mode==clinesting::SearchMode::Continuous) {
      clinesting::cli::ResultDirectory results(base,absoluteInput);
      std::cout<<"Continuous search: cleared "<<results.path().string()<<"\n"
               <<"Press Ctrl+C or close this console to stop. Improving layouts are saved immediately.\n"
               <<"Restart budget: "<<request.config.continuousRoundSeconds<<" seconds; no overall time limit.\n"<<std::flush;
      size_t saved=0;
      clinesting::runContinuousNesting(request,stopped,[&](const auto& candidate,const auto& result,size_t sequence) {
        results.save(sequence,candidate,result,dxf.has_value(),svg.has_value());
        saved=sequence;
        const auto quality=clinesting::layoutQuality(candidate.sheets,result.placement,result.bitmapStats);
        std::cout<<"Saved result"<<sequence<<".json"<<(dxf ? " + DXF" : "")<<(svg ? " + SVG" : "")
          <<"; placed: "<<candidate.individual.placement.size()-quality.unplaced
          <<"; sheet waste: "<<quality.usedSheetWasteArea<<" mm2; compact waste: "<<quality.compactWasteArea
          <<" mm2; restart: "<<candidate.config.searchIteration<<"\n"<<std::flush;
        if(sequence==1 && request.output.openPreview) preview(results.path()/"result1.svg");
      });
      std::cout<<"Stopped by user. Saved "<<saved<<" improving layouts in "<<results.path().string()<<"\n";
      return 0;
    }
    clinesting::OrchestratorRunStats result;
    if(request.config.mode==clinesting::SearchMode::Timed) {
      std::cout<<"Optimizing for "<<request.config.timeLimitSeconds<<" seconds...\n"<<std::flush;
      result=clinesting::runTimedNesting(request,stopped);
    } else {
      auto first=request;
      first.config.bitmapTrials=1;
      first.config.timeLimitSeconds=0;
      first.config.stopRequested=stopped;
      Sink sink;
      clinesting::BackgroundOrchestrator orchestrator;
      result=orchestrator.runWithStats(first,sink);
    }
    for(size_t i=1;i<paths.size();++i) {
      const auto parent=fs::absolute(paths[i]).parent_path();
      fs::create_directories(parent);
    }
    clinesting::writeNestingJson(output,request,result);
    if(dxf) clinesting::exportPlacementResultToDxf(*dxf,request.sheets,request.individual.placement,result.placement);
    if(svg) clinesting::cli::exportSvg(*svg,request.sheets,request.individual.placement,result.placement);
    std::cout<<"Placed: "<<request.individual.placement.size()-result.placement.unplaced.size()
      <<", unplaced: "<<result.placement.unplaced.size()<<", utilisation: "<<result.placement.utilisation<<"%\n"
      <<"Time: "<<result.timings.totalMs<<" ms; JSON: "<<fs::absolute(output).string()<<"\n";
    if(request.config.gpuEnabled) std::cout<<"OpenCL GPU: "<<result.bitmapStats.gpuDevice
      <<"; batches: "<<result.bitmapStats.gpuBatches<<"; fallback: "<<result.bitmapStats.gpuFallbackReason<<"\n";
    if(dxf) std::cout<<"DXF: "<<fs::absolute(*dxf).string()<<"\n";
    if(svg) std::cout<<"SVG: "<<fs::absolute(*svg).string()<<"\n";
    if(result.bitmapStats.timeLimitReached) std::cout<<"Time limit reached. Saved the best validated layout found; inspect unplacedCount.\n";
    if(result.bitmapStats.cancelled) std::cout<<"Stopped by user. Saved the available validated layout.\n";
    if(request.output.openPreview) preview(fs::absolute(*svg));
    return 0;
  } catch(const std::exception& e) { std::cerr<<"Error: "<<e.what()<<"\n"; return 1; }
}


#include "latest_json.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <future>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace clinesting;
using namespace std::chrono_literals;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}

int main(int argc,char** argv) {
  // Use a caller-provided build directory, never a production results directory.
  if(argc!=2) return 2;
  const fs::path root=fs::path(argv[1])/"latest-json-test";
  require(!fs::exists(root),"Test directory already exists");
  fs::create_directories(root);
  const auto output=root/"result.json";
  try {
    auto request=parseNestingJson(R"({"units":"mm","config":{"mode":"first","gpu":false},
      "sheets":[{"points":[[0,0],[100,0],[100,100],[0,100]]}],
      "parts":[{"id":"panel","points":[[0,0],[10,0],[10,10],[0,10]],"quantity":1}],
      "output":{"json":"result.json"}})");
    OrchestratorRunStats result;result.placement.unplaced=request.individual.placement;
    std::ofstream(output)<<"{\"previous\":true}";
#ifdef _WIN32
    auto handle=CreateFileW(output.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                 nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Cannot lock test JSON");
    try {
#endif
      cli::LatestJsonPublisher writer(output,request);
      result.searchIteration=1;writer.submit(request,result);
      const auto deadline=std::chrono::steady_clock::now()+3s;
      auto temporary=output;temporary+=".tmp";
#ifdef _WIN32
      while(!fs::exists(temporary) && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
      require(fs::exists(temporary),"Writer never prepared a snapshot");
#endif
      const auto started=std::chrono::steady_clock::now();
      for(int iteration=2;iteration<=200;++iteration) {
        result.searchIteration=iteration;writer.submit(request,result);
      }
      require(std::chrono::steady_clock::now()-started<1s,"Submitting snapshots blocked on a reader");
      auto finished=std::async(std::launch::async,[&] {writer.finish();});
#ifdef _WIN32
      const auto status=finished.wait_for(100ms);
      std::string previous;std::getline(std::ifstream(output),previous);
      CloseHandle(handle);
      handle=INVALID_HANDLE_VALUE;
      require(status==std::future_status::timeout,"Writer did not wait for the reader");
      require(previous=="{\"previous\":true}","An incomplete or outdated replacement became visible");
#endif
      finished.get();
      nlohmann::json latest;std::ifstream(output)>>latest;
      require(latest["searchIteration"]==200,"Final JSON was not the newest pending snapshot");
      require(!fs::exists(temporary),"Temporary JSON leaked after publication");
#ifdef _WIN32
    } catch(...) {if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);throw;}
#endif
    // Unwinding/destruction must cancel a waiting writer rather than hang.
#ifdef _WIN32
    const auto second=CreateFileW(output.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                 nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(second!=INVALID_HANDLE_VALUE,"Cannot relock test JSON");
    const auto started=std::chrono::steady_clock::now();
    {cli::LatestJsonPublisher writer(output,request);writer.submit(request,result);std::this_thread::sleep_for(50ms);}
    CloseHandle(second);
    require(std::chrono::steady_clock::now()-started<1s,"Destructor hung on a locked file");
#endif
  } catch(...) {fs::remove_all(root);throw;}
  fs::remove_all(root);
  std::cout<<"Latest snapshots coalesce, publication waits, and cleanup joins the writer.\n";
}

#include "Module.h"
#include <core/ProcessInfo.h>
#include <interfaces/IMemory.h>
#include <interfaces/IResourceMonitor.h>
#include <sstream>
#include <vector>

using std::endl;
using std::cerr; // TODO: temp
using std::list;
using std::stringstream;
using std::vector;

// TODO: don't create our own thread, use threadpool from WPEFramework

namespace WPEFramework {
namespace Plugin {
   // TODO: read name via link from "/usr/bin/WPEFramework" ?
   const string g_parentProcessName = _T("WPEFramework-1.0.0");

   class ResourceMonitorImplementation : public Exchange::IResourceMonitor {
  private:
      class Config : public Core::JSON::Container {
      public:
         enum class CollectMode {
            Single,    // Expect one process that can come and go
            Multiple,  // Expect several processes with the same name
            Callsign,  // Log Thunder process by callsign
            ClassName, // Log Thunder process by class name
            Invalid
         };

     private:
         Config& operator=(const Config&) = delete;
         
     public:
         Config()
             : Core::JSON::Container()
             , Path()
             , Interval()
             , Mode()
             , ParentName()
         {
            Add(_T("path"), &Path);
            Add(_T("interval"), &Interval);
            Add(_T("mode"), &Mode);
            Add(_T("parent-name"), &ParentName);
         }
         Config(const Config& copy)
             : Core::JSON::Container()
             , Path(copy.Path)
             , Interval(copy.Interval)
             , Mode(copy.Mode)
             , ParentName(copy.ParentName)
         {
            Add(_T("path"), &Path);
            Add(_T("interval"), &Interval);
            Add(_T("mode"), &Mode);
            Add(_T("parent-name"), &ParentName);
         }
         ~Config()
         {
         }
         
         CollectMode GetCollectMode() const
         {
             if (Mode == "single") {
                return CollectMode::Single;
             } else if(Mode == "multiple") {
                return CollectMode::Multiple;
             } else if (Mode == "callsign") {
                return CollectMode::Callsign;
             } else if (Mode == "classname") {
                return CollectMode::ClassName;
             }

             // TODO: no assert, should be logged
             ASSERT(!"Not a valid collect mode!");
             return CollectMode::Invalid;
         }

     public:
         Core::JSON::String Path;
         Core::JSON::DecUInt32 Interval;
         Core::JSON::String Mode;
         Core::JSON::String ParentName;
      };

      class ProcessThread : public Core::Thread {
     public:
         explicit ProcessThread(const Config& config)
             : _binFile(nullptr)
             , _otherMap(nullptr)
             , _ourMap(nullptr)
             , _bufferEntries(0)
             , _interval(0)
             , _collectMode(Config::CollectMode::Invalid)
         {
            _binFile = fopen(config.Path.Value().c_str(), "w");

            uint32_t pageCount = Core::SystemInfo::Instance().GetPhysicalPageCount();
            const uint32_t bitPersUint32 = 32;
            _bufferEntries = pageCount / bitPersUint32;
            if ((pageCount % bitPersUint32) != 0) {
               _bufferEntries++;
            }

            // Because linux doesn't report the first couple of pages it uses itself,
            //    allocate a little extra to make sure we don't miss the highest ones.
            _bufferEntries += _bufferEntries / 10;

            _ourMap = new uint32_t[_bufferEntries];
            _otherMap = new uint32_t[_bufferEntries];
            _interval = config.Interval.Value();
            _collectMode = config.GetCollectMode();
            _parentName = config.ParentName.Value();
         }

         ~ProcessThread()
         {
            Core::Thread::Stop();
            Core::Thread::Wait(STOPPED, Core::infinite);

            fclose(_binFile);

            delete [] _ourMap;
            delete [] _otherMap;
         }

         void GetProcessNames(vector<string>& processNames)
         {
            _namesLock.Lock();
            processNames = _processNames;
            _namesLock.Unlock();
         }

      private:
         // TODO: combine these "Collect*" methods
         void CollectSingle()
         {
            list<Core::ProcessInfo> processes;
            Core::ProcessInfo::FindByName(_parentName, false, processes);

            // TODO: check if only one, warning otherwise?

            uint32_t mapBufferSize = sizeof(_ourMap[0]) * _bufferEntries;
            memset(_ourMap, 0, mapBufferSize);
            memset(_otherMap, 0, mapBufferSize);

            vector<uint32_t> processIds;

            for (const Core::ProcessInfo& processInfo : processes) {
               string processName = processInfo.Name();

               _namesLock.Lock();
               if (find(_processNames.begin(), _processNames.end(), _parentName) == _processNames.end()) {
                  _processNames.push_back(_parentName);
               }
               _namesLock.Unlock();

               Core::ProcessTree processTree(processInfo.Id());

               processTree.MarkOccupiedPages(_ourMap, mapBufferSize);

               for (uint32_t pid : processTree.GetProcessIds()) {
                  processIds.push_back(pid);
               }
            }

            memset(_otherMap, 0, mapBufferSize);

            // Find other processes
            list<Core::ProcessInfo> otherProcesses;
            Core::ProcessInfo::Iterator otherIterator;
            while (otherIterator.Next()) {
               uint32_t otherId = otherIterator.Current().Id();
               if (find(processIds.begin(), processIds.end(), otherId) == processIds.end()) {
                  otherIterator.Current().MarkOccupiedPages(_otherMap, mapBufferSize);
               }
            }

            // We are only interested in pages NOT used by any other process.
            for (uint32_t i = 0; i < _bufferEntries; i++) {
               _otherMap[i] = ~_otherMap[i];
            }

            StartLogLine(1);
            LogProcess(_parentName);
         }

         void CollectMultiple()
         {
            list<Core::ProcessInfo> processes;
            Core::ProcessInfo::FindByName(_parentName, false, processes);

            StartLogLine(processes.size());

            for (const Core::ProcessInfo& processInfo : processes) {
               uint32_t mapBufferSize = sizeof(_ourMap[0]) * _bufferEntries;
               memset(_ourMap, 0, mapBufferSize);
               memset(_otherMap, 0, mapBufferSize);

               string processName = processInfo.Name() + " (" + std::to_string(processInfo.Id()) + ")";

               _namesLock.Lock();
               if (find(_processNames.begin(), _processNames.end(), processName) == _processNames.end()) {
                  _processNames.push_back(processName);
               }
               _namesLock.Unlock();

               Core::ProcessTree processTree(processInfo.Id());

               processTree.MarkOccupiedPages(_ourMap, mapBufferSize);

               list<Core::ProcessInfo> otherProcesses;
               Core::ProcessInfo::Iterator otherIterator;
               while (otherIterator.Next()) {
                  uint32_t otherId = otherIterator.Current().Id();
                  if (!processTree.ContainsProcess(otherId)) {
                     otherIterator.Current().MarkOccupiedPages(_otherMap, mapBufferSize);
                  }
               }

               // We are only interested in pages NOT used by any other process.
               for (uint32_t i = 0; i < _bufferEntries; i++) {
                  _otherMap[i] = ~_otherMap[i];
               }

               LogProcess(processName);
            }
         }

         void CollectWPEProcess(const string& argument)
         {
            const string processName = "WPEProcess";

            list<Core::ProcessInfo> processes;
            Core::ProcessInfo::FindByName(processName, false, processes);

            vector<std::pair<uint32_t, string> > processIds;
            for (const Core::ProcessInfo& processInfo : processes) {
               std::list<string> commandLine = processInfo.CommandLine();

               bool shouldTrack = false;
               string columnName;

               // Get callsign/classname
               std::list<string>::const_iterator i = std::find(commandLine.cbegin(), commandLine.cend(), argument);
               if (i != commandLine.cend()) {
                  i++;
                  if (i != commandLine.cend()) {
                     if (*i == _parentName) {
                        columnName = _parentName + " (" + std::to_string(processInfo.Id()) + ")";
                        processIds.push_back(std::pair<uint32_t, string>(processInfo.Id(), columnName));
                     }
                  }
               }

               if (!shouldTrack) {
                  continue;
               }

               _namesLock.Lock();
               if (std::find(_processNames.cbegin(), _processNames.cend(), columnName) == _processNames.cend()) {
                  _processNames.push_back(columnName);
               }
               _namesLock.Unlock();
            }

            StartLogLine(processIds.size());

            for (std::pair<uint32_t, string> processDesc : processIds) {
               Core::ProcessTree tree(processDesc.first);

               uint32_t mapBufferSize = sizeof(_ourMap[0]) * _bufferEntries;
               memset(_ourMap, 0, mapBufferSize);
               memset(_otherMap, 0, mapBufferSize);

               tree.MarkOccupiedPages(_ourMap, mapBufferSize);

               list<Core::ProcessInfo> otherProcesses;
               Core::ProcessInfo::Iterator otherIterator;
               while (otherIterator.Next()) {
                  uint32_t otherId = otherIterator.Current().Id();
                  if (!tree.ContainsProcess(otherId)) {
                     otherIterator.Current().MarkOccupiedPages(_otherMap, mapBufferSize);
                  }
               }

               for (uint32_t i = 0; i < _bufferEntries; i++) {
                  _otherMap[i] = ~_otherMap[i];
               }

               LogProcess(processDesc.second);
            }
         }

     protected:
         virtual uint32_t Worker()
         {
            switch(_collectMode) {
               case Config::CollectMode::Single:
                  CollectSingle();
                  break;
               case Config::CollectMode::Multiple: 
                  CollectMultiple();
                  break;
               case Config::CollectMode::Callsign: 
                  CollectWPEProcess("-C");
                  break;
               case Config::CollectMode::ClassName:
                  CollectWPEProcess("-c");
                  break;
               case Config::CollectMode::Invalid:
                  // TODO: ASSERT?
                  break;
            }


            Thread::Block();
            return _interval * 1000;
         }

    private:
         uint32_t CountSetBits(uint32_t pageBuffer[], const uint32_t* mask)
         {
            uint32_t count = 0;

            if (mask == nullptr) {
               for (uint32_t index = 0; index < _bufferEntries; index++) {
                  count += __builtin_popcount(pageBuffer[index]);
               }
            } else {
               for (uint32_t index = 0; index < _bufferEntries; index++) {
                  count += __builtin_popcount(pageBuffer[index] & mask[index]);
               }
            }

            return count;
         }

         void LogProcess(const string& name)
         {
            uint32_t vss = CountSetBits(_ourMap, nullptr);
            uint32_t uss = CountSetBits(_ourMap, _otherMap);

            uint32_t nameSize = name.length();
            fwrite(&nameSize, sizeof(nameSize), 1, _binFile);
            fwrite(name.c_str(), sizeof(name[0]), name.length(), _binFile);
            fwrite(&vss, 1, sizeof(vss), _binFile);
            fwrite(&uss, 1, sizeof(uss), _binFile);
            fflush(_binFile);
         }

         void StartLogLine(uint32_t processCount)
         {
            // TODO: no simple time_t alike in Thunder?
            uint32_t timestamp = static_cast<uint32_t>(Core::Time::Now().Ticks() / 1000 / 1000);

            fwrite(&timestamp, 1, sizeof(timestamp), _binFile);
            fwrite(&processCount, 1, sizeof(processCount), _binFile);
         }

         FILE *_binFile;
         vector<string> _processNames; // Seen process names.
         Core::CriticalSection _namesLock;
         uint32_t * _otherMap; // Buffer used to mark other processes pages.
         uint32_t * _ourMap;   // Buffer for pages used by our process (tree).
         uint32_t _bufferEntries; // Numer of entries in each buffer.
         uint32_t _interval; // Seconds between measurement.
         Config::CollectMode _collectMode; // Collection style.
         string _parentName; // Process/plugin name we are looking for.
      };

  private:
      ResourceMonitorImplementation(const ResourceMonitorImplementation&) = delete;
      ResourceMonitorImplementation& operator=(const ResourceMonitorImplementation&) = delete;

  public:
      ResourceMonitorImplementation()
          : _processThread(nullptr)
          , _binPath(_T("/tmp/resource-log.bin"))
      {
      }

      virtual ~ResourceMonitorImplementation()
      {
         if (_processThread != nullptr) {
            delete _processThread;
            _processThread = nullptr;
         }
      }

      virtual uint32_t Configure(PluginHost::IShell* service) override
      {
         uint32_t result(Core::ERROR_INCOMPLETE_CONFIG);

         ASSERT(service != nullptr);

         Config config;
         config.FromString(service->ConfigLine());

         result = Core::ERROR_NONE;

         _processThread = new ProcessThread(config);
         _processThread->Run();

         return (result);
      }

      string CompileMemoryCsv() override
      {
         // TODO: should we worry about doing this as repsonse to RPC (could take too long?)
         FILE* inFile = fopen(_binPath.c_str(), "rb");
         stringstream output;

         vector<string> processNames;
         _processThread->GetProcessNames(processNames);

         output << _T("time (s)");
         for (const string& processName : processNames) {
            output << _T("\t") << processName << _T(" (VSS)\t") << processName << _T(" (USS)");
         }
         output << endl;

         vector<uint32_t> pageVector(processNames.size() * 2);
         bool seenFirstTimestamp = false;
         uint32_t firstTimestamp = 0;

         while (true) {
            std::fill(pageVector.begin(), pageVector.end(), 0);

            uint32_t timestamp = 0;
            size_t readCount = fread(&timestamp, sizeof(timestamp), 1, inFile);
            if (readCount != 1) {
               break;
            }

            if (!seenFirstTimestamp) {
               firstTimestamp = timestamp;
               seenFirstTimestamp = true;
            }

            uint32_t processCount = 0;
            fread(&processCount, sizeof(processCount), 1, inFile);

            for (uint32_t processIndex = 0; processIndex < processCount; processIndex++) {
               uint32_t nameLength = 0;
               fread(&nameLength, sizeof(nameLength), 1, inFile);
               // TODO: unicode?
               char nameBuffer[nameLength + 1];
               fread(nameBuffer, sizeof(char), nameLength, inFile);
               nameBuffer[nameLength] = '\0';
               string name(nameBuffer);

               vector<string>::const_iterator nameIterator = std::find(processNames.cbegin(), processNames.cend(), name);

               uint32_t vss, uss;
               fread(&vss, sizeof(vss), 1, inFile);
               fread(&uss, sizeof(uss), 1, inFile);
               if (nameIterator == processNames.cend()) {
                   continue;
               }

               int index = nameIterator - processNames.cbegin();

               pageVector[index * 2] = vss;
               pageVector[index * 2 + 1] = uss;
            }

            output << (timestamp - firstTimestamp);
            for (uint32_t pageEntry : pageVector) {
               output << "\t" << pageEntry;
            }
            output << endl;
         }

         fclose(inFile);

         return output.str();
      }

      BEGIN_INTERFACE_MAP(ResourceMonitorImplementation)
      INTERFACE_ENTRY(Exchange::IResourceMonitor)
      END_INTERFACE_MAP

  private:
      ProcessThread* _processThread;
      string _binPath;
   };

   SERVICE_REGISTRATION(ResourceMonitorImplementation, 1, 0);
} /* namespace Plugin */
} // namespace ResourceMonitor

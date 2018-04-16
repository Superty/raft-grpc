#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/util/delimited_message_util.h>
#include "raft.pb.h"
#include "Storage.h"

#include <utility>
#include <fstream>

using std::string;
using raft::LogEntry;
using google::protobuf::io::IstreamInputStream;
using google::protobuf::util::SerializeDelimitedToOstream;
using google::protobuf::util::ParseDelimitedFromZeroCopyStream;

Storage::Storage(const std::string& storageDir) {
  logFile = storageDir + "/" + LOG_FILENAME;
  stateFile = storageDir + "/" + STATE_FILENAME;
}

void Storage::UpdateState(int currentTerm, int votedFor) {
  std::ofstream fout(stateFile, std::ofstream::binary | std::ofstream::trunc);
  fout << currentTerm << ' ' << votedFor << '\n';
}

void Storage::UpdateLog(LogEntryConstIterator cbegin,
                        LogEntryConstIterator cend, bool append) {
  auto flags = std::ofstream::binary;
  if (append) {
    flags |= std::ofstream::app;
  }
  std::ofstream fout(logFile, flags);
  for (auto it = cbegin; it != cend; ++it) {
    SerializeDelimitedToOstream(*it, &fout);
  }
}

void Storage::LoadFromStorage(int* currentTerm, int* votedFor,
                     std::vector<LogEntry>* entries) const {
  std::ifstream stateStream(stateFile, std::ifstream::binary);
  stateStream >> *currentTerm >> *votedFor;

  std::ifstream logStream(logFile, std::ifstream::binary);
  IstreamInputStream logInputStream(&logStream);
  LogEntry entry;
  while (ParseDelimitedFromZeroCopyStream(&entry, &logInputStream, nullptr)) {
    entries->push_back(entry);
  }
}

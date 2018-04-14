#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <random>

#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>
#include "raft.pb.h"

using std::string;
using raft::LogEntry;

static const string LOG_FILENAME = "log", STATE_FILENAME = "state";

using LogEntryConstIterator = std::vector<LogEntry>::const_iterator;

class Storage {
 public:
  Storage(const string& storageDir);
  void UpdateState(int currentTerm, int votedFor);
  void UpdateLog(LogEntryConstIterator cbegin,
                 LogEntryConstIterator cend, bool append);
  void LoadFromStorage(int* currentTerm, int* votedFor,
                       std::vector<LogEntry>* entries);

 private:
  string logFile, stateFile;
};

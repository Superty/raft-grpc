#pragma once
#include <vector>
#include <string>

class RaftServerImpl;
struct LogEntry {
  int term;
  std::string command;
};

class RequestInterface {
public:
  virtual void Initialize(std::vector<std::string> hostList) = 0;
  virtual void AppendEntries(
    int destId,
    int leaderTerm,
    int leaderId,
    int prevLogIndex,
    int prevLogTerm,
    const std::vector<std::string>& entries,
    int leaderCommit,
    int * responseTerm,
    bool * success
  );
  virtual void RequestVote(
    int destId,
    int candidateTerm,
    int candidateId,
    int lastLogIndex,
    int lastLogTerm,
    int * responseTerm,
    bool * voteGranted
  );
};

class ResponseInterface {
public:
  virtual void Initialize(RaftServerImpl* raftServer) = 0;
};

class MachineInterface {
public:
  virtual void Process(std::string command) = 0;
};

class StorageInterface {
public:
  virtual void Initialize(std::string logFile) = 0;
  virtual void InitializeState(
    int * currentTerm,
    int * votedFor,
    std::vector<LogEntry> * log
  ) = 0;
  virtual void Update(
    int currentTerm,
    int votedFor,
    const std::vector<LogEntry>& log
  ) = 0;
};
#pragma once

#include "RaftInterface.h"
#include <memory>
#include <vector>

using std::unique_ptr;
using std::string;

enum class ServerState { Leader, Candidate, Follower };

class RaftServerImpl {
public:
  RaftServerImpl(
    unique_ptr<RequestInterface> o_requestImpl,
    unique_ptr<ResponseInterface> o_responseImpl,
    unique_ptr<MachineInterface> o_snapshot,
    unique_ptr<StorageInterface> o_storageImpl,
    std::vector<std::string> hostList,
    string logFile
  );
  void Run();
  void HandleAppendEntries(
    int leaderTerm,
    int leaderId,
    int prevLogIndex,
    int prevLogTerm,
    const std::vector<LogEntry>& entries,
    int leaderCommit,
    int * responseTerm,
    bool * success
  );
  void HandleRequestVote(
    int candidateTerm,
    int candidateId,
    int lastLogIndex,
    int lastLogTerm,
    int * responseTerm,
    bool * voteGranted
  );
  void BecomeLeader();
  void BecomeCandidate();
  void BecomeFollower();

private:
  unique_ptr<RequestInterface> requestImpl;
  unique_ptr<ResponseInterface> responseImpl;
  unique_ptr<MachineInterface> currentState;
  unique_ptr<StorageInterface> storageImpl;

  // persistent state
  int32_t currentTerm, votedFor;
  std::vector<LogEntry> log;

  // volatile state (common)
  int commitIndex, lastApplied, currentLeader;
  ServerState serverState;

  // volatile state (valid if this object is leader)
  std::vector<int> nextIndex, matchIndex;
};

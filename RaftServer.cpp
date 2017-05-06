#include "RaftServer.h"

#include <utility>
#include <fstream>

using std::string;
using std::unique_ptr;

RaftServerImpl::RaftServerImpl(
  unique_ptr<RequestInterface> o_requestImpl,
  unique_ptr<ResponseInterface> o_responseImpl,
  unique_ptr<MachineInterface> o_snapshot,
  unique_ptr<StorageInterface> o_storageImpl,
  std::vector<std::string> hostList,
  string logFile
):
requestImpl(std::move(o_requestImpl)),
responseImpl(std::move(o_responseImpl)),
currentState(std::move(o_snapshot)),
storageImpl(std::move(o_storageImpl)),
commitIndex(-1),
lastApplied(-1),
currentLeader(-1),
serverState(ServerState::Follower)
{
  requestImpl->Initialize(hostList);
  responseImpl->Initialize(this);
  storageImpl->Initialize(logFile);
  storageImpl->InitializeState(&currentTerm, &votedFor, &log);

  for(LogEntry entry: log) {
    currentState->Process(entry.command);
  }

  Run();
}

void RaftServerImpl::HandleAppendEntries(
  int leaderTerm,
  int leaderId,
  int prevLogIndex,
  int prevLogTerm,
  const std::vector<LogEntry>& entries,
  int leaderCommit,
  int * responseTerm,
  bool * success
) {
  bool mustUpdateStorage = false;
  *responseTerm = currentTerm;

  if (leaderTerm < currentTerm) {
    *success = false;
    return;
  }

  if (leaderTerm > currentTerm) {
    currentTerm = leaderTerm;
    BecomeFollower();
    mustUpdateStorage = true;
  }

  if (prevLogIndex >= log.size()
           || log[prevLogIndex].term != prevLogTerm) {
    *success = false;
    return;
  }

  uint curLogIndex = prevLogIndex + 1;
  bool mustAppend = false;
  for (LogEntry entry: entries) {
    if (mustAppend) {
      log.push_back(entry);
      mustUpdateStorage = true;
    }
    else if (curLogIndex >= log.size()) {
      mustAppend = true;
    }
    else if (log[curLogIndex].term != entry.term) {
      log.resize(curLogIndex);
      mustUpdateStorage = true;
      mustAppend = true;
    }
    curLogIndex++;
  }

  if(leaderCommit > commitIndex) {
    commitIndex = std::min(leaderCommit, int(log.size() - 1));
  }

  if(mustUpdateStorage) {
    storageImpl->Update(currentTerm, votedFor, log);
  }

  currentLeader = leaderId;
  *success = true;
}

void RaftServerImpl::HandleRequestVote(
  int candidateTerm,
  int candidateId,
  int lastLogIndex,
  int lastLogTerm,
  int * responseTerm,
  bool * voteGranted
) {
  *responseTerm = currentTerm;

  if(candidateTerm < currentTerm) {
    *voteGranted = false;
    return;
  }

  if (candidateTerm > currentTerm) {
    currentTerm = candidateTerm;
    BecomeFollower();
  }

  if(votedFor != -1 && votedFor != candidateId) {
    *voteGranted = false;
    return;
  }

  if (log.back().term > lastLogTerm ||
     (log.back().term == lastLogTerm && log.size() - 1 > lastLogIndex)) {
    *voteGranted = false;
    return;
  }

  votedFor = candidateId;
  storageImpl->Update(currentTerm, votedFor, log);
  *voteGranted = true;
}
